// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// xexsurvey: load a retail XEX through XenonRecomp's loader (Xex2LoadImage: AES + basic/LZX
// decompression), check the loaded image independently, write it, and list sections, imports and
// the compiler helpers XenonRecomp's config needs.
//
//   tools/survey/xexsurvey <file.xex> <out-dir>
//
// Writes <out-dir>/image.bin (proprietary-derived: <out-dir> must be under game/, git-ignored).
// Exits 1 with "FATAL:" when a check fails; prints "image check: OK" when all pass.
//
// Adapted from ReCarbon survey/xexsurvey.cpp (ReCarbon, GPL-3.0-or-later).
// FM4 Recomp changes: the image checks below (Xex2LoadImage sets image.size from the header's
// image_size unconditionally, so comparing the two would prove nothing); FATAL exits instead of
// printing on an empty load; function/data import counts per library.
//
// Image checks:
//   1. the page descriptors' total equals the security info's image_size (Xenia canary
//      67f16c4e315f xex_module.cc ReadImageBasicCompressed sizes the image this way);
//   2. basic compression: the blocks' data_size + zero_size sum to at most image_size (Xenia
//      zero-fills the rest), and the blocks' data fits in the file after the header (layout:
//      xex2_info.h xex2_opt_file_format_info / xex2_file_basic_compression_block). Upstream
//      Xex2LoadImage allocates only the block sum but reports image_size, so a shorter sum
//      means the loader would read past its buffer: refuse (FATAL) until the loader is fixed
//     
//   3. the loaded image is a PE image: "MZ", e_lfanew inside the image, "PE\0\0"
//      (a wrong key or a bad decompression yields neither);
//   4. PE OptionalHeader.SizeOfImage == image_size and ImageBase == the load base;
//   5. every PE section lies inside the image.

#include <xex.h>
#include <image.h>
#include <file.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

static uint32_t ReadBE32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

static uint32_t ReadLE32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static uint16_t ReadLE16(const uint8_t* p)
{
    return uint16_t(p[0] | (p[1] << 8));
}

static int failures = 0;

// True while tools/XenonRecomp has no loader fix for BASIC images whose blocks are shorter than
// the declared image (upstream ddd128bc). Set to false in the same change that adds the fix.
constexpr bool kUpstreamLoader = true;

static void Check(bool ok, const char* what, uint64_t got, uint64_t want)
{
    if (ok)
    {
        printf("  ok    %s (0x%llX)\n", what, (unsigned long long)got);
        return;
    }
    printf("  FATAL: %s: got 0x%llX, want 0x%llX\n", what, (unsigned long long)got, (unsigned long long)want);
    fprintf(stderr, "FATAL: %s: got 0x%llX, want 0x%llX\n", what, (unsigned long long)got, (unsigned long long)want);
    failures++;
}

// Optional-header lookup on the raw file (headers are never encrypted). Key low byte 0x00/0x01:
// the value is inline (Xenia xex_module.cc GetOptHeader).
static const uint8_t* FindOptHeader(const std::vector<uint8_t>& file, uint32_t key)
{
    uint32_t count = ReadBE32(&file[0x14]);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* entry = &file[0x18 + i * 8];
        if (ReadBE32(entry) == key)
        {
            uint32_t value = ReadBE32(entry + 4);
            if ((key & 0xFF) == 0 || (key & 0xFF) == 1)
                return entry + 4;
            return value < file.size() ? &file[value] : nullptr;
        }
    }
    return nullptr;
}

