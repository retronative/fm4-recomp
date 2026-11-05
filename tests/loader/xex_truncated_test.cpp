// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Taken from ForzaRecomp tests/loader/xex_truncated_test.cpp (ForzaRecomp), reviewed and
// re-run in FM4 Recomp. FM4 Recomp changes: this attribution block, and the patch
// number (ForzaRecomp's 0003 is FM4 Recomp's 0004: patches/xenonrecomp/README.md).
//
// Regression test for patches/xenonrecomp/0004's loader guard (ForzaRecomp 0003): a XEX whose header claims to be
// larger than the file loads as an empty image. The unpatched loader reads past its input on it,
// so its negative control is an AddressSanitizer report.
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
    auto input = fixture(0x1000);
    be32(input, 8, 0x2000);  // header size past the 0x1400-byte file
    expect(!Xex2LoadImage(input.data(), input.size()).data, "header larger than the file is rejected");
    std::printf("xex-truncated: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
