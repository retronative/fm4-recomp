// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The kernel's LZX decompression interface (ldi.h). Neither Xenia (xboxkrnl_table.inc lists LDICreateDecompression,
// LDIDecompress, LDIDestroyDecompression without code) nor ReCarbon or ForzaRecomp implements it; the argument roles
// are INFERRED from the LDI interface of Microsoft's cabinet SDK (LDICreateDecompression(pcbDataBlockMax,
// pvConfiguration, pfnma, pfnmf, pcbSrcBufferMin, phandle, ...), LDIDecompress(handle, pbSrc, cbSrc, pbDst,
// pcbDecompressed), LZXDECOMPRESS {WindowSize, CpuType}, 0 = MDI_ERROR_NO_ERROR) and VERIFIED against FM4's only
// caller in our translation: the handle is written through the 7th argument (the value later given to
// LDIDecompress and LDIDestroyDecompression), the 6th (a size output) is never read back, a result of 0 is success,
// and each decompression must report exactly the size the caller asked for. Correctness of the decoded bytes is
// checked against the CRC-32 of FM4's zip entries (tests/ldi/run_crosscheck.sh). A decoder error stops (7): no
// output is invented.
#include "imports.h"
#include "ldi.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>

namespace {
std::mutex ldiLock;
runtime::ldi::Factory factory;
std::map<uint32_t, std::unique_ptr<runtime::ldi::Decoder>> contexts;
uint32_t nextHandle = 1;
}  // namespace

void runtime::ldi::SetFactory(Factory f)
{
    std::lock_guard guard(ldiLock);
    factory = std::move(f);
}

GUEST_IMPORT(LDICreateDecompression, "INFERRED: cabinet SDK LDI interface; VERIFIED against FM4's caller (see file comment)")
{
    // (block max*, configuration* {window size, second word}, alloc, free, ?, size out*, handle*): a decoder for an LZX
    // window of the first word's size (a power of two, 2^15-2^21); *handle = an opaque non-zero value; 0. OBSERVED:
    // FM4's first context, {0x80000, 0}, decodes its data only with a 512 KB window (2^19; the local capture ldi_fail_block (not distributed)
    // tried at 2^15-2^21); its zips (method 21) are 128 KB-window streams (2^17; tests/ldi). The second word is 0 in
    // every call seen (its meaning, the cabinet SDK's CPU type, is INFERRED); another value, allocation callbacks, or no
    // handle pointer stops (7). The size output is left untouched (FM4 never reads it).
    // FM4 Recomp: SpeechFacade_default.xex creates {0x8000, 1} with its own allocation callbacks.
    // The second word 1 is accepted beside 0: as the cabinet SDK's CPU type (286/386) it picks the decoder's code path, not its
    // output; an LZX stream's own header carries what decoding depends on (window, E8 translation), and every block is still
    // decoded by the same validating decoder (INFERRED). The callbacks are accepted and not called (APPROXIMATE): the decoder's
    // state lives in host memory, so the guest allocator is not used, which a caller sees only if it counts its own allocations.
    const uint32_t config = ctx.r4.u32, alloc = ctx.r5.u32, free = ctx.r6.u32, handleOut = ctx.r9.u32;
    const uint32_t window = runtime::Load32(base, config), second = runtime::Load32(base, config + 4);
    uint32_t bits = 15;
    while (bits <= 21 && (1u << bits) != window)
        bits++;
    if (bits > 21 || second > 1 || (alloc == 0) != (free == 0) || handleOut == 0)
        Stop(StopCode::Unimplemented, "LDICreateDecompression(configuration {%08X, %08X}, alloc %08X, free %08X, handle* %08X) is not modelled",
             window, second, alloc, free, handleOut);
    std::lock_guard guard(ldiLock);
    if (!factory)
        Stop(StopCode::Setup, "LDICreateDecompression: no LZX decoder is built in");
    std::string error;
    auto decoder = factory(bits, error);
    if (!decoder)
        Stop(StopCode::Unimplemented, "LDICreateDecompression: %s", error.c_str());
    const uint32_t handle = nextHandle++;
    contexts[handle] = std::move(decoder);
    runtime::Store32(base, handleOut, handle);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(LDIDecompress, "INFERRED: cabinet SDK LDIDecompress; VERIFIED against FM4's caller")
{
    // (handle, source*, source bytes, destination*, size* (in: the size to produce, at most 32 KB; out: produced)):
    // decodes one block of the context's LZX stream; 0. An unknown handle or a decoder error stops (7).
    const uint32_t handle = ctx.r3.u32, src = ctx.r4.u32, srcBytes = ctx.r5.u32, dst = ctx.r6.u32, sizeAt = ctx.r7.u32;
    const uint32_t want = runtime::Load32(base, sizeAt);
    std::lock_guard guard(ldiLock);
    const auto it = contexts.find(handle);
    if (it == contexts.end() || want == 0 || want > 0x8000)
        Stop(StopCode::Unimplemented, "LDIDecompress(handle %X, %u bytes into %u) is not modelled", handle, srcBytes, want);
    std::string error;
    if (!it->second->Decompress(base + src, srcBytes, base + dst, want, error))
    {
        // Diagnostics (opt-in): the failing block, for offline study (game data: kept local, never committed).
        if (const char* dump = std::getenv("FM4_LDI_DUMP"))
            if (FILE* f = std::fopen(dump, "wb"))
                std::fwrite(base + src, 1, srcBytes, f), std::fclose(f);
        Stop(StopCode::Unimplemented, "LDIDecompress(handle %X, %u bytes into %u): %s", handle, srcBytes, want, error.c_str());
    }
    runtime::Store32(base, sizeAt, want);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(LDIDestroyDecompression, "INFERRED: cabinet SDK LDIDestroyDecompression; VERIFIED against FM4's caller")
{
    // (handle): frees the context; 0. An unknown handle stops (7).
    std::lock_guard guard(ldiLock);
    if (contexts.erase(ctx.r3.u32) == 0)
        Stop(StopCode::Unimplemented, "LDIDestroyDecompression(%X): no such context", ctx.r3.u32);
    ctx.r3.u64 = 0;
}
