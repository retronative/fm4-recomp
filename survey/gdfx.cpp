// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The GDFX (Xbox 360 disc) directory of a disc image, as survey/gdfx.py: every entry's path, first sector, size and attribute
// byte, one line per entry `path<TAB>sector<TAB>size<TAB>attributes` (hex, lower case; attributes two digits), in GDFX tree
// order (in-order, depth first). A C++ port so the player's setup needs no Python (no Homebrew or
// developer tools on the player's Mac); its output must be byte-identical to gdfx.py's (setup.sh checks its SHA-256).
//   gdfx <disc image (.iso)> <out .tsv>
// Layout re-derived from Xenia xenia-canary 67f16c4e315f vfs/devices/disc_image_device.cc:70-185 (all fields little-endian):
// the game partition starts at one of 0, 0xFB20, 0x20600, 0x2080000, 0xFD90000 bytes; its sector 32 begins
// "MICROSOFT*XBOX*MEDIA" and holds the root directory's sector (+20) and size (+24); a directory is a binary tree of entries at
// 4-byte offsets: left (u16, in 4-byte units), right (u16), sector (u32), size (u32), attributes (u8, 0x10 = directory), name
// length (u8), name. Fail-closed as gdfx.py: a missing volume, a root size outside 13..32 MB, an entry or directory past the
// image, a tree cycle, an entry past its directory's data or a non-ASCII name stops with exit 1; nothing is guessed.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint64_t kSector = 2048;
constexpr uint64_t kOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000};
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

struct Row { std::string path; uint32_t sector, size; uint8_t attributes; };

struct Image
{
    std::ifstream file;
    uint64_t size = 0;
    std::vector<uint8_t> Read(uint64_t offset, uint64_t length)
    {
        if (offset + length > size)
        {
            char m[160];
            std::snprintf(m, sizeof m, "read of %llu bytes at %#llx is past the image (%#llx bytes)", (unsigned long long)length,
                          (unsigned long long)offset, (unsigned long long)size);
            throw std::runtime_error(m);
        }
        std::vector<uint8_t> data(length);
        file.seekg(std::streamoff(offset));
        file.read(reinterpret_cast<char*>(data.data()), std::streamsize(length));
        if (uint64_t(file.gcount()) != length)
            throw std::runtime_error("short read");
        return data;
    }
};

uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

struct Walker
{
    Image& image;
    uint64_t game = 0;
    std::vector<Row> out;

    void Directory(uint32_t sector, uint32_t size, const std::string& prefix)
    {
        const std::string where = prefix.empty() ? "/" : prefix;
        if (size > (32u << 20))
            throw std::runtime_error("directory " + where + " is " + std::to_string(size) + " bytes");
        const std::vector<uint8_t> data = image.Read(game + uint64_t(sector) * kSector, size);
        std::set<uint32_t> seen;
        Node(data, 0, prefix, where, seen);
    }

    void Node(const std::vector<uint8_t>& data, uint32_t ordinal, const std::string& prefix, const std::string& where, std::set<uint32_t>& seen)
    {
        if (!seen.insert(ordinal).second)
            throw std::runtime_error("a cycle in directory " + where + " at entry " + std::to_string(ordinal));
        const uint64_t at = uint64_t(ordinal) * 4;
        if (at + 14 > data.size())
            throw std::runtime_error("entry " + std::to_string(ordinal) + " of " + where + " is past its directory data");
        const uint8_t* e = data.data() + at;
        const uint16_t left = U16(e), right = U16(e + 2);
        const uint32_t esector = U32(e + 4), esize = U32(e + 8);
        const uint8_t attributes = e[12], length = e[13];
        if (at + 14 + length > data.size())
            throw std::runtime_error("the name of entry " + std::to_string(ordinal) + " of " + where + " is past its directory data");
        if (left)
            Node(data, left, prefix, where, seen);
        std::string name(reinterpret_cast<const char*>(e + 14), length);
        for (unsigned char c : name)
            if (c >= 0x80)
                throw std::runtime_error("a name in " + where + " is not ASCII");
        const std::string path = prefix + name;
        out.push_back({path, esector, esize, attributes});
        if ((attributes & 0x10) && esize)
            Directory(esector, esize, path + "/");
        else if (!(attributes & 0x10) && game + uint64_t(esector) * kSector + esize > image.size)
            throw std::runtime_error(path + " (" + std::to_string(esize) + " bytes at sector " + std::to_string(esector) + ") is past the image");
        if (right)
            Node(data, right, prefix, where, seen);
    }
};
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: gdfx <disc image> <out .tsv>\n");
        return 1;
    }
    Image image;
    image.file.open(argv[1], std::ios::binary);
    if (!image.file)
    {
        std::fprintf(stderr, "gdfx: %s: cannot open\n", argv[1]);
        return 1;
    }
    image.file.seekg(0, std::ios::end);
    image.size = uint64_t(image.file.tellg());
    Walker walker{image};
    try
    {
        bool found = false;
        uint32_t rootSector = 0, rootSize = 0;
        for (uint64_t offset : kOffsets)
        {
            const uint64_t at = offset + 32 * kSector;
            if (at + 28 > image.size)
                continue;
            if (std::memcmp(image.Read(at, 20).data(), kMagic, 20) != 0)
                continue;
            const std::vector<uint8_t> root = image.Read(at + 20, 8);
            rootSector = U32(root.data()), rootSize = U32(root.data() + 4);
            if (rootSize < 13 || rootSize > (32u << 20))
                throw std::runtime_error("root directory size " + std::to_string(rootSize) + " is outside 13..32 MB");
            walker.game = offset, found = true;
            break;
        }
        if (!found)
            throw std::runtime_error("no GDFX volume (\"MICROSOFT*XBOX*MEDIA\" at sector 32 of any known partition offset)");
        walker.Directory(rootSector, rootSize, "");
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "gdfx: %s: %s\n", argv[1], e.what());
        return 1;
    }
    FILE* out = std::fopen(argv[2], "wb");
    if (out == nullptr)
    {
        std::fprintf(stderr, "gdfx: %s: cannot write\n", argv[2]);
        return 1;
    }
    size_t directories = 0;
    for (const Row& r : walker.out)
    {
        std::fprintf(out, "%s\t%x\t%x\t%02x\n", r.path.c_str(), r.sector, r.size, r.attributes);
        directories += (r.attributes & 0x10) != 0;
    }
    std::fclose(out);
    std::printf("gdfx: %zu entries (%zu directories) -> %s\n", walker.out.size(), directories, argv[2]);
    return 0;
}
