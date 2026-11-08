// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The LDI decoder (runtime/ldi_mspack.cpp) on FM4's zips (local game data, never committed): every method-21 (LZX,
// XMemCompress native framing) entry is split into its blocks (0xFF: uncompressed and compressed size in 2 bytes
// each; otherwise a 2-byte compressed size and 32 KB out; a zero size ends the stream), decoded block by block with
// one decoder per entry (window 2^17: the zips are 128 KB-window streams, OBSERVED; LDI_BITS=n tries another), and checked against the zip's own CRC-32 and
// uncompressed size: an independent oracle (the CRC is computed here, IEEE 802.3 polynomial).
//
//   crosscheck <zip> [<zip> ...]
//
// LDI_EXTRACT=<dir> (opt-in) with LDI_EXTRACT_SUFFIX=<suffix>: each verified entry whose name ends in the suffix is
// also written to <dir>/<zip path with '/' as '_'>/<entry name> (game bytes: a research/ folder, never committed).
#include "ldi_mspack.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
uint32_t Crc32(const uint8_t* p, size_t n)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
uint16_t Le16(const std::vector<uint8_t>& d, size_t at) { return uint16_t(d[at] | d[at + 1] << 8); }
uint32_t Le32(const std::vector<uint8_t>& d, size_t at) { return d[at] | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24; }
}  // namespace

int main(int argc, char** argv)
{
    const runtime::ldi::Factory factory = runtime::ldi::MspackDecoderFactory();
    unsigned entries = 0, failures = 0;
    unsigned long long bytes = 0;
    for (int a = 1; a < argc; a++)
    {
        std::ifstream in(argv[a], std::ios::binary);
        const std::vector<uint8_t> zip((std::istreambuf_iterator<char>(in)), {});
        for (size_t j = 0; j + 46 <= zip.size(); j++)
        {
            if (Le32(zip, j) != 0x02014B50)
                continue;
            const uint16_t method = Le16(zip, j + 10), nameLength = Le16(zip, j + 28);
            const uint32_t crc = Le32(zip, j + 16), csize = Le32(zip, j + 20), usize = Le32(zip, j + 24), local = Le32(zip, j + 42);
            if (method != 21)
                continue;
            const std::string name(zip.begin() + long(j + 46), zip.begin() + long(j + 46 + nameLength));
            const size_t data = local + 30 + Le16(zip, local + 26) + Le16(zip, local + 28);
            std::vector<uint8_t> out(usize);
            std::string error;
            static const int bits = std::getenv("LDI_BITS") ? std::atoi(std::getenv("LDI_BITS")) : 17;
            auto decoder = factory(uint32_t(bits), error);
            size_t p = data, end = data + csize;
            uint32_t produced = 0;
            bool ok = decoder != nullptr;
            while (ok && p + 2 <= end)
            {
                uint32_t u = 0x8000, c;
                if (zip[p] == 0xFF)
                    u = uint32_t(zip[p + 1]) << 8 | zip[p + 2], c = uint32_t(zip[p + 3]) << 8 | zip[p + 4], p += 5;
                else
                    c = uint32_t(zip[p]) << 8 | zip[p + 1], p += 2;
                if (c == 0)
                    break;
                if (produced + u > usize || p + c > end || !decoder->Decompress(&zip[p], c, &out[produced], u, error))
                {
                    ok = false;
                    break;
                }
                produced += u, p += c;
            }
            entries++, bytes += usize;
            const bool verified = ok && produced == usize && Crc32(out.data(), out.size()) == crc;
            static const char* extract = std::getenv("LDI_EXTRACT");
            static const std::string suffix = std::getenv("LDI_EXTRACT_SUFFIX") ? std::getenv("LDI_EXTRACT_SUFFIX") : "";
            if (verified && extract && !suffix.empty() && name.size() >= suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            {
                std::string folder = argv[a];
                for (char& ch : folder)
                    if (ch == '/')
                        ch = '_';
                const std::filesystem::path path = std::filesystem::path(extract) / folder / name;
                std::filesystem::create_directories(path.parent_path());
                std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
            }
            if (!verified)
            {
                failures++;
                std::printf("FAIL %s %s: %s (%u of %u bytes, CRC %08X want %08X)\n", argv[a], name.c_str(), error.c_str(), produced, usize,
                            Crc32(out.data(), out.size()), crc);
            }
        }
    }
    std::printf("ldi crosscheck: %u LZX entries, %llu bytes, %u failures\n", entries, bytes, failures);
    return failures == 0 && entries > 0 ? 0 : 1;
}
