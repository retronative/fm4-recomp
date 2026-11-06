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
