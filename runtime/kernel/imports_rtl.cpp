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

namespace {

// NTSTATUS -> Win32 error for the statuses this runtime returns, each from Xenia's table
// (xboxkrnl_error.cc, the line of its entry in parentheses). Demand-driven: a status not listed stops.
struct StatusError { uint32_t status, error; };
constexpr StatusError kStatusErrors[] = {
    {0x00000103, 0x3E5},  // PENDING -> ERROR_IO_PENDING (:34)
    {0xC0000003, 0x57},  // INVALID_INFO_CLASS -> ERROR_INVALID_PARAMETER (:171)
    {0xC0000004, 0x18},  // INFO_LENGTH_MISMATCH -> ERROR_BAD_LENGTH (:172)
    {0xC0000008, 0x06},  // INVALID_HANDLE -> ERROR_INVALID_HANDLE (:176)
    {0xC000000D, 0x57},  // INVALID_PARAMETER -> ERROR_INVALID_PARAMETER (:181)
    {0xC000000F, 0x02},  // NO_SUCH_FILE -> ERROR_FILE_NOT_FOUND (:183)
    {0xC0000011, 0x26},  // END_OF_FILE -> ERROR_HANDLE_EOF (:185)
    {0xC0000024, 0x06},  // OBJECT_TYPE_MISMATCH -> ERROR_INVALID_HANDLE (:204)
    {0xC0000033, 0x7B},  // OBJECT_NAME_INVALID -> ERROR_INVALID_NAME (:219)
    {0xC0000034, 0x02},  // OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND (:220)
    {0xC00000BA, 0x05},  // FILE_IS_A_DIRECTORY -> ERROR_ACCESS_DENIED (:354)
};

}  // namespace

GUEST_IMPORT(RtlNtStatusToDosError, "REFERENCE_DERIVED: Xenia xboxkrnl_error.cc:974 xeRtlNtStatusToDosError (rules and table)")
{
    // Xenia's rules: 0 and customer codes (bit 29) unchanged; 0x8007xxxx (a Win32 HRESULT) -> xxxx; a
    // 0xDxxxxxxx status is looked up with bits 28-29 cleared; the table; 0xC001xxxx -> xxxx. A status
    // the table here does not list stops (7) instead of Xenia's fallback 317 (ERROR_MR_MID_NOT_FOUND).
    (void)base;
    uint32_t status = ctx.r3.u32;
    if (status == 0 || (status & 0x20000000))
        return (void)(ctx.r3.u64 = status);
    if ((status >> 16) == 0x8007)
        return (void)(ctx.r3.u64 = status & 0xFFFF);
    if ((status & 0xF0000000) == 0xD0000000)
        status &= ~0x30000000u;
    for (const StatusError& e : kStatusErrors)
        if (e.status == status)
            return (void)(ctx.r3.u64 = e.error);
    if ((status >> 16) == 0xC001)
        return (void)(ctx.r3.u64 = status & 0xFFFF);
    Stop(StopCode::Unimplemented, "RtlNtStatusToDosError(%08X): a status the runtime's table does not list", ctx.r3.u32);
}

GUEST_IMPORT(RtlFillMemoryUlong, "REFERENCE_DERIVED: Xenia xboxkrnl_rtl.cc:78 RtlFillMemoryUlong")
{
    // (destination*, length in bytes, pattern): length / 4 words of the pattern, big-endian.
    const uint32_t destination = ctx.r3.u32, count = ctx.r4.u32 >> 2, pattern = ctx.r5.u32;
    for (uint32_t i = 0; i < count; i++)
        runtime::Store32(base, destination + 4 * i, pattern);
}
