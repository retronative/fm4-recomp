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
    // FM4 Recomp (M7): written straight into the file buffer (one copy of the pixels), Adler-32 with the modulo deferred to every
    // 5552 bytes (zlib adler32.c NMAX: the largest n with 255 n (n + 1) / 2 + (n + 1) (65520) < 2^32), CRC-32 over the chunk in place.
    const size_t rowBytes = size_t(width) * 3, rawBytes = size_t(height) * (rowBytes + 1), blocks = (rawBytes + 65534) / 65535;
    const size_t idatBytes = 2 + blocks * 5 + rawBytes + 4;
    std::vector<uint8_t> png;
    png.reserve(8 + 25 + 12 + idatBytes + 12);
    png.insert(png.end(), {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'});
    auto put32 = [&](uint32_t v) { png.insert(png.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}); };
    auto chunk = [&](const char* type, auto&& body) {
        put32(uint32_t(0));
        const size_t at = png.size();
        png.insert(png.end(), type, type + 4);
        body();
        const uint32_t n = uint32_t(png.size() - at - 4);
        png[at - 4] = uint8_t(n >> 24), png[at - 3] = uint8_t(n >> 16), png[at - 2] = uint8_t(n >> 8), png[at - 1] = uint8_t(n);
        put32(runtime::Crc32(png.data() + at, png.size() - at));
    };
    chunk("IHDR", [&] {
        png.insert(png.end(), {uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width), uint8_t(height >> 24),
                               uint8_t(height >> 16), uint8_t(height >> 8), uint8_t(height), 8, 2, 0, 0, 0});
    });
    chunk("IDAT", [&] {
        png.insert(png.end(), {0x78, 0x01});
        uint32_t a = 1, b = 0, pending = 0;
        size_t left = rawBytes, block = 0, row = 0, col = 0;  // col: position within the current scanline (0 = its filter byte)
        while (left > 0)
        {
            const uint16_t n = uint16_t(std::min<size_t>(65535, left));
            left -= n, block = n;
            png.insert(png.end(), {uint8_t(left == 0), uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(~n >> 8)});
            while (block > 0)
            {
                // The next run: the filter byte (none, 0) or the rest of the scanline, at most what the block holds.
                const uint8_t zero = 0;
                const uint8_t* from = col == 0 ? &zero : rgb.data() + row * rowBytes + (col - 1);
                const size_t take = col == 0 ? 1 : std::min(block, rowBytes + 1 - col);
                png.insert(png.end(), from, from + take);
                for (size_t i = 0; i < take; i++)
                {
                    a += from[i], b += a;
                    if (++pending == 5552)
                        a %= 65521, b %= 65521, pending = 0;
                }
                block -= take, col += take;
                if (col == rowBytes + 1)
                    col = 0, row++;
            }
        }
        a %= 65521, b %= 65521;
        const uint32_t adler = b << 16 | a;
        png.insert(png.end(), {uint8_t(adler >> 24), uint8_t(adler >> 16), uint8_t(adler >> 8), uint8_t(adler)});
    });
    chunk("IEND", [] {});
    return png;
}

}  // namespace runtime::video
