// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Regression test for patches/xenonrecomp/0001's first check: a BASIC file-format header too short
// to hold one block (info size 4 < 8) is rejected. Upstream computes the block count as
// 4 / 8 - 1, which wraps to SIZE_MAX and walks past the header, so its negative control is an
// AddressSanitizer report. Synthetic fixture (tests/loader/xex_fixture.h): no game bytes.
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

    auto shortFormat = fixture(0x2000);
    be32(shortFormat, 0x60, 4);  // xex2_opt_file_format_info.info_size
    expect(!Xex2LoadImage(shortFormat.data(), shortFormat.size()).data, "file-format header without a block is rejected");

    std::printf("xex-basic-short-format: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
