// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Rtl string imports. Semantics: Xenia xenia-canary 67f16c4e315f,
// src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc; X_ANSI_STRING = {be16 length, be16 maximum length, be32 buffer}.
#include "imports.h"
#include <cstring>

GUEST_IMPORT(RtlInitAnsiString, "REFERENCE_DERIVED: Xenia xboxkrnl_rtl.cc:224 RtlInitAnsiString")
{
    // (string*, source*): length = strlen(source), maximum = length + 1, buffer = source; a null source
    // gives 0, 0, null. A source of 65535 characters or more does not fit the 16-bit fields: stop (7).
    const uint32_t string = ctx.r3.u32, source = ctx.r4.u32;
    size_t length = 0;
    if (source != 0)
    {
        length = strnlen(reinterpret_cast<const char*>(base + source), 0xFFFF);
        if (length >= 0xFFFF)
            Stop(StopCode::Unimplemented, "RtlInitAnsiString: a source of 65535 characters or more at %08X", source);
    }
    const uint32_t maximum = source ? uint32_t(length) + 1 : 0;
    runtime::Store32(base, string, uint32_t(length) << 16 | maximum);
    runtime::Store32(base, string + 4, source);
}
