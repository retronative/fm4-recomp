// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Rtl string imports. Semantics: Xenia xenia-canary 67f16c4e315f,
// src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc; X_ANSI_STRING = {be16 length, be16 maximum length, be32 buffer}.
#include "imports.h"
#include <cstdio>
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

GUEST_IMPORT(RtlMultiByteToUnicodeN, "REFERENCE_DERIVED: Xenia xboxkrnl_rtl.cc:357-377 (kSketchy); ASCII only")
{
    // (wchar dest*, dest length in bytes, written*, source*, source length): min(dest length / 2, source length)
    // characters, each byte widened to a big-endian UTF-16 unit; *written (if given) = characters x 2; 0. Xenia widens
    // every byte (Latin-1) and marks itself sketchy: for a byte of 0x80 or more the result depends on the ANSI code
    // page (UNRESOLVED), so such a byte stops (7); ASCII is exact.
    const uint32_t dest = ctx.r3.u32, destLength = ctx.r4.u32, written = ctx.r5.u32, source = ctx.r6.u32, sourceLength = ctx.r7.u32;
    const uint32_t count = destLength / 2 < sourceLength ? destLength / 2 : sourceLength;
    for (uint32_t i = 0; i < count; i++)
        if (base[source + i] >= 0x80)
            Stop(StopCode::Unimplemented, "RtlMultiByteToUnicodeN: byte %02X at %u needs the ANSI code page (not modelled)", base[source + i], i);
    for (uint32_t i = 0; i < count; i++)
        base[dest + 2 * i] = 0, base[dest + 2 * i + 1] = base[source + i];
    if (written)
        runtime::Store32(base, written, count * 2);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(EtxProducerRegister, "APPROXIMATE: Xenia lists the export only (xboxkrnl_table.inc); no event-trace consumer exists; FM4 ignores the result (VERIFIED)")
{
    // (descriptor*, ?, ?, ?, ?, ?): registers an event-trace (ETX) producer, which feeds a diagnostics consumer. No
    // consumer exists in this runtime, so nothing is registered or written: 0, logged once. FM4's caller does not read
    // the result (VERIFIED in our translation). Semantics UNKNOWN; logging through the producer stays a strict stop.
    static bool logged = false;
    if (!logged)
        logged = true, std::fprintf(stderr, "[runtime] EtxProducerRegister(%08X): no trace consumer, nothing registered\n", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(EtxProducerUnregister, "APPROXIMATE: the counterpart of EtxProducerRegister, which registers nothing here; Xenia lists the export only")
{
    // (descriptor*, ...): unregisters an event-trace producer. Nothing was registered (no trace consumer): 0.
    ctx.r3.u64 = 0;
}
