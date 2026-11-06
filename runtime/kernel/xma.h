// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The XMA decoder: its register block (guest 7FEA0000-7FEAFFFF, register r at 7FEA0000 + 4r; Xenia xenia-canary
// 67f16c4e315f apu/xma_decoder.cc:114, xma_register_table.inc), the kernel's context array and the two imports FM4
// uses (XMACreateContext, XMAReleaseContext; xboxkrnl_audio_xma.cc). FM4 drives the rest through the registers,
// reached through the translator's device-access hooks (recomp/mmio.h). Modelled, after Xenia's
// XmaDecoder::ReadRegister/WriteRegister (xma_decoder.cc:240-360):
//   - 600 ContextArrayAddress (read): the physical address of 320 contexts of 64 bytes (xma_decoder.h:83,
//     XmaDecoder::Setup), allocated in the 64 KB physical view at the first need;
//   - 601 (write 0x02000000 / 0x03000000 as the guest stores them): Xenia "written ... around a lock operation",
//     ignored there and here (meaning UNKNOWN);
//   - 606 CurrentContextIndex (read): 0. FM4's only reader (its context-lock wait, VERIFIED in our translation; address in reference/kernel/audio.md) XORs it with
//     0x200 and waits while that equals one of its context ids: bit 9 set means "processing that context".
//     Decoding runs inside the kick here, so no context is ever in progress when the guest reads (Xenia returns a
//     rotating counter instead, a stated hack);
//   - 650-659 Kick, 690-699 Lock, 6A0-6A9 Clear (write; bit i of register base+g is context 32g+i, the device's
//     little-endian word): a kick runs one decoding pass of each context at once (apu::XmaPass in xma_pass.h;
//     APPROXIMATE: the hardware decodes asynchronously), a lock has nothing in progress to stop, a clear resets
//     the context's buffers and offsets (Xenia XmaContext::Clear).
// Anything else (another register or value, a context that is not allocated, a pass the model cannot do: loops,
// completion interrupts, decoder errors) stops with its reason. Nothing is written that the decoder did not produce.
#pragma once
#include "xma_pass.h"
#include <cstdint>

namespace runtime::xma {

constexpr uint32_t kRegisterBase = 0x7FEA0000, kRegisterBytes = 0x10000;
constexpr uint32_t kContextCount = 320, kContextBytes = 64;

// The decoder kicks use: FFmpeg's (runtime/xma_ffmpeg.cpp, set by main) or a test's. Without one, a kick with
// input to decode stops.
void SetDecoderFactory(apu::XmaDecoderFactory factory);

// Register accesses (recomp/mmio.h's hooks for addresses in the block): `value` and the result as PPC_STORE_U32
// takes and PPC_LOAD_U32 returns them.
uint32_t LoadRegister(uint8_t* base, uint32_t address);
void StoreRegister(uint8_t* base, uint32_t address, uint32_t value);

// The guest address of a free context (lowest first, zeroed), marked allocated; 0 when all are taken.
uint32_t CreateContext(uint8_t* base);
// Zeroes and frees an allocated context; stops (7) for anything else.
void ReleaseContext(uint8_t* base, uint32_t context);

struct Counters
{
    uint32_t allocated = 0, bracketWrites = 0, lastBracket = 0, kicks = 0, idleKicks = 0, locks = 0, clears = 0, passes = 0;
    uint64_t framesDecoded = 0, framesWritten = 0;
};
Counters GetCounters();
// One "[xma]" line (the deadline report, and every 2 s of audio output from kernel/audio.cpp).
void Report(const char* when);

}  // namespace runtime::xma
