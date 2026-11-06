// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// One LZX stream decoded block by block with libmspack's lzxd (LGPL-2.1, the copy pinned in tools/XenonRecomp's
// thirdparty, the decoder XenonRecomp and Xenia (cpu/lzx.cc) use). The stream's frames are 32 KB of output and
// 16-bit aligned in the input, and each XMemCompress block holds whole frames, so at a block's end the bit reader's
// leftover bits are alignment padding: they are dropped before the next block (lzxd would otherwise pad the
// exhausted input with two zero bytes, read into the next frame). Before each block the stream's output length is
// set to the bytes so far plus this block's, so a short final frame decodes to its size (lzxd_set_output_length), and
// each block is requested as all but its last byte, then that byte (see Decompress). The window comes from the
// guest's configuration (kernel/imports_ldi.cpp).
#include "ldi_mspack.h"
extern "C" {
#include <system.h>
#include <lzx.h>
}
#include <cstdlib>
#include <cstring>

namespace runtime::ldi {
namespace {
struct MemoryFile
{
    const uint8_t* data = nullptr;
    uint8_t* out = nullptr;
    uint32_t size = 0, position = 0;
};

int Read(mspack_file* file, void* buffer, int bytes)
{
    auto* f = reinterpret_cast<MemoryFile*>(file);
    const int n = int(std::min<uint32_t>(uint32_t(bytes), f->size - f->position));
    std::memcpy(buffer, f->data + f->position, size_t(n));
    f->position += uint32_t(n);
    return n;
}

int Write(mspack_file* file, void* buffer, int bytes)
{
    auto* f = reinterpret_cast<MemoryFile*>(file);
    if (f->position + uint32_t(bytes) > f->size)
        return -1;
    std::memcpy(f->out + f->position, buffer, size_t(bytes));
    f->position += uint32_t(bytes);
    return bytes;
}

void* Alloc(mspack_system*, size_t bytes) { return std::calloc(1, bytes); }
void Free(void* p) { std::free(p); }
void Copy(void* src, void* dst, size_t bytes) { std::memmove(dst, src, bytes); }

class MspackDecoder final : public Decoder
{
public:
    explicit MspackDecoder(uint32_t windowBits)
    {
        system_.read = Read, system_.write = Write, system_.alloc = Alloc, system_.free = Free, system_.copy = Copy;
        stream_ = lzxd_init(&system_, reinterpret_cast<mspack_file*>(&input_), reinterpret_cast<mspack_file*>(&output_), int(windowBits), 0,
                            0x8000, 0, 0);
    }
    ~MspackDecoder() override
    {
        if (stream_)
            lzxd_free(stream_);
    }
    bool ok() const { return stream_ != nullptr; }

    bool Decompress(const uint8_t* src, uint32_t srcBytes, uint8_t* dst, uint32_t dstBytes, std::string& error) override
    {
        input_ = {src, nullptr, srcBytes, 0};
        output_ = {nullptr, dst, dstBytes, 0};
        // A new block: no bits carried over from the previous one (see the file comment).
        stream_->i_ptr = stream_->i_end = stream_->inbuf;
        stream_->bit_buffer = 0, stream_->bits_left = 0, stream_->input_end = 0;
        lzxd_set_output_length(stream_, stream_->offset + dstBytes);
        // lzxd decodes frames up to (offset + bytes) / 32 KB + 1: asking for a block that ends on a 32 KB boundary
        // would run one more (empty) frame and push its frame count past the stream's. Asking for all but the last
        // byte decodes exactly this block's frame; the last byte then comes from lzxd's frame buffer, with no decoding.
        int r = dstBytes > 1 ? lzxd_decompress(stream_, dstBytes - 1) : MSPACK_ERR_OK;
        if (r == MSPACK_ERR_OK)
            r = lzxd_decompress(stream_, 1);
        if (r != MSPACK_ERR_OK || output_.position != dstBytes)
        {
            error = "lzxd error " + std::to_string(r) + ", " + std::to_string(output_.position) + " of " + std::to_string(dstBytes) + " bytes";
            return false;
        }
        return true;
    }

private:
    mspack_system system_{};
    MemoryFile input_, output_;
    lzxd_stream* stream_ = nullptr;
};
}  // namespace

Factory MspackDecoderFactory()
{
    return [](uint32_t windowBits, std::string& error) -> std::unique_ptr<Decoder> {
        auto d = std::make_unique<MspackDecoder>(windowBits);
        if (!d->ok())
            return error = "lzxd_init failed", nullptr;
        return d;
    };
}

}  // namespace runtime::ldi
