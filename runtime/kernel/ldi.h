// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// LZX decompression (the kernel's LDI interface: LDICreateDecompression, LDIDecompress, LDIDestroyDecompression;
// imports_ldi.cpp). One context decodes one LZX stream given block by block, the XMemCompress native framing FM4's
// zips use (method 21): each LDIDecompress gets one block's compressed bytes and the size it must produce (32 KB, or
// less for the last block), and the window carries over from block to block. The decoder: libmspack's lzxd
// (runtime/ldi_mspack.cpp); tests use fakes.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace runtime::ldi {

class Decoder
{
public:
    virtual ~Decoder() = default;
    // Decodes one block: `srcBytes` of compressed data into exactly `dstBytes` bytes; false and `error` on failure.
    virtual bool Decompress(const uint8_t* src, uint32_t srcBytes, uint8_t* dst, uint32_t dstBytes, std::string& error) = 0;
};
using Factory = std::function<std::unique_ptr<Decoder>(uint32_t windowBits, std::string& error)>;

// Set by main (the mspack decoder) or a test. Without one, LDICreateDecompression stops.
void SetFactory(Factory factory);

}  // namespace runtime::ldi
