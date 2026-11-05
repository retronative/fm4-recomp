// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Image::Find(address) (XenonUtils/image.cpp) returns the section data for an address inside a
// section, and null outside every section (patch 0023a). Built with AddressSanitizer by
// tests/test_analysis.py. One 16-byte section at 0x82001000 (image base 0).
#include <image.h>
#include <cstdio>
#include <cstdint>

int main()
{
    unsigned failures = 0;
    auto expect = [&](bool ok, const char* name) { if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); } };
    static uint8_t text[16];
    Image image;
    image.base = 0;
    image.Map(".text", 0x82001000, sizeof(text), SectionFlags_Code, text);
    expect(image.Find(0x82001000) == text, "first byte of the section");
    expect(image.Find(0x8200100C) == text + 12, "last word of the section");
    expect(image.Find(0x82000FF8) == nullptr, "address before the first section is null");
    expect(image.Find(0x82001010) == nullptr, "address at the section's end is null");
    image.Map(".empty", 0x82002000, 0, 0, nullptr);
    expect(image.Find(0x82002000) == nullptr, "section without data is null");
    std::printf("image-find: %u failures\n", failures);
    return failures ? 1 : 0;
}
