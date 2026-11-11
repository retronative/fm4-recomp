// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Rtl string imports. Semantics: Xenia xenia-canary 67f16c4e315f,
// src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc; X_ANSI_STRING = {be16 length, be16 maximum length, be32 buffer}.
#include "imports.h"
#include <algorithm>
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
    {1u << 31 | 0x6, 0x12},  // NO_MORE_FILES (0x8000_0006) -> ERROR_NO_MORE_FILES (:76)
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

// FM4 Recomp: reached by SpeechFacade_default.xex.
GUEST_IMPORT(RtlCompareMemoryUlong, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_rtl.cc:58-75 (bytes that equal the pattern, a word at a time over length & ~3)")
{
    // (source*, length, pattern): the number of bytes from the start that equal the pattern, compared one big-endian word at a
    // time over the length rounded down to a multiple of 4; stops at the first word that differs.
    const uint32_t source = ctx.r3.u32, length = ctx.r4.u32 & ~3u, pattern = ctx.r5.u32;
    uint32_t compared = 0;
    while (compared < length && runtime::Load32(base, source + compared) == pattern)
        compared += 4;
    ctx.r3.u64 = compared;
}

// FM4 Recomp: the Rtl string and time routines SpeechFacade_default.xex imports (it reached
// RtlUnicodeToMultiByteN; the rest of the family is implemented with it). Character conversions are exact for ASCII; a unit or byte
// of 0x80 or more needs the console's code page or case tables (UNRESOLVED), so it stops (7), as RtlMultiByteToUnicodeN above.
namespace {
// X_ANSI_STRING / X_UNICODE_STRING (Xenia xbox.h): length u16 +0 (bytes), maximum length u16 +2, buffer pointer u32 +4.
void SetCountedString(uint8_t* base, uint32_t string, uint16_t length, uint16_t maximum, uint32_t pointer)
{
    runtime::Store16(base, string, length), runtime::Store16(base, string + 2, maximum), runtime::Store32(base, string + 4, pointer);
}
uint16_t AsciiUnit(uint16_t unit, const char* import)
{
    if (unit >= 0x80)
        Stop(StopCode::Unimplemented, "%s: character %04X needs the console's code page or case tables (not modelled)", import, unit);
    return unit;
}
// Days since 1970-01-01 <-> civil date, proleptic Gregorian (H. Hinnant's public-domain algorithms, re-derived), as Xenia's date library.
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = int64_t(yoe) + era * 400 + (m <= 2);
}
constexpr int64_t kTicksPerDay = 864000000000LL;   // 100 ns units
constexpr int64_t kDays1601To1970 = 134774;        // FILETIME epoch to the Unix epoch
}  // namespace

