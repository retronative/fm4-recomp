// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// File-system imports. Semantics: Xenia xenia-canary 67f16c4e315f,
// src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc.
#include "imports.h"

GUEST_IMPORT(FscSetCacheElementCount, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:669 FscSetCacheElementCount (a stub: success)")
{
    // (0, count): sizes the kernel's file-system cache. The runtime has no such cache, so the setting has
    // no guest-visible effect; Xenia also returns success. Policy: accepted only
    // in the shape FM4 uses (first argument 0, a count of 1..65536 elements); anything else stops (7),
    // because its meaning is undocumented (Xenia's own comment: "unk_1 looks like a count?").
    (void)base;
    const uint32_t first = ctx.r3.u32, count = ctx.r4.u32;
    if (first != 0 || count == 0 || count > 0x10000)
        Stop(StopCode::Unimplemented, "FscSetCacheElementCount(%u, %u): arguments outside the modelled shape", first, count);
    ctx.r3.u64 = 0;  // X_STATUS_SUCCESS
}
