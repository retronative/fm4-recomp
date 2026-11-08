// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See ps_trace.h. Diagnostic only, behind FM4_TRACE_PS.
#include "ps_trace.h"
#include "stops.h"
#include "kernel/graphics.h"
#include "kernel/virtual_memory.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace runtime::video {

namespace {

bool parsed = false, active = false;
uint64_t wanted[8] = {};
uint32_t wantedCount = 0;
uint64_t lastTraced = ~0ull;  // the draw TracePsDraw traced last (TracePsBind follows it)
uint32_t traced = 0;
uint64_t fromDraw = 0;
uint32_t traceCap = kPsTraceCap;  // FM4_TRACE_PS_CAP=<n> (at most 4096) raises it  // FM4_TRACE_PS_FROM=<decimal draw number>: trace only from this draw on (later screens)

float F(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// IEEE half -> float (the translator's xe_half: shader_glsl.cpp kHalfHelper; exponent 31 is flagged there, printed here as inf/nan).
float Half(uint32_t h)
{
    const uint32_t sign = h >> 15 & 1, exponent = h >> 10 & 31, mantissa = h & 1023;
    float v = exponent == 0 ? std::ldexp(float(mantissa), -24) : exponent == 31 ? (mantissa ? NAN : INFINITY) : std::ldexp(float(mantissa | 1024), int(exponent) - 25);
    return sign ? -v : v;
}

// shader_glsl.cpp kVertexHeader xe_swap: 1 8in16, 2 8in32, 3 16in32, applied to the 32-bit little-endian load of the guest bytes
// (the storage buffer word the shader reads: the renderer copies guest memory bytes into the ring unchanged).
uint32_t XeSwap(uint32_t v, uint32_t e)
{
    if (e == 1 || e == 2)
        v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2 || e == 3)
        v = (v << 16) | (v >> 16);
    return v;
}

void Constant(const uint8_t* base, std::string& out, const char* name, uint32_t index)
{
    char s[128];
    const uint32_t r = 0x4000 + 4 * index;
    std::snprintf(s, sizeof s, " %s=(%.9g %.9g %.9g %.9g)", name, double(F(runtime::gpu::ReadRegister(base, r))), double(F(runtime::gpu::ReadRegister(base, r + 1))),
                  double(F(runtime::gpu::ReadRegister(base, r + 2))), double(F(runtime::gpu::ReadRegister(base, r + 3))));
    out += s;
}

}  // namespace

