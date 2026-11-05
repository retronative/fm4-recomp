// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Taken from ForzaRecomp tests/loader/xex_basic_allocation_test.cpp (ForzaRecomp, GPL-3.0-or-later), reviewed and
// re-run in FM4 Recomp: fixture layout checked against Xenia canary 67f16c4e315f
// xex2_info.h and the PE32 header layout; expected values follow by hand from the fixture bytes.
// FM4 Recomp changes: this attribution block only.
//
// Regression test for patches/xenonrecomp/0001 (XEX BASIC compression allocates the declared
// image size). A synthetic, unencrypted XEX with one BASIC block: no game bytes. Built with
// AddressSanitizer by tests/run_loader_tests.sh, which also runs it against the unpatched loader
// as a negative control (it must fail there).
// Negative control: FAIL
#include "xex_fixture.h"
#include <xex.h>
#include <image.h>
#include <sanitizer/asan_interface.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    unsigned checks = 0, failures = 0;
    auto expect = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
    };

    auto input = fixture(0x2000);  // blocks expand to less than the declared size
    auto image = Xex2LoadImage(input.data(), input.size());
    expect(image.size == 0x2000, "reports the declared image size");
    const bool covered = image.data && !__asan_region_is_poisoned(image.data.get(), image.size);
    expect(covered, "allocation covers the reported image size");
    expect(image.data && std::memcmp(image.data.get(), input.data() + 0x400, 0x1000) == 0,
           "expanded block bytes are preserved");
    if (covered)
        expect(std::all_of(image.data.get() + 0x1000, image.data.get() + 0x2000, [](uint8_t v) { return v == 0; }),
               "declared tail is zero-filled");

    auto equal_input = fixture(0x1000);  // blocks expand to exactly the declared size
    auto equal = Xex2LoadImage(equal_input.data(), equal_input.size());
    expect(equal.data && equal.size == 0x1000 && !__asan_region_is_poisoned(equal.data.get(), equal.size),
           "equal-size BASIC image still loads");

    auto oversize_input = fixture(0x800);  // blocks expand beyond the declared size
    auto oversize = Xex2LoadImage(oversize_input.data(), oversize_input.size());
    expect(!oversize.data, "blocks expanding beyond the declared size are rejected");

    std::printf("xex-basic-allocation: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
