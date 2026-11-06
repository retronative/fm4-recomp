// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Force-included into the translated code (recomp/CMakeLists.txt): the accesses the translator marks as device
// accesses go through the runtime, which models the XMA decoder's registers (runtime/kernel/xma.h) and treats every
// other address as plain guest memory. Marked are 32-bit stores followed by eieio (upstream XenonRecomp,
// PPC_MM_STORE_U32) and byte-reversed word loads (lwbrx, patch 0033, PPC_MM_LOAD_U32). Both keep the plain
// macros' meaning: a load returns the word as PPC_LOAD_U32 would (big-endian), a store takes the value as
// PPC_STORE_U32 would. Same approach as ForzaRecomp recomp (PpcMmioLoad32/PpcMmioStore32), re-written.
#pragma once
#include <cstdint>

uint32_t PpcMmioLoad32(uint8_t* base, uint32_t address);
void PpcMmioStore32(uint8_t* base, uint32_t address, uint32_t value);

#define PPC_MM_LOAD_U32(x) PpcMmioLoad32(base, uint32_t(x))
#define PPC_MM_STORE_U32(x, y) PpcMmioStore32(base, uint32_t(x), uint32_t(y))