void TracePsDraw(const uint8_t* base, uint64_t draw, uint64_t vsHash, uint64_t psHash, const shader::ShaderTranslation& vs)
{
    if (!parsed)
    {
        parsed = true;
        const char* env = std::getenv("FM4_TRACE_PS");
        if (env != nullptr && *env != '\0')
        {
            // A comma-separated list of 16-hex-digit hashes (at most 8); anything else stops (fail closed).
            const std::string list = env;
            for (size_t at = 0; at <= list.size();)
            {
                const size_t comma = std::min(list.find(',', at), list.size());
                const std::string item = list.substr(at, comma - at);
                char* end = nullptr;
                const uint64_t h = std::strtoull(item.c_str(), &end, 16);
                if (item.size() != 16 || end == nullptr || *end != '\0' || wantedCount == 8)
                    Stop(StopCode::Setup, "FM4_TRACE_PS=%s is not a list of at most 8 16-hex-digit pixel shader hashes", env);
                wanted[wantedCount++] = h;
                at = comma + 1;
            }
            active = true;
            if (const char* from = std::getenv("FM4_TRACE_PS_FROM"); from != nullptr && *from != '\0')
            {
                char* fend = nullptr;
                fromDraw = std::strtoull(from, &fend, 10);
                if (fend == nullptr || *fend != '\0')
                    Stop(StopCode::Setup, "FM4_TRACE_PS_FROM=%s is not a decimal draw number", from);
            }
            if (const char* cap = std::getenv("FM4_TRACE_PS_CAP"); cap != nullptr && *cap != '\0')
            {
                char* cend = nullptr;
                const unsigned long n = std::strtoul(cap, &cend, 10);
                if (cend == nullptr || *cend != '\0' || n == 0 || n > 4096)
                    Stop(StopCode::Setup, "FM4_TRACE_PS_CAP=%s is not a count from 1 to 4096", cap);
                traceCap = uint32_t(n);
            }
            std::fprintf(stderr, "[ps-trace] tracing draws with %u pixel shader hash(es), first ps_%016llx, from draw %llu (at most %u)\n", wantedCount,
                         (unsigned long long)wanted[0], (unsigned long long)fromDraw, traceCap);
        }
    }
    if (!active || draw < fromDraw || traced >= traceCap || std::find(wanted, wanted + wantedCount, psHash) == wanted + wantedCount)
        return;
    traced++;
    lastTraced = draw;
    char s[256];
    std::string line;
    std::snprintf(s, sizeof s, "[ps-trace] draw %llu vs_%016llx ps_%016llx alpha_ref %.9g (%08X) window_offset %08X window_scissor_tl %08X",
                  (unsigned long long)draw, (unsigned long long)vsHash, (unsigned long long)psHash, double(F(runtime::gpu::ReadRegister(base, 0x210E))),
                  runtime::gpu::ReadRegister(base, 0x210E), runtime::gpu::ReadRegister(base, 0x2080), runtime::gpu::ReadRegister(base, 0x2081));
    line = s;
    std::fprintf(stderr, "%s\n", line.c_str());
    line = "[ps-trace] draw " + std::to_string(draw) + " vs";
    for (uint32_t n = 0; n <= 23; n++)
        Constant(base, line, ("c" + std::to_string(n)).c_str(), n);
    std::fprintf(stderr, "%s\n", line.c_str());
    line = "[ps-trace] draw " + std::to_string(draw) + " ps";
    for (const uint32_t k : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 254u, 255u})  // c4-c7: the glow PS 93879bb4's alpha is (c5.x * texel.w)^2
        Constant(base, line, ("c" + std::to_string(k)).c_str(), 256 + k);
    std::fprintf(stderr, "%s\n", line.c_str());
    line = "[ps-trace] draw " + std::to_string(draw) + " bool";
    for (uint32_t i = 0; i < 8; i++)
        std::snprintf(s, sizeof s, " %08X", runtime::gpu::ReadRegister(base, 0x4900 + i)), line += s;
    line += " loop";
    for (uint32_t i = 0; i < 32; i++)
        std::snprintf(s, sizeof s, " %08X", runtime::gpu::ReadRegister(base, 0x4908 + i)), line += s;
    std::fprintf(stderr, "%s\n", line.c_str());
    // Vertex fetch slot 95 as the VS reads it: stride and element end from the translation (vertexFetchStrides / Ends).
    const uint32_t w0 = runtime::gpu::ReadRegister(base, 0x4800 + 2 * 95), w1 = runtime::gpu::ReadRegister(base, 0x4800 + 2 * 95 + 1);
    uint32_t stride = 0, end = 0;
    for (size_t i = 0; i < vs.vertexFetchSlots.size(); i++)
        if (vs.vertexFetchSlots[i] == 95)
            stride = vs.vertexFetchStrides[i], end = vs.vertexFetchEnds[i];
    std::snprintf(s, sizeof s, "[ps-trace] draw %llu vf95 %08X %08X (address %08X, %u dwords, endian %u) stride %u end %u", (unsigned long long)draw, w0, w1,
                  w0 & ~3u, (w1 >> 2) & 0xFFFFFF, w1 & 3, stride, end);
    std::fprintf(stderr, "%s\n", s);
    for (uint32_t v = 0; v < 3 && end != 0 && (w0 & 3) == 3; v++)
    {
        line = "[ps-trace] draw " + std::to_string(draw) + " vf95 vertex " + std::to_string(v) + ":";
        for (uint32_t d = 0; d < end; d++)
        {
            const uint32_t at = v * stride + d;
            uint32_t guest = 0;
            if (at >= ((w1 >> 2) & 0xFFFFFF) || !runtime::PhysicalToGuest((w0 & ~3u) + 4 * at, guest))
            {
                line += " (outside)";
                break;
            }
            uint32_t le = 0;
            std::memcpy(&le, base + guest, 4);
            const uint32_t word = XeSwap(le, w1 & 3);
            std::snprintf(s, sizeof s, " [%u] %08X half(lo %.6g hi %.6g) float %.9g", d, word, double(Half(word & 0xFFFF)), double(Half(word >> 16)),
                          double(F(word)));
            line += s;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

void TracePsBind(uint64_t draw, const PsBindTrace& b)
{
    if (!active || draw != lastTraced)
        return;
    std::fprintf(stderr,
                 "[ps-trace] draw %llu bind depthctl %08X sumode %08X target pitch %u msaa %u colour base %u fmt %u written %d depth base %u fmt %u "
                 "written %d pitch_tiles %u edram_rows %u height %u key depth used %u write %u func %u stencil %u samples %u host %ux%u x%u "
                 "scissor (%d, %d, %ux%u) viewport %gx%g depth %g..%g colorctl %08X vport %08X %08X %08X %08X %08X %08X clip %08X vte %08X vtxcntl %08X "
                 "xe_flags %u fold z %.9g + %.9g*w ps defines [%s]%s\n",
                 (unsigned long long)draw, b.depthControl, b.suScModeCntl, b.pitch, b.msaa, b.colorBase, b.colorFormat, int(b.colorWritten), b.depthBase,
                 b.depthFormat, int(b.depthWritten), b.pitchTiles, b.edramRows, b.targetHeight, b.keyDepthUsed, b.keyDepthWrite, b.keyDepthFunc, b.keyStencil,
                 b.keySamples, b.hostWidth, b.hostHeight, b.hostSamples, b.scissorX, b.scissorY, b.scissorWidth, b.scissorHeight, double(b.viewportWidth),
                 double(b.viewportHeight), double(b.minDepth), double(b.maxDepth), b.colorControl, b.vport[0], b.vport[1], b.vport[2], b.vport[3], b.vport[4], b.vport[5], b.clipCntl, b.vteCntl, b.vtxCntl, b.systemFlags,
                 double(b.foldZScale), double(b.foldZOffset), b.psDefines, b.noDepthDiagnostic ? " (FM4_DIAG_PS_NO_DEPTH: depth test and write off)" : "");
}

}  // namespace runtime::video
