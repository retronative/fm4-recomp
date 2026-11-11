// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The xboxkrnl printf family: _snprintf (ordinal 314) and _snwprintf. Ported from Xenia
// xenia-canary 67f16c4e315f, src/xenia/kernel/xboxkrnl/xboxkrnl_strings.cc:
// format_core (:120-683), StackArgList (:685-701), StringFormatData/WideStringFormatData (:722-791),
// _snprintf_entry (:857-894), _snwprintf_entry (:928-966). The varargs come from
// src/xenia/kernel/util/shim_utils.h:51-84 (get_arg_stack_ptr, get_arg_32, get_arg_64).
// Xenia's notice and licence, as its LICENSE requires for source redistributions:
//
//   Copyright 2022 Ben Vanik. All rights reserved. (xboxkrnl_strings.cc)
//   Copyright (c) 2015, Ben Vanik. All rights reserved. (LICENSE)
//
//   Redistribution and use in source and binary forms, with or without modification, are permitted
//   provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright notice, this list of
//       conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright notice, this list of
//       conditions and the following disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of the project nor the names of its contributors may be used to endorse or
//       promote products derived from this software without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
//   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
//   FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
//   DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
//   BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
//   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// FM4 Recomp changes (each marked "FM4 change" below):
//  - Every path where Xenia asserts (assert_always: %Z, an unknown conversion) stops (7) by name, as
//    do the conversions not modelled here: %n (writes guest memory), %e %E %g %G %a %A (Xenia formats
//    them with iostreams: a C99 two-digit exponent and libc++'s hexfloat; the Xbox 360 CRT's exact
//    text is not known), the 'L' size (Xenia's own TODO, :294-297), a non-finite %f, a width or
//    precision above 65536, an integer precision above 510 (Xenia's 512-byte work buffer) and a
//    %f text of 512 bytes or more (Xenia asserts, :485).
//  - Stack varargs: Xenia's StackArgList reads every argument as get_arg_64 (shim_utils.h:79-84),
//    which for n >= 8 loads the doubleword at r1 + 0x54 + 8k: the low word of slot k and the high
//    word of slot k+1, so its 32-bit truncation is slot k+1's high word, not the argument. Here a
//    32-bit read is runtime::Argument (the low word at r1 + 0x54 + 8k, Xenia get_arg_32, which every
//    stack-argument import of this runtime uses) and a 64-bit read is the whole slot at
//    r1 + 0x50 + 8k (big-endian, so its low word is the one get_arg_32 reads).
//  - Integers are converted from an unsigned 64-bit magnitude (Xenia's int64 arithmetic is
//    undefined for INT64_MIN and gives negative digits for %llu/%llx above INT64_MAX), and an
//    unsigned conversion with 'h' takes the low 16 bits zero-extended (Xenia sign-extends, so %hu of
//    0xFFFF would print 4294967295; MSDN "Argument Size Specification", tcxf1dw6: h = unsigned short).
//  - %f is std::snprintf("%.*f") / ("%#.*f" for '#'), which is what Xenia's std::fixed <<
//    std::setprecision (format_double, :87-118) produces by the C++ standard ([facet.num.put.virtuals]).
//  - One host buffer of UTF-16 code units for both widths (Xenia: std::string / std::u16string), the
//    guest written big-endian; Xenia's text/prefix work buffers are std::u16string.
#include "imports.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace {

enum : uint32_t
{
    kLeftJustify = 1 << 0,
    kAddLeadingZeros = 1 << 1,
    kAddPositive = 1 << 2,
    kAddPositiveAsSpace = 1 << 3,
    kAddNegative = 1 << 4,
    kAddPrefix = 1 << 5,
    kIsShort = 1 << 6,
    kIsLong = 1 << 7,
    kIsLongLong = 1 << 8,
    kIsWide = 1 << 9,
    kIsSigned = 1 << 10,
    kForceLeadingZero = 1 << 11,
    kInvertWide = 1 << 12,
};  // Xenia FormatFlags, :43-57

constexpr int32_t kMaxWidth = 65536;  // FM4 change: a bound on the width/precision (Xenia has none)

// The format in guest memory (Xenia StringFormatData :722-742, WideStringFormatData :759-779): get()
// does not advance past the terminator; a wide format is big-endian UTF-16.
struct FormatInput
{
    const uint8_t* base;
    uint32_t at;
    bool wide;
    uint16_t peek(int32_t offset) const { return wide ? runtime::Load16(base, at + 2 * offset) : base[at + offset]; }
    uint16_t get()
    {
        const uint16_t c = peek(0);
        if (c)
            at += wide ? 2 : 1;
        return c;
    }
    void skip(int32_t count)
    {
        while (count-- > 0 && get())
        {
        }
    }
};

