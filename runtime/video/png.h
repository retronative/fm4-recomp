// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// An 8-bit RGB PNG with stored (uncompressed) deflate blocks: no compression library. Taken from
// ForzaRecomp runtime/video/presenter.cpp:280-330 (WritePng; GPL-3.0-or-later, same
// owner). Reviewed against ISO/IEC 15948, RFC 1950 and RFC 1951
// (tests/runtime/test_gpu_guard.cpp TestPng). FM4 Recomp changes: returns the bytes instead of
// writing a file (the capture store needs the size before anything is written); uses runtime::Crc32
// (layout.h); an empty image or an `rgb` that is not width * height * 3 bytes returns no bytes
// (ForzaRecomp read past the end; a 0x0 image would emit a zlib stream without a block).
#pragma once
#include "layout.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace runtime::video {

// Empty when width or height is 0 or `rgb` is not width * height * 3 bytes.
inline std::vector<uint8_t> EncodePngRgb8(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb)
{
    if (width == 0 || height == 0 || rgb.size() != size_t(width) * height * 3)
        return {};
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
    for (uint32_t y = 0; y < height; y++)
    {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), rgb.begin() + size_t(y) * width * 3, rgb.begin() + size_t(y + 1) * width * 3);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t at = 0; at < raw.size(); at += 65535)
    {
        const uint16_t n = uint16_t(std::min<size_t>(65535, raw.size() - at));
        z.push_back(at + n == raw.size());
        z.insert(z.end(), {uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(~n >> 8)});
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw)
    {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = b << 16 | a;
    z.insert(z.end(), {uint8_t(adler >> 24), uint8_t(adler >> 16), uint8_t(adler >> 8), uint8_t(adler)});
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        const uint32_t n = uint32_t(data.size()), crc = runtime::Crc32(body.data(), body.size());
        png.insert(png.end(), {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
        png.insert(png.end(), body.begin(), body.end());
        png.insert(png.end(), {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)});
    };
    chunk("IHDR", {uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width), uint8_t(height >> 24),
                   uint8_t(height >> 16), uint8_t(height >> 8), uint8_t(height), 8, 2, 0, 0, 0});
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

}  // namespace runtime::video
