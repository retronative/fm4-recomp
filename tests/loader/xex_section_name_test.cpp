// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Taken from ForzaRecomp tests/loader/xex_section_name_test.cpp (ForzaRecomp, GPL-3.0-or-later), reviewed and
// re-run in FM4 Recomp: expected values follow by hand from xex_fixture.h and the
// PE32 section header layout. FM4 Recomp changes: this attribution block only.
//
// Regression test for patches/xenonrecomp/0002 (section names are read from at most the 8-byte
// PE field). An 8-character name has no terminating NUL; the byte after it is VirtualSize (4).
// Negative control: FAIL
#include "xex_fixture.h"
#include <xex.h>
#include <image.h>
#include <cstdio>
#include <string>

int main() {
    unsigned checks = 0, failures = 0;
    auto expect = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
    };
    for (const char* name : {".text", "ABCDEFGH"}) {
        auto input = fixture(0x1000, name);
        auto image = Xex2LoadImage(input.data(), input.size());
        bool found = false;
        for (const auto& section : image.sections)
            if (section.base == 0x82000400) found = section.name == std::string(name);
        expect(image.data && found, std::string(name).size() == 8 ? "8-character section name read exactly"
                                                                    : "short section name read exactly");
    }
    std::printf("xex-section-name: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
