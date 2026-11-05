// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Taken from ForzaRecomp tests/loader/xex_section_extent_test.cpp (ForzaRecomp, GPL-3.0-or-later), reviewed and
// re-run in FM4 Recomp: expected values follow by hand from xex_fixture.h and the
// PE32 section header layout. FM4 Recomp changes: this attribution block only.
//
// Regression test for patches/xenonrecomp/0002's extent bounds: a data section extending past the
// loaded image is mapped only up to the image end; a code section that does not fit rejects it.
// Negative control: FAIL
#include "xex_fixture.h"
#include <xex.h>
#include <image.h>
#include <cstdio>

int main() {
    unsigned checks = 0, failures = 0;
    auto expect = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
    };
    constexpr uint32_t kData = 0x40000040;  // initialized data, readable

    // RVA 0x400 + 0x1000 bytes ends 0x400 past the 0x1000-byte image.
    auto data_input = fixture(0x1000, ".reloc", 0x1000, kData);
    auto data = Xex2LoadImage(data_input.data(), data_input.size());
    bool clamped = false;
    for (const auto& section : data.sections)
        if (section.base == 0x82000400) clamped = section.size == 0xC00 && section.data == data.data.get() + 0x400;
    expect(data.data && clamped, "data section past the image is mapped up to the image end");

    auto inside_input = fixture(0x1000, ".data", 0xC00, kData);
    auto inside = Xex2LoadImage(inside_input.data(), inside_input.size());
    bool whole = false;
    for (const auto& section : inside.sections)
        if (section.base == 0x82000400) whole = section.size == 0xC00;
    expect(inside.data && whole, "data section ending at the image end is mapped whole");

    auto code_input = fixture(0x1000, ".text", 0x1000);
    expect(!Xex2LoadImage(code_input.data(), code_input.size()).data, "code section past the image is rejected");

    std::printf("xex-section-extent: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
