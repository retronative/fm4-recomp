// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Taken from ForzaRecomp tests/loader/xex_basic_overread_test.cpp (ForzaRecomp, GPL-3.0-or-later), reviewed and
// re-run in FM4 Recomp: fixture layout checked against Xenia canary 67f16c4e315f
// xex2_info.h and the PE32 header layout; expected values follow by hand from the fixture bytes.
// FM4 Recomp changes: this attribution block only.
//
// Regression test for patches/xenonrecomp/0001's bounds checks: BASIC block data that runs past
// the end of the file is rejected. The unpatched loader reads past its input on it, so its
// negative control is an AddressSanitizer report.
// Negative control: AddressSanitizer
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

    // 0x2000 data bytes declared, 0x1000 present after the header.
    auto overread = fixture(0x2000);
    be32(overread, kBlockDataSize, 0x2000);
    expect(!Xex2LoadImage(overread.data(), overread.size()).data, "block data past the end of the file is rejected");

    std::printf("xex-basic-overread: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
