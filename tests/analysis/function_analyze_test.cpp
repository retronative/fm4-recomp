// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Function::Analyze (XenonAnalyse/function.cpp) must stay within the size it is given (patch
// 0020). Built with AddressSanitizer by tests/test_analysis.py: each code buffer is exactly `size`
// bytes, so a read past it is reported. Words big-endian, hand-encoded (PowerPC ISA):
// li r3,1 = 0x38600001, li r4,2 = 0x38800002, blr = 0x4E800020.
#include <function.h>
#include <cstdio>
#include <cstdint>
#include <memory>

static uint32_t be(uint32_t v) { return __builtin_bswap32(v); }

int main()
{
    unsigned failures = 0;
    auto expect = [&](bool ok, const char* name) { if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); } };

    {   // two straight-line words, size 4: the function is the first word only
        auto code = std::make_unique<uint32_t[]>(1);
        code[0] = be(0x38600001);
        auto fn = Function::Analyze(code.get(), 4, 0x82000000);
        expect(fn.base == 0x82000000 && fn.size == 4, "size limit of one word is kept");
    }
    {   // a lone blr, size 4
        auto code = std::make_unique<uint32_t[]>(1);
        code[0] = be(0x4E800020);
        auto fn = Function::Analyze(code.get(), 4, 0x82000000);
        expect(fn.size == 4, "lone blr is one word");
    }
    {   // li; blr within a size of 8: two words
        auto code = std::make_unique<uint32_t[]>(2);
        code[0] = be(0x38800002); code[1] = be(0x4E800020);
        auto fn = Function::Analyze(code.get(), 8, 0x82000000);
        expect(fn.size == 8, "li; blr is two words");
    }
    std::printf("function-analyze: %u failures\n", failures);
    return failures ? 1 : 0;
}
