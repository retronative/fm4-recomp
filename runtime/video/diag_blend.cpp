// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See diag_blend.h. Diagnostic only, behind FM4_DIAG_BLEND_LOG and FM4_DIAG_PS_SKIP.
#include "diag_blend.h"
#include "stops.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace runtime::video {

namespace {
bool parsed = false, skipAnnounced = false;
int64_t logFrom = -1;
uint64_t skip[8] = {};
uint32_t skipCount = 0;
std::set<uint64_t> seen;
uint32_t texBases[4] = {};
uint32_t texBaseCount = 0;
std::set<std::string> texSeen;

void Parse()
{
    parsed = true;
    if (const char* env = std::getenv("FM4_DIAG_BLEND_LOG"); env != nullptr && *env != '\0')
    {
        char* end = nullptr;
        const long long v = std::strtoll(env, &end, 10);
        if (end == nullptr || *end != '\0' || v < 0)
            Stop(StopCode::Setup, "FM4_DIAG_BLEND_LOG=%s is not a vblank number", env);
        logFrom = v;
        std::fprintf(stderr, "[diag] FM4_DIAG_BLEND_LOG: blended draw states from vblank %lld (diagnostic)\n", v);
    }
    if (const char* env = std::getenv("FM4_DIAG_TEX_USERS"); env != nullptr && *env != '\0')
    {
        const std::string list = env;
        for (size_t at = 0; at <= list.size();)
        {
            const size_t comma = std::min(list.find(',', at), list.size());
            const std::string item = list.substr(at, comma - at);
            char* end = nullptr;
            const unsigned long v = std::strtoul(item.c_str(), &end, 16);
            if (item.size() != 8 || end == nullptr || *end != '\0' || texBaseCount == 4 || (v & 0xFFF))
                Stop(StopCode::Setup, "FM4_DIAG_TEX_USERS=%s is not a list of at most 4 8-hex-digit, 4 KB-aligned texture base addresses", env);
            texBases[texBaseCount++] = uint32_t(v);
            at = comma + 1;
        }
        std::fprintf(stderr, "[diag] FM4_DIAG_TEX_USERS: the shader pairs binding %u texture base(s) are logged (diagnostic)\n", texBaseCount);
    }
    if (const char* env = std::getenv("FM4_DIAG_PS_SKIP"); env != nullptr && *env != '\0')
    {
        const std::string list = env;
        for (size_t at = 0; at <= list.size();)
        {
            const size_t comma = std::min(list.find(',', at), list.size());
            const std::string item = list.substr(at, comma - at);
            char* end = nullptr;
            const uint64_t h = std::strtoull(item.c_str(), &end, 16);
            if (item.size() != 16 || end == nullptr || *end != '\0' || skipCount == 8)
                Stop(StopCode::Setup, "FM4_DIAG_PS_SKIP=%s is not a list of at most 8 16-hex-digit pixel shader hashes", env);
            skip[skipCount++] = h;
            at = comma + 1;
        }
    }
}
}  // namespace

void DiagBlendLog(uint64_t draw, const PipelineKey& k, uint64_t vblank)
{
    if (!parsed)
        Parse();
    if (logFrom < 0 || int64_t(vblank) < logFrom || !(k.colorUsed & 1))
        return;
    const PipelineKeyColor& c = k.color[0];
    if (c.srcColor == 1 && c.dstColor == 0 && c.colorOp == 0 && c.srcAlpha == 1 && c.dstAlpha == 0 && c.alphaOp == 0)
        return;  // colour 0 does not blend
    PipelineKey state = k;
    state.vsVariant = state.psVariant = state.samplerLayout = 0;
    if (!seen.insert(state.Hash()).second)
        return;
    std::fprintf(stderr,
                 "[diag blend] draw %llu vblank %llu vs_%016llx ps_%016llx: target format %u mask %X; colour src %u dst %u op %u, alpha src %u dst %u op %u; "
                 "depth used %u write %u func %u bias %u (format %u); stencil %u front %u/%u/%u/%u back %u/%u/%u/%u; cull front %u back %u cw %u; topology %u\n",
                 (unsigned long long)draw, (unsigned long long)vblank, (unsigned long long)k.vsHash, (unsigned long long)k.psHash, c.format, c.writeMask,
                 c.srcColor, c.dstColor, c.colorOp, c.srcAlpha, c.dstAlpha, c.alphaOp, k.depthUsed, k.depthWrite, k.depthFunc, k.depthBias, k.depthFormat,
                 k.stencilEnable, k.stencilFront[0], k.stencilFront[1], k.stencilFront[2], k.stencilFront[3], k.stencilBack[0], k.stencilBack[1],
                 k.stencilBack[2], k.stencilBack[3], k.cullFront, k.cullBack, k.frontClockwise, k.topology);
}

bool DiagSkipDraw(uint64_t psHash, uint64_t draw)
{
    if (!parsed)
        Parse();
    if (skipCount == 0 || std::find(skip, skip + skipCount, psHash) == skip + skipCount)
        return false;
    if (!skipAnnounced)
    {
        skipAnnounced = true;
        std::fprintf(stderr, "[diag] FM4_DIAG_PS_SKIP: draw %llu of ps_%016llx NOT drawn (and every later one of the %u listed hash(es)); a diagnostic "
                             "run, not a rendering mode\n", (unsigned long long)draw, (unsigned long long)psHash, skipCount);
    }
    return true;
}

void DiagTextureUser(uint64_t draw, uint64_t vsHash, uint64_t psHash, uint32_t slot, const uint32_t (&w)[6])
{
    if (!parsed)
        Parse();
    if (texBaseCount == 0)
        return;
    const uint32_t base = w[1] & 0xFFFFF000u;
    if (std::find(texBases, texBases + texBaseCount, base) == texBases + texBaseCount)
        return;
    char key[64];
    std::snprintf(key, sizeof key, "%016llx%016llx%u", (unsigned long long)vsHash, (unsigned long long)psHash, slot);
    if (!texSeen.insert(key).second)
        return;
    std::fprintf(stderr, "[diag tex-user] draw %llu vs_%016llx ps_%016llx slot %u base %08X words %08X %08X %08X %08X %08X %08X\n", (unsigned long long)draw,
                 (unsigned long long)vsHash, (unsigned long long)psHash, slot, base, w[0], w[1], w[2], w[3], w[4], w[5]);
}

}  // namespace runtime::video
