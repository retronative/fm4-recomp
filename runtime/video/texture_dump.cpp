// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See texture_dump.h. Diagnostic only, behind FM4_DUMP_TEXTURE.
#include "texture_dump.h"
#include "stops.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace runtime::video {

namespace {
std::string dumpDir;
bool parsed = false, enabled = false;
uint32_t address = 0, dumps = 0;
uint32_t matchWidth = 0, matchHeight = 0;  // FM4_DUMP_TEXTURE=<W>x<H>: match by size (guest addresses move between runs)

bool WriteFile(const std::string& path, const void* bytes, size_t size)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool written = std::fwrite(bytes, 1, size, f) == size;
    return std::fclose(f) == 0 && written;
}

void Parse()
{
    parsed = true;
    const char* env = std::getenv("FM4_DUMP_TEXTURE");
    if (env == nullptr || *env == '\0')
        return;
    if (const char* x = std::strchr(env, 'x'); x != nullptr)
    {
        char* wend = nullptr;
        char* hend = nullptr;
        const unsigned long w = std::strtoul(env, &wend, 10), h = std::strtoul(x + 1, &hend, 10);
        if (wend != x || hend == nullptr || *hend != '\0' || w == 0 || h == 0 || w > 8192 || h > 8192)
            Stop(StopCode::Setup, "FM4_DUMP_TEXTURE=%s is neither an address nor <width>x<height>", env);
        if (dumpDir.empty())
            Stop(StopCode::Setup, "FM4_DUMP_TEXTURE is set but the renderer has no capture directory (--gpu-capture)");
        matchWidth = uint32_t(w), matchHeight = uint32_t(h), enabled = true;
        std::fprintf(stderr, "[diag] FM4_DUMP_TEXTURE: the first %u loads of a %ux%u texture go to %s (a diagnostic run)\n", kMaxTextureDumps,
                     matchWidth, matchHeight, dumpDir.c_str());
        return;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(env, &end, 16);
    if (std::string(env).size() != 8 || end == nullptr || *end != '\0' || (v & 0xFFF) != 0 || v >= 0x20000000ull)
        Stop(StopCode::Setup, "FM4_DUMP_TEXTURE=%s is not an 8-hex-digit, 4 KB-aligned physical base address below 512 MB", env);
    if (dumpDir.empty())
        Stop(StopCode::Setup, "FM4_DUMP_TEXTURE is set but the renderer has no capture directory (--gpu-capture)");
    address = uint32_t(v);
    enabled = true;
    std::fprintf(stderr, "[diag] FM4_DUMP_TEXTURE: the first %u loads of the texture at %08X go to %s (a diagnostic run)\n", kMaxTextureDumps, address,
                 dumpDir.c_str());
}
}  // namespace

void SetTextureDumpDir(const char* dir) { dumpDir = dir != nullptr ? dir : ""; }

void MaybeDumpTexture(const uint32_t words[6], const TextureDescription& d, const TextureBinding& binding, const LoadPlan& plan, GuestBytesFn guest,
                      void* context, const uint8_t* staged)
{
    if (!parsed)
        Parse();
    if (!enabled || dumps >= kMaxTextureDumps)
        return;
    if (matchWidth != 0 ? (d.width != matchWidth || d.height != matchHeight) : d.base.physical != address)
        return;
    address = d.base.physical;  // the dump's name and log lines carry the matched texture's address
    uint64_t total = plan.stagingBytes;
    for (const MipLevel& l : d.levels)
        total += l.extentBytes;
    char prefix[64];
    std::snprintf(prefix, sizeof prefix, "/texdump_%08X_%u", address, dumps);
    const std::string base = dumpDir + prefix;
    dumps++;
    if (total > kMaxTextureDumpBytes)
    {
        std::fprintf(stderr, "[diag] FM4_DUMP_TEXTURE: dump %u of %08X skipped (%llu bytes > %llu)\n", dumps - 1, address, (unsigned long long)total,
                     (unsigned long long)kMaxTextureDumpBytes);
        return;
    }
    std::string info;
    char line[512];
    std::snprintf(line, sizeof line, "words %08X %08X %08X %08X %08X %08X\n", words[0], words[1], words[2], words[3], words[4], words[5]);
    info += line;
    std::snprintf(line, sizeof line, "format %u %s dimension %u size %u %u %u tiled %d endian %u bytes_per_block %u levels %u packed_level %d\n",
                  d.fetch.format, d.info.name, d.dimension, d.width, d.height, d.depth, int(d.fetch.tiled), d.fetch.endian, 1u << d.log2BytesPerBlock,
                  d.levelCount, d.packedLevel == UINT32_MAX ? -1 : int(d.packedLevel));
    info += line;
    std::snprintf(line, sizeof line, "host_format %u components %u %u %u %u gamma %d exp_adjust %d sampler mag %u min %u mip %u address %u %u %u "
                                     "min_lod %g max_lod %g border %u\n",
                  d.host.vkFormat, binding.components[0], binding.components[1], binding.components[2], binding.components[3], int(binding.gamma),
                  binding.expAdjust, binding.sampler.magFilter, binding.sampler.minFilter, binding.sampler.mipmapMode, binding.sampler.addressModeU,
                  binding.sampler.addressModeV, binding.sampler.addressModeW, double(binding.sampler.minLod), double(binding.sampler.maxLod),
                  binding.sampler.borderColor);
    info += line;
    bool ok = true;
    for (size_t k = 0; k < d.levels.size(); k++)
    {
        const MipLevel& l = d.levels[k];
        std::snprintf(line, sizeof line,
                      "level %zu address %08X x %u y %u blocks %u %u depth %u texels %u %u pitch_blocks %u row_pitch_bytes %u z_stride_rows %u "
                      "extent %zu staged_offset %zu staged_bytes %zu\n",
                      k, l.address, l.x, l.y, l.widthBlocks, l.heightBlocks, l.depth, l.widthTexels, l.heightTexels, l.pitchBlocks, l.rowPitchBytes,
                      l.zStrideBlockRows, l.extentBytes, plan.levelOffsets[k], plan.levelBytes[k]);
        info += line;
        std::string error;
        const uint8_t* bytes = guest(context, l.address, uint32_t(l.extentBytes), error);
        ok = ok && bytes != nullptr && WriteFile(base + "_level" + std::to_string(k) + "_guest.bin", bytes, l.extentBytes);
    }
    ok = ok && WriteFile(base + "_staged.bin", staged, plan.stagingBytes) && WriteFile(base + "_info.txt", info.data(), info.size());
    std::fprintf(stderr, "[diag] FM4_DUMP_TEXTURE: dump %u of %08X %s (%s_*)\n", dumps - 1, address, ok ? "written" : "FAILED", base.c_str());
}

}  // namespace runtime::video
