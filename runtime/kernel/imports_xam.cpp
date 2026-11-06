// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XAM imports, added as runs reach them (FM4's order). Values and semantics: Xenia xenia-canary 67f16c4e315f
// (src/xenia/kernel/xam/), cited per import. ForzaRecomp's
// console_config.h (its choices for region, language, system version) is not taken.
#include "imports.h"

GUEST_IMPORT(XGetAVPack, "REFERENCE_DERIVED: Xenia xam_info.cc:205 XGetAVPack returns cvar avpack, default 8 (xam_info.cc:32)")
{
    // The AV pack in use. Xenia notes games use it as a PAL check and accept 3, 4, 6 or 8; 8 is its default.
    ctx.r3.u64 = 8;
}

// Launch data: the runtime always starts the title cold (no other title launched it with data), so
// there is none (Xenia: loader_data.launch_data_present false).
GUEST_IMPORT(XamLoaderGetLaunchDataSize, "REFERENCE_DERIVED: Xenia xam_info.cc:294 XamLoaderGetLaunchDataSize (no launch data)")
{
    // (size*): ERROR_INVALID_PARAMETER (0x57) without a pointer; else size 0 and ERROR_NOT_FOUND (0x490).
    if (ctx.r3.u32 == 0)
    {
        ctx.r3.u64 = 0x57;
        return;
    }
    runtime::Store32(base, ctx.r3.u32, 0);
    ctx.r3.u64 = 0x490;
}

GUEST_IMPORT(XamLoaderGetLaunchData, "REFERENCE_DERIVED: Xenia xam_info.cc:311 XamLoaderGetLaunchData (no launch data)")
{
    // (buffer*, size): nothing copied, ERROR_NOT_FOUND (0x490).
    (void)base;
    ctx.r3.u64 = 0x490;
}

GUEST_IMPORT(XGetGameRegion, "REFERENCE_DERIVED: Xenia xam_info.cc:217 xeXGetGameRegion (table by user country)")
{
    // Xenia maps the user's country to a region through its table; the runtime's country is 103 (US, the
    // XConfig policy in imports_xconfig.cpp), and table[103] (xam_info.cc:231, the 8th value of the 13th
    // row) is 0x00FF (North America).
    (void)base;
    ctx.r3.u64 = 0x00FF;
}
