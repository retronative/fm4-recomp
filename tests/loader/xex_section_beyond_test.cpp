// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Regression test for patches/xenonrecomp/0002: a data section that starts at or past the end of
// the loaded image (FM4's facade DLLs: .edata, .idata, .XBLD, .reloc lie wholly past their images)
// is mapped empty, with no data pointer into or past the image buffer. Upstream maps it with its
// full size at image.data + RVA, past the allocation. Synthetic fixture (xex_fixture.h): the
// section header is at 0x400 + 0x178; its VirtualAddress (RVA) at +12 is moved past the image.
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
    constexpr size_t kSectionRva = 0x400 + 0x178 + 12;

    for (uint32_t rva : {0x1000u, 0x3000u}) {  // at the image end; wholly past it
        auto input = fixture(0x1000, ".edata", 0x200, kData);
        le32(input, kSectionRva, rva);
        auto image = Xex2LoadImage(input.data(), input.size());
        bool empty = false;
        for (const auto& section : image.sections)
            if (section.base == 0x82000000 + rva) empty = section.size == 0 && section.data == nullptr;
        expect(image.data && empty, rva == 0x1000 ? "data section at the image end is mapped empty"
                                                  : "data section past the image is mapped empty");
    }
    std::printf("xex-section-beyond: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
