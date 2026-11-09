// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only: see diag_texture_uploads.h.
#include "diag_texture_uploads.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace runtime::video {
namespace {

struct Key
{
    uint32_t physical, format, width, height, levels;
    bool operator<(const Key& o) const
    {
        return std::tie(physical, format, width, height, levels) < std::tie(o.physical, o.format, o.width, o.height, o.levels);
    }
};

struct Range
{
    uint32_t physical, bytes;
};

struct History
{
    uint64_t hash = 0, swap = 0, gpuSeq = 0;
    std::vector<Range> ranges;
    bool everGpuWritten = false;
};

enum Class { kFirst, kSameSwap, kEarlier, kGpuChanged, kCpuChanged, kClasses };
constexpr const char* kClassNames[kClasses] = {"first", "same-swap", "earlier", "gpu-changed", "cpu-changed"};

struct PerTexture
{
    uint64_t uploads = 0, bytes = 0, classBytes[kClasses] = {};
};

struct State
{
    bool enabled = false;
    std::map<Key, History> history;
    std::vector<std::pair<Range, uint64_t>> gpuWrites;  // (range, sequence), the last kMaxWrites
    uint64_t gpuSeq = 0, swap = 1;
    uint64_t uploads[kClasses] = {}, bytes[kClasses] = {}, gpuEverBytes = 0, gpuEverUploads = 0;
    std::set<std::pair<Key, uint64_t>> unique;
    uint64_t uniqueBytes = 0;
    std::map<Key, PerTexture> perTexture;
};
constexpr size_t kMaxWrites = 65536;

State& S()
{
    static State s = [] {
        State t;
        const char* v = std::getenv("FM4_DIAG_TEXTURE_UPLOADS");
        t.enabled = v != nullptr && *v != '\0';
        if (t.enabled)
            std::fprintf(stderr, "[diag-tex] FM4_DIAG_TEXTURE_UPLOADS: per-swap upload breakdown on (diagnostic; timing not representative)\n");
        return t;
    }();
    return s;
}

uint64_t Hash(const uint8_t* p, uint64_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h ^ n;
}

bool Overlaps(const Range& a, const Range& b)
{
    return uint64_t(a.physical) < uint64_t(b.physical) + b.bytes && uint64_t(b.physical) < uint64_t(a.physical) + a.bytes;
}

}  // namespace

bool DiagTextureUploadsEnabled() { return S().enabled; }

void DiagTextureGpuWrite(uint32_t physical, uint32_t bytes)
{
    State& s = S();
    if (!s.enabled || bytes == 0)
        return;
    if (s.gpuWrites.size() == kMaxWrites)
        s.gpuWrites.erase(s.gpuWrites.begin(), s.gpuWrites.begin() + kMaxWrites / 2);
    s.gpuWrites.push_back({{physical, bytes}, ++s.gpuSeq});
}

void DiagTextureUpload(const TextureDescription& d, const uint8_t* staging, uint64_t bytes)
{
    State& s = S();
    if (!s.enabled)
        return;
    const Key key{d.base.physical, uint32_t(d.host.vkFormat) << 8 | d.fetch.format, d.width, d.height, d.levelCount};
    const uint64_t hash = Hash(staging, bytes);
    std::vector<Range> ranges;
    for (const MipLevel& l : d.levels)
        ranges.push_back({l.address, uint32_t(std::min<uint64_t>(l.extentBytes, 0xFFFFFFFFull))});
    auto [it, fresh] = s.history.try_emplace(key);
    History& h = it->second;
    bool gpuSince = false, gpuEver = h.everGpuWritten;
    for (const auto& [w, seq] : s.gpuWrites)
        for (const Range& r : ranges)
            if (Overlaps(w, r))
            {
                gpuEver = true;
                if (seq > h.gpuSeq)
                    gpuSince = true;
            }
    Class c;
    if (fresh)
        c = kFirst;
    else if (hash == h.hash)
        c = h.swap == s.swap ? kSameSwap : kEarlier;
    else
        c = gpuSince ? kGpuChanged : kCpuChanged;
    h.hash = hash, h.swap = s.swap, h.gpuSeq = s.gpuSeq, h.ranges = ranges, h.everGpuWritten = gpuEver;
    s.uploads[c]++, s.bytes[c] += bytes;
    if (gpuEver)
        s.gpuEverUploads++, s.gpuEverBytes += bytes;
    if (s.unique.insert({key, hash}).second)
        s.uniqueBytes += bytes;
    PerTexture& t = s.perTexture[key];
    t.uploads++, t.bytes += bytes, t.classBytes[c] += bytes;
}

std::string DiagTextureSwapSummary()
{
    State& s = S();
    if (!s.enabled)
        return {};
    uint64_t total = 0, count = 0;
    for (int c = 0; c < kClasses; c++)
        total += s.bytes[c], count += s.uploads[c];
    std::string out;
    if (count != 0)
    {
        char line[512];
        std::snprintf(line, sizeof line, "[diag-tex] swap %llu: %llu upload(s), %.1f MB; unique (texture, content) %.1f MB;",
                      (unsigned long long)s.swap, (unsigned long long)count, total / 1048576.0, s.uniqueBytes / 1048576.0);
        out = line;
        for (int c = 0; c < kClasses; c++)
        {
            std::snprintf(line, sizeof line, " %s %llu / %.1f MB;", kClassNames[c], (unsigned long long)s.uploads[c], s.bytes[c] / 1048576.0);
            out += line;
        }
        std::snprintf(line, sizeof line, " resolve-written textures %llu / %.1f MB", (unsigned long long)s.gpuEverUploads, s.gpuEverBytes / 1048576.0);
        out += line;
        std::vector<std::pair<Key, PerTexture>> top(s.perTexture.begin(), s.perTexture.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        for (size_t i = 0; i < top.size() && i < 5; i++)
        {
            const auto& [k, t] = top[i];
            int dominant = 0;
            for (int c = 1; c < kClasses; c++)
                if (t.classBytes[c] > t.classBytes[dominant])
                    dominant = c;
            std::snprintf(line, sizeof line, "\n[diag-tex]   top %zu: %08X fmt %u (vk %u) %ux%u x%u levels: %llu upload(s), %.1f MB (mostly %s)%s", i + 1,
                          k.physical, k.format & 0xFF, k.format >> 8, k.width, k.height, k.levels, (unsigned long long)t.uploads, t.bytes / 1048576.0,
                          kClassNames[dominant], s.history[k].everGpuWritten ? ", resolve-written" : "");
            out += line;
        }
    }
    s.swap++;
    for (int c = 0; c < kClasses; c++)
        s.uploads[c] = s.bytes[c] = 0;
    s.gpuEverBytes = s.gpuEverUploads = s.uniqueBytes = 0;
    s.unique.clear();
    s.perTexture.clear();
    return out;
}

}  // namespace runtime::video