GUEST_IMPORT(RtlUnicodeToMultiByteN, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_rtl.cc:381-401 (kSketchy); ASCII only")
{
    // (dest*, dest length in bytes, written*, wchar source*, source length in bytes): min(source length / 2, dest length) units,
    // each big-endian UTF-16 unit narrowed to a byte; *written (if given) = that count; 0. Xenia writes '?' for 256 and above and
    // the low byte for 0x80-0xFF (code-page dependent): those stop here (ASCII is exact).
    const uint32_t dest = ctx.r3.u32, destLength = ctx.r4.u32, written = ctx.r5.u32, source = ctx.r6.u32, sourceLength = ctx.r7.u32;
    const uint32_t count = sourceLength / 2 < destLength ? sourceLength / 2 : destLength;
    for (uint32_t i = 0; i < count; i++)
        (void)AsciiUnit(runtime::Load16(base, source + 2 * i), "RtlUnicodeToMultiByteN");
    for (uint32_t i = 0; i < count; i++)
        base[dest + i] = uint8_t(runtime::Load16(base, source + 2 * i));
    if (written)
        runtime::Store32(base, written, count);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(RtlInitUnicodeString, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_rtl.cc:254-265")
{
    // (UNICODE_STRING*, wchar source*): length = units x 2, maximum = (units + 1) x 2, the buffer = the source; a null source
    // clears the string. Returns the destination (Xenia).
    const uint32_t dest = ctx.r3.u32, source = ctx.r4.u32;
    if (source)
    {
        uint32_t units = 0;
        while (runtime::Load16(base, source + 2 * units) != 0)
            units++;
        SetCountedString(base, dest, uint16_t(units * 2), uint16_t((units + 1) * 2), source);
    }
    else
        SetCountedString(base, dest, 0, 0, 0);
    ctx.r3.u64 = dest;
}

GUEST_IMPORT(RtlCopyString, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_rtl.cc:278-293")
{
    // (ANSI_STRING dest*, ANSI_STRING source*): min(dest maximum, source length) bytes copied into the destination's buffer, its
    // length set to that; a null source sets the length to 0 (Xenia).
    const uint32_t dest = ctx.r3.u32, source = ctx.r4.u32;
    if (!source)
        return runtime::Store16(base, dest, 0);
    const uint16_t length = std::min(runtime::Load16(base, dest + 2), runtime::Load16(base, source));
    if (length)
        std::memmove(base + runtime::Load32(base, dest + 4), base + runtime::Load32(base, source + 4), length);
    runtime::Store16(base, dest, length);
}

GUEST_IMPORT(RtlUnicodeStringToAnsiString, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_rtl.cc:312-355 (no allocation); ASCII only")
{
    // (ANSI_STRING dest*, UNICODE_STRING source*, allocate): the source's characters as ASCII bytes into the destination's buffer,
    // terminated; when they do not fit, maximum - 1 bytes and a terminator, STATUS_BUFFER_OVERFLOW (Xenia, which leaves
    // the destination length unchanged). Allocating the destination needs the system heap's allocation, which pairs with
    // RtlFreeAnsiString: not modelled, it stops. Over 0xFFFE characters: STATUS_INVALID_PARAMETER_2 (Xenia).
    const uint32_t dest = ctx.r3.u32, source = ctx.r4.u32, allocate = ctx.r5.u32 & 0xFF;
    if (allocate)
        Stop(StopCode::Unimplemented, "RtlUnicodeStringToAnsiString with AllocateDestinationString is not implemented (lr=%08X)", uint32_t(ctx.lr));
    const uint32_t units = runtime::Load16(base, source) / 2, text = runtime::Load32(base, source + 4);
    if (units > 0xFFFE)
        return (void)(ctx.r3.u64 = 0xC00000F0);  // STATUS_INVALID_PARAMETER_2
    const uint32_t capacity = runtime::Load16(base, dest + 2), buffer = runtime::Load32(base, dest + 4);
    if (capacity == 0)
        Stop(StopCode::Unimplemented, "RtlUnicodeStringToAnsiString into a zero-capacity buffer (Xenia writes before it) (lr=%08X)", uint32_t(ctx.lr));
    const uint32_t copied = std::min(units, capacity - 1);
    for (uint32_t i = 0; i < copied; i++)
        base[buffer + i] = uint8_t(AsciiUnit(runtime::Load16(base, text + 2 * i), "RtlUnicodeStringToAnsiString"));
    base[buffer + copied] = 0;
    ctx.r3.u64 = units + 1 > capacity ? ((2u << 30) | 5) : 0;  // STATUS_BUFFER_OVERFLOW (0x8000_0005)
}

GUEST_IMPORT(RtlFreeAnsiString, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_rtl.cc:244-251; only an empty string here")
{
    // (ANSI_STRING*): frees a buffer RtlUnicodeStringToAnsiString allocated, then clears the string. No allocation is ever made
    // here (that form stops), so a string with a buffer was not allocated by the kernel: it stops; an empty one is cleared.
    const uint32_t string = ctx.r3.u32;
    if (runtime::Load32(base, string + 4) != 0)
        Stop(StopCode::Unimplemented, "RtlFreeAnsiString(%08X): a buffer the kernel never allocated here (lr=%08X)", string, uint32_t(ctx.lr));
    SetCountedString(base, string, 0, 0, 0);
}

GUEST_IMPORT(RtlUpcaseUnicodeChar, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_rtl.cc:237-241 (locale toupper); ASCII only")
{
    const uint16_t c = AsciiUnit(uint16_t(ctx.r3.u32), "RtlUpcaseUnicodeChar");
    ctx.r3.u64 = c >= 'a' && c <= 'z' ? c - 32 : c;
}

GUEST_IMPORT(RtlDowncaseUnicodeChar, "INFERRED: NT RtlDowncaseUnicodeChar (the inverse of RtlUpcaseUnicodeChar); Xenia has none; ASCII only")
{
    const uint16_t c = AsciiUnit(uint16_t(ctx.r3.u32), "RtlDowncaseUnicodeChar");
    ctx.r3.u64 = c >= 'A' && c <= 'Z' ? c + 32 : c;
}

GUEST_IMPORT(RtlTimeToTimeFields, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_rtl.cc:696-717 (FILETIME -> TIME_FIELDS, proleptic Gregorian, no scaling)")
{
    // (FILETIME (100 ns since 1601-01-01) *, TIME_FIELDS*): year, month, day, hour, minute, second, milliseconds, weekday (0 =
    // Sunday), each a big-endian u16 (Xenia X_TIME_FIELDS, :684-694).
    const uint64_t time = uint64_t(runtime::Load32(base, ctx.r3.u32)) << 32 | runtime::Load32(base, ctx.r3.u32 + 4);
    const uint32_t fields = ctx.r4.u32;
    const int64_t days = int64_t(time / kTicksPerDay), rest = int64_t(time % kTicksPerDay);
    int64_t y; unsigned m, d;
    CivilFromDays(days - kDays1601To1970, y, m, d);
    const uint16_t values[8] = {uint16_t(y), uint16_t(m), uint16_t(d), uint16_t(rest / 36000000000LL), uint16_t(rest / 600000000 % 60),
                                uint16_t(rest / 10000000 % 60), uint16_t(rest / 10000 % 1000), uint16_t((days + 1) % 7)};  // 1601-01-01: Monday
    for (uint32_t i = 0; i < 8; i++)
        runtime::Store16(base, fields + 2 * i, values[i]);
}

GUEST_IMPORT(RtlTimeFieldsToTime, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_rtl.cc:719-747 (validated fields -> FILETIME; FALSE when invalid)")
{
    // (TIME_FIELDS*, FILETIME*): TRUE and the time, or FALSE (nothing written) for a year before 1601, a month outside 1-12, a day
    // outside 1-31 or past the month, an hour past 23, a minute or second past 59, milliseconds past 999. The weekday is ignored.
    const uint32_t fields = ctx.r3.u32, out = ctx.r4.u32;
    auto f = [&](uint32_t i) { return runtime::Load16(base, fields + 2 * i); };
    const unsigned y = f(0), m = f(1), d = f(2), h = f(3), mi = f(4), s = f(5), ms = f(6);
    if (y < 1601 || m < 1 || m > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59 || ms > 999)
        return (void)(ctx.r3.u64 = 0);
    int64_t cy; unsigned cm, cd;
    const int64_t days = DaysFromCivil(y, m, d);
    CivilFromDays(days, cy, cm, cd);
    if (cy != int64_t(y) || cm != m || cd != d)  // a day past the month's end (Xenia: year_month_day::ok)
        return (void)(ctx.r3.u64 = 0);
    const uint64_t time = uint64_t(days + kDays1601To1970) * kTicksPerDay + uint64_t(h) * 36000000000ULL + uint64_t(mi) * 600000000ULL +
                          uint64_t(s) * 10000000ULL + uint64_t(ms) * 10000ULL;
    runtime::Store32(base, out, uint32_t(time >> 32)), runtime::Store32(base, out + 4, uint32_t(time));
    ctx.r3.u64 = 1;
}