struct Pattern
{
    const char* key;
    std::vector<uint8_t> bytes;
};

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: xexsurvey <file.xex> <out-dir>\n");
        return 2;
    }

    std::vector<uint8_t> file = LoadFile(argv[1]);
    if (file.size() < 0x18 || memcmp(file.data(), "XEX2", 4) != 0)
    {
        fprintf(stderr, "FATAL: %s is not a readable XEX2 file\n", argv[1]);
        return 1;
    }

    const uint32_t headerSize = ReadBE32(&file[0x8]);
    const uint32_t securityOffset = ReadBE32(&file[0x10]);
    if (securityOffset + 0x184 > file.size() || headerSize > file.size())
    {
        fprintf(stderr, "FATAL: header or security info outside the file\n");
        return 1;
    }
    const uint32_t imageSize = ReadBE32(&file[securityOffset + 0x4]);
    const uint32_t loadAddress = ReadBE32(&file[securityOffset + 0x110]);

    printf("## Image checks\n\n");
    const uint8_t* fileFormat = FindOptHeader(file, 0x000003FF);
    if (fileFormat == nullptr)
    {
        fprintf(stderr, "FATAL: no file format header\n");
        return 1;
    }
    const uint32_t formatSize = ReadBE32(fileFormat);
    const uint16_t encryption = (fileFormat[4] << 8) | fileFormat[5];
    const uint16_t compression = (fileFormat[6] << 8) | fileFormat[7];
    printf("  encryption %u, compression %u, header size 0x%X, image size 0x%X, load 0x%08X\n",
        encryption, compression, headerSize, imageSize, loadAddress);
    {
        const uint32_t imageFlags = ReadBE32(&file[securityOffset + 0x10C]);
        const uint64_t page = (imageFlags & 0x10000000) ? 0x1000 : 0x10000;
        const uint32_t descriptors = ReadBE32(&file[securityOffset + 0x180]);
        if (securityOffset + 0x184 + uint64_t(descriptors) * 24 > file.size())
        {
            fprintf(stderr, "FATAL: page descriptors outside the file\n");
            return 1;
        }
        uint64_t pages = 0;
        for (uint32_t i = 0; i < descriptors; i++)
            pages += (ReadBE32(&file[securityOffset + 0x184 + i * 24]) >> 4) * page;
        Check(pages == imageSize, "page descriptors total == image_size", pages, imageSize);
    }
    if (compression == 1)
    {
        uint64_t total = 0, data = 0;
        const size_t blocks = formatSize / 8 - 1;
        for (size_t i = 0; i < blocks; i++)
        {
            total += uint64_t(ReadBE32(fileFormat + 8 + i * 8)) + ReadBE32(fileFormat + 12 + i * 8);
            data += ReadBE32(fileFormat + 8 + i * 8);
        }
        printf("  %zu basic-compression blocks\n", blocks);
        Check(total <= imageSize, "sum of block data+zero sizes <= image_size", total, imageSize);
        Check(data <= file.size() - headerSize, "block data fits in the file after the header", data, file.size() - headerSize);
        if (total < imageSize)
            printf("  note  image tail 0x%llX bytes past the blocks is zero-filled by the console loader\n",
                (unsigned long long)(imageSize - total));
        Check(total == imageSize || !kUpstreamLoader,
            "upstream Xex2LoadImage allocates only the block sum: loading would read past its buffer",
            total, imageSize);
    }
    else
    {
        printf("  (compression %u: no block-sum check)\n", compression);
    }
    if (failures)
        return 1;

    Image image = Xex2LoadImage(file.data(), file.size());
    if (!image.data)
    {
        fprintf(stderr, "FATAL: Xex2LoadImage returned no image\n");
        return 1;
    }

    const uint8_t* img = image.data.get();
    Check(img[0] == 'M' && img[1] == 'Z', "image starts with MZ", ReadLE16(img), 0x5A4D);
    const uint32_t lfanew = ReadLE32(img + 0x3C);
    Check(lfanew + 0xF8 <= imageSize, "e_lfanew + PE headers inside the image", lfanew, imageSize);
    if (failures)
        return 1;
    const uint8_t* nt = img + lfanew;
    Check(memcmp(nt, "PE\0\0", 4) == 0, "PE signature", ReadLE32(nt), 0x00004550);
    const uint8_t* opt = nt + 24;  // IMAGE_OPTIONAL_HEADER32
    Check(ReadLE32(opt + 56) == imageSize, "PE SizeOfImage == image_size", ReadLE32(opt + 56), imageSize);
    Check(ReadLE32(opt + 28) == loadAddress, "PE ImageBase == load address", ReadLE32(opt + 28), loadAddress);
    const uint16_t numSections = ReadLE16(nt + 6);
    const uint16_t optSize = ReadLE16(nt + 20);
    const uint8_t* sec = opt + optSize;
    for (uint16_t i = 0; i < numSections; i++, sec += 40)
    {
        char name[9] = {};
        memcpy(name, sec, 8);
        const uint64_t end = uint64_t(ReadLE32(sec + 12)) + ReadLE32(sec + 8);
        std::string what = std::string("section ") + name + " inside the image";
        Check(end <= imageSize, what.c_str(), end, imageSize);
    }
    if (failures)
        return 1;
    printf("image check: OK\n");

    printf("\n## Image\n\n");
    printf("base 0x%08zX, size 0x%08X, entry 0x%08zX\n\n", image.base, image.size, image.entry_point);
    printf("| Section | Start | End | Size | Kind |\n|---|---|---|---|---|\n");
    for (auto& s : image.sections)
    {
        printf("| %s | 0x%08zX | 0x%08zX | 0x%X | %s |\n", s.name.c_str(), s.base, s.base + s.size, s.size,
            (s.flags & SectionFlags_Code) ? "code" : "data");
    }

    std::string outPath = std::string(argv[2]) + "/image.bin";
    FILE* f = fopen(outPath.c_str(), "wb");
    if (f == nullptr || fwrite(image.data.get(), 1, image.size, f) != image.size)
    {
        fprintf(stderr, "FATAL: cannot write %s\n", outPath.c_str());
        return 1;
    }
    fclose(f);
    printf("\nwrote %s\n", outPath.c_str());

    // Imports: walk the import-library header on the raw file; read each thunk from the loaded
    // image. The loader overwrites function thunks and names them in the symbol table; variable
    // records keep their ordinal (type 0 = variable).
    printf("\n## Imports\n\n");
    const uint8_t* importHeader = FindOptHeader(file, 0x000103FF);
    std::map<std::string, std::set<std::string>> byLibrary;
    std::map<std::string, std::set<std::string>> variablesByLibrary;
    if (importHeader != nullptr)
    {
        uint32_t stringTableSize = ReadBE32(importHeader + 4);
        uint32_t numLibraries = ReadBE32(importHeader + 8);
        std::vector<std::string> names;
        const char* strings = reinterpret_cast<const char*>(importHeader + 12);
        size_t off = 0;
        for (uint32_t i = 0; i < numLibraries; i++)
        {
            names.emplace_back(strings + off);
            off += ((names.back().size() + 1) + 3) & ~size_t(3);
        }

        const uint8_t* lib = importHeader + 12 + stringTableSize;
        for (uint32_t i = 0; i < numLibraries; i++)
        {
            uint32_t libSize = ReadBE32(lib);
            uint16_t nameIndex = (lib[0x24] << 8) | lib[0x25];
            uint16_t count = (lib[0x26] << 8) | lib[0x27];
            const std::string& libName = names.at(nameIndex);
            const auto* table = libName == "xam.xex" ? &XamExports
                : libName == "xboxkrnl.exe" ? &XboxKernelExports : nullptr;

            for (uint16_t im = 0; im < count; im++)
            {
                uint32_t thunkAddr = ReadBE32(lib + 0x28 + im * 4);
                auto sym = image.symbols.find(thunkAddr);
                if (sym != image.symbols.end() && sym->address == thunkAddr)
                {
                    byLibrary[libName].insert(sym->name);
                    continue;
                }

                auto* thunk = static_cast<const uint8_t*>(image.Find(thunkAddr));
                if (thunk == nullptr)
                {
                    fprintf(stderr, "FATAL: import record 0x%08X of %s outside the image\n", thunkAddr, libName.c_str());
                    return 1;
                }

                // Loader byteswapped this record to host order.
                uint32_t data;
                memcpy(&data, thunk, 4);
                uint32_t ordinal = data & 0xFFFF;
                uint32_t type = data >> 24;
                const char* name = nullptr;
                if (table != nullptr)
                {
                    auto it = table->find(ordinal);
                    if (it != table->end())
                        name = it->second;
                }

                char buf[64];
                if (name == nullptr)
                {
                    snprintf(buf, sizeof(buf), "ordinal_%u", ordinal);
                    name = buf;
                }

                if (type == 0)
                    variablesByLibrary[libName].insert(name);
                else
                    byLibrary[libName].insert(name);
            }
            lib += libSize;
        }
    }

    size_t totalFunctions = 0, totalData = 0;
    for (auto& [lib, fns] : byLibrary)
    {
        // A variable record that pairs a named function is just its IAT slot.
        std::set<std::string> dataOnly;
        for (auto& v : variablesByLibrary[lib])
            if (!fns.count(v))
                dataOnly.insert(v);

        printf("### %s: %zu functions, %zu data imports\n\n", lib.c_str(), fns.size(), dataOnly.size());
        for (auto& fn : fns)
            printf("- %s\n", fn.c_str());
        for (auto& v : dataOnly)
            printf("- %s (data)\n", v.c_str());
        printf("\n");
        totalFunctions += fns.size();
        totalData += dataOnly.size();
    }
    printf("total distinct imports: %zu functions + %zu data\n", totalFunctions, totalData);

    // Compiler helpers: byte patterns from the XenonRecomp README.
    const Pattern patterns[] = {
        { "restgprlr_14_address", { 0xE9, 0xC1, 0xFF, 0x68 } },
        { "savegprlr_14_address", { 0xF9, 0xC1, 0xFF, 0x68 } },
        { "restfpr_14_address", { 0xC9, 0xCC, 0xFF, 0x70 } },
        { "savefpr_14_address", { 0xD9, 0xCC, 0xFF, 0x70 } },
        { "restvmx_14_address", { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x60, 0xCE } },
        { "savevmx_14_address", { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x61, 0xCE } },
        { "restvmx_64_address", { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x60, 0xCB } },
        { "savevmx_64_address", { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x61, 0xCB } },
    };

    printf("\n## Compiler helpers\n\n");
    for (auto& p : patterns)
    {
        std::vector<size_t> hits;
        for (auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            for (size_t i = 0; i + p.bytes.size() <= s.size; i += 4)
            {
                if (memcmp(s.data + i, p.bytes.data(), p.bytes.size()) == 0)
                    hits.push_back(s.base + i);
            }
        }

        printf("%s =", p.key);
        for (size_t h : hits)
            printf(" 0x%08zX", h);
        printf("%s\n", hits.empty() ? " (not found)" : hits.size() > 1 ? "   <-- multiple hits, check by hand" : "");
    }

    printf("\n## Import stubs\n\n");
    for (auto& sym : image.symbols)
    {
        if (sym.name.rfind("__imp__", 0) == 0)
            printf("0x%08zX %s\n", sym.address, sym.name.c_str());
    }

    return 0;
}