// The output (Xenia StringFormatData::put :744-750, WideStringFormatData::put :781-784): a narrow
// output refuses a code unit of 0x100 or more, which makes format_core return -1.
struct FormatOutput
{
    bool wide;
    std::u16string units;
    bool put(uint16_t c)
    {
        if (!wide && c >= 0x100)
            return false;
        units.push_back(char16_t(c));
        return true;
    }
};

// The varargs from argument `index` on (Xenia StackArgList :685-701 over shim_utils.h; see the
// FM4 change at the top for the stack slots).
struct StackArgs
{
    const PPCContext& ctx;
    const uint8_t* base;
    unsigned index;
    uint32_t get32() { return runtime::Argument(ctx, base, index++); }
    uint64_t get64()
    {
        const unsigned n = index++;
        const PPCRegister* registers[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
        if (n < 8)
            return registers[n]->u64;
        // FM4 Recomp: where a 64-bit vararg past the eighth sits on the stack is not verified (Xenia's get_arg_64 reads r1+0x54+8k,
        // which straddles two slots; no FM4 caller has been seen passing one): stop by name instead of guessing.
        Stop(StopCode::Unimplemented, "_snprintf/_snwprintf: a 64-bit argument (%%lld, %%I64, %%f) passed on the stack (vararg %u) is not modelled", n + 1);
    }
};

// Xenia format_core (:120-683): the number of code units written, or -1 when the format ends
// inside a conversion (:172-175) or the output refuses a code unit (:160-162 and the puts below).
int32_t FormatCore(const char* import, FormatInput& data, FormatOutput& out, StackArgs& args)
{
    int32_t count = 0;
    for (uint16_t c = data.get();; c = data.get())
    {
        if (!c)
            return count;
        if (c != '%')
        {
            if (!out.put(c))
                return -1;
            ++count;
            continue;
        }
        if (!(c = data.get()))
            return -1;
        if (c == '%')
        {
            if (!out.put(c))
                return -1;
            ++count;
            continue;
        }
        auto next = [&] { return (c = data.get()) != 0; };  // Xenia :172-175: a terminator in a conversion is -1

        uint32_t flags = 0;
        int32_t width = 0, precision = -1;
        std::u16string prefix, text;

        // Flags (:213-232).
        for (;;)
        {
            if (c == '-')
                flags |= kLeftJustify;
            else if (c == '+')
                flags |= kAddPositive;
            else if (c == '0')
                flags |= kAddLeadingZeros;
            else if (c == ' ')
                flags |= kAddPositiveAsSpace;
            else if (c == '#')
                flags |= kAddPrefix;
            else
                break;
            if (!next())
                return -1;
        }
        // Width (:235-251): '*' takes an argument, negative = left-justified.
        if (c == '*')
        {
            width = int32_t(args.get32());
            if (width < 0)
            {
                flags |= kLeftJustify;
                width = width == INT32_MIN ? INT32_MAX : -width;
            }
            if (!next())
                return -1;
        }
        else
        {
            while (c >= '0' && c <= '9')
            {
                width = width * 10 + (c - '0');
                if (width > kMaxWidth || !next())
                    break;
            }
            if (!c)
                return -1;
        }
        // Precision (:254-280): '.' alone is 0; '*' takes an argument, negative = none.
        if (c == '.' && width <= kMaxWidth)
        {
            precision = 0;
            if (!next())
                return -1;
            if (c == '*')
            {
                precision = int32_t(args.get32());
                if (precision < 0)
                    precision = -1;
                if (!next())
                    return -1;
            }
            else
            {
                while (c >= '0' && c <= '9')
                {
                    precision = precision * 10 + (c - '0');
                    if (precision > kMaxWidth || !next())
                        break;
                }
                if (!c)
                    return -1;
            }
        }
        if (width > kMaxWidth || precision > kMaxWidth)
            Stop(StopCode::Unimplemented, "%s: a width or precision above %d (not modelled)", import, kMaxWidth);
        // Size (:283-324): l, ll, h, w, I, I32, I64. FM4 change: 'L' stops (Xenia :294-297 ignores it, unverified).
        if (c == 'l')
        {
            if (data.peek(0) == 'l')
            {
                data.skip(1);
                flags |= kIsLongLong;
            }
            else
                flags |= kIsLong;
            if (!next())
                return -1;
        }
        else if (c == 'L')
            Stop(StopCode::Unimplemented, "%s: the size prefix 'L' (Xenia ignores it with a TODO; not modelled)", import);
        else if (c == 'h' || c == 'w')
        {
            flags |= c == 'h' ? kIsShort : kIsWide;
            if (!next())
                return -1;
        }
        else if (c == 'I')
        {
            if (data.peek(0) == '6' && data.peek(1) == '4')
            {
                data.skip(2);
                flags |= kIsLongLong;
            }
            else if (data.peek(0) == '3' && data.peek(1) == '2')
                data.skip(2);
            if (!next())
                return -1;
        }

        // The integer conversions (:375-430). FM4 change: an unsigned 64-bit magnitude; unsigned 'h' zero-extends.
        auto integer = [&](const char* digits, uint32_t radix) {
            uint64_t magnitude;
            if (flags & kIsLongLong)
            {
                const uint64_t value = args.get64();
                const bool negative = (flags & kIsSigned) && int64_t(value) < 0;
                if (negative)
                    flags |= kAddNegative;
                magnitude = negative ? 0 - value : value;
            }
            else
            {
                const uint32_t raw = args.get32();
                int64_t value;
                if (flags & kIsLong)
                    value = int32_t(raw);
                else if (flags & kIsShort)
                    value = (flags & kIsSigned) ? int64_t(int16_t(raw)) : int64_t(uint16_t(raw));
                else
                    value = int32_t(raw);
                if ((flags & kIsSigned) && value < 0)
                {
                    value = -value;
                    flags |= kAddNegative;
                }
                magnitude = uint64_t(value) & UINT32_MAX;  // Xenia :402-404
            }
            if (precision > 510)
                Stop(StopCode::Unimplemented, "%s: an integer precision of %d (above Xenia's 512-byte work buffer)", import, precision);
            int32_t digitsLeft = precision >= 0 ? precision : 1;  // Xenia :391-395
            if (magnitude == 0)
                prefix.clear();  // Xenia :406-408: no 0x for zero
            while (digitsLeft-- > 0 || magnitude != 0)
            {
                text.insert(text.begin(), char16_t(digits[magnitude % radix]));
                magnitude /= radix;
            }
            if ((flags & kForceLeadingZero) && (text.empty() || text[0] != '0'))
                text.insert(text.begin(), u'0');  // Xenia :421-424 (octal '#')
        };

        // The conversion (:327-593).
        switch (c)
        {
        case 'c':
        case 'C':  // Xenia :330-366: lc/wc wide, hc narrow, else C inverts the format's width
        {
            const bool isWide = (flags & (kIsLong | kIsWide)) ? true
                                : (flags & kIsShort)          ? false
                                                              : (c == 'C') != data.wide;
            const uint32_t value = args.get32();
            text.push_back(isWide ? char16_t(uint16_t(value)) : char16_t(uint8_t(value)));
            break;
        }
        case 'd':
        case 'i':
            flags |= kIsSigned;
            integer("0123456789", 10);
            break;
        case 'o':
            if (flags & kAddPrefix)
                flags |= kForceLeadingZero;
            integer("01234567", 8);
            break;
        case 'u':
            integer("0123456789", 10);
            break;
        case 'x':
        case 'X':
            if (flags & kAddPrefix)
                prefix = c == 'x' ? u"0x" : u"0X";
            integer(c == 'x' ? "0123456789abcdef" : "0123456789ABCDEF", 16);
            break;
        case 'p':  // Xenia :523-530: 8 upper-case hex digits of a 32-bit value
            precision = 8;
            flags = (flags & ~(kIsLongLong | kIsShort)) | kIsLong;
            integer("0123456789ABCDEF", 16);
            break;
        case 'f':  // Xenia :471-497: the double's bits are the next 64-bit argument (a GPR or a stack slot)
        {
            flags |= kIsSigned;
            const uint64_t bits = args.get64();
            double value;
            std::memcpy(&value, &bits, 8);
            if (!std::isfinite(value))
                Stop(StopCode::Unimplemented, "%s: %%f of a non-finite double %016llX (the CRT's text is not modelled)", import,
                     static_cast<unsigned long long>(bits));
            if (value < 0)
            {
                value = -value;
                flags |= kAddNegative;
            }
            char work[512];
            const int length = std::snprintf(work, sizeof(work), (flags & kAddPrefix) ? "%#.*f" : "%.*f", precision < 0 ? 6 : precision, value);
            if (length < 0 || length >= int(sizeof(work)))
                Stop(StopCode::Unimplemented, "%s: a %%f text of %d bytes (Xenia's 512-byte work buffer)", import, length);
            for (int i = 0; i < length; i++)
                text.push_back(char16_t(uint8_t(work[i])));
            break;
        }
        case 's':
        case 'S':  // Xenia :533-581: ls/ws wide, hs narrow, else S inverts the format's width; null is "(null)"
        {
            const uint32_t pointer = args.get32();
            int32_t cap = precision < 0 ? INT32_MAX : precision;
            if (pointer == 0)
            {
                for (const char* s = "(null)"; *s && cap > 0; ++s, --cap)
                    text.push_back(char16_t(*s));
                break;
            }
            const bool isWide = (flags & (kIsLong | kIsWide)) ? true
                                : (flags & kIsShort)          ? false
                                                              : (c == 'S') != data.wide;
            for (uint32_t at = pointer; cap > 0; --cap)
            {
                const uint16_t unit = isWide ? runtime::Load16(args.base, at) : args.base[at];
                if (!unit)
                    break;
                text.push_back(char16_t(unit));
                at += isWide ? 2 : 1;
            }
            break;
        }
        default:
            // FM4 change: Xenia asserts on %Z and unknown conversions (:584-591) and formats %e/%g/%a with
            // iostreams (:87-118); %n writes guest memory (:512-520). None is modelled: stop by name.
            Stop(StopCode::Unimplemented, "%s: the conversion '%%%c' (U+%04X) is not modelled", import, c >= 0x20 && c < 0x7F ? int(c) : int('?'), unsigned(c));
        }

        // The sign prefix, padding, prefix, zeros, text and right padding (:596-677).
        if (flags & kIsSigned)
        {
            if (flags & kAddNegative)
                prefix = u"-";
            else if (flags & kAddPositive)
                prefix = u"+";
            else if (flags & kAddPositiveAsSpace)
                prefix = u" ";
        }
        int32_t padding = width - int32_t(text.size()) - int32_t(prefix.size());
        if (!(flags & (kLeftJustify | kAddLeadingZeros)) && padding > 0)
        {
            count += padding;
            while (padding-- > 0)
                if (!out.put(' '))
                    return -1;
        }
        count += int32_t(prefix.size());
        for (char16_t unit : prefix)
            if (!out.put(unit))
                return -1;
        if ((flags & kAddLeadingZeros) && !(flags & kLeftJustify) && padding > 0)
        {
            count += padding;
            while (padding-- > 0)
                if (!out.put('0'))
                    return -1;
        }
        for (char16_t unit : text)
            if (!out.put(unit))
                return -1;
        count += int32_t(text.size());
        if ((flags & kLeftJustify) && padding > 0)
        {
            count += padding;
            while (padding-- > 0)
                if (!out.put(' '))
                    return -1;
        }
    }
}

// Xenia _snprintf_entry (:857-894) and _snwprintf_entry (:928-966): (buffer, count in code units,
// format, ...). A null buffer or format, or a count <= 0: -1, nothing written. A format error: the
// first unit 0, -1. Output of n units: n <= count copies them, terminated only when n < count, and
// returns n; n > count copies the first count units, unterminated, and returns -1.
void Printf(const char* import, PPCContext& ctx, uint8_t* base, bool wide)
{
    const uint32_t buffer = ctx.r3.u32, format = ctx.r5.u32;
    const int32_t capacity = ctx.r4.s32;
    if (buffer == 0 || capacity <= 0 || format == 0)
        return (void)(ctx.r3.u64 = uint64_t(int64_t(-1)));
    FormatInput data{base, format, wide};
    FormatOutput out{wide, {}};
    StackArgs args{ctx, base, 3};
    int32_t written = FormatCore(import, data, out, args);
    auto store = [&](int32_t i, uint16_t unit) {
        if (wide)
            runtime::Store16(base, buffer + 2 * uint32_t(i), unit);
        else
            base[buffer + uint32_t(i)] = uint8_t(unit);
    };
    if (written < 0)
        store(0, 0);
    else if (written <= capacity)
    {
        for (int32_t i = 0; i < written; i++)
            store(i, out.units[size_t(i)]);
        if (written < capacity)
            store(written, 0);
    }
    else
    {
        for (int32_t i = 0; i < capacity; i++)
            store(i, out.units[size_t(i)]);
        written = -1;
    }
    ctx.r3.u64 = uint64_t(int64_t(written));  // SHIM_SET_RETURN_32: sign-extended (shim_utils.h:131)
}

}  // namespace

GUEST_IMPORT(_snprintf, "REFERENCE_DERIVED: Xenia xboxkrnl_strings.cc:857 _snprintf_entry, format_core :120-683 (stops and stack slots: see the file)")
{
    Printf("_snprintf", ctx, base, false);
}

GUEST_IMPORT(_snwprintf, "REFERENCE_DERIVED: Xenia xboxkrnl_strings.cc:928 _snwprintf_entry, format_core :120-683 (stops and stack slots: see the file)")
{
    Printf("_snwprintf", ctx, base, true);
}
