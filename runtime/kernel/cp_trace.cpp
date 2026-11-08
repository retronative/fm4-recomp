// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The command-processor trace (cp_trace.h). Register indices: Xenia 67f16c4 gpu/register_table.inc
// (RB_SURFACE_INFO 2000, RB_COLOR_INFO 2001, RB_DEPTH_INFO 2002, RB_COLOR1_INFO 2003, PA_SC_WINDOW_SCISSOR_BR 2082,
// RB_MODECONTROL 2208).
#include "cp_trace.h"
#include "graphics.h"
#include "command_processor.h"
#include "imports.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace runtime::gpu {
namespace {

constexpr uint64_t kCap = uint64_t(64) << 20;
std::mutex traceLock;
FILE* file = nullptr;
uint64_t written = 0;
bool opened = false, capped = false;

FILE* Open()
{
    if (!opened)
    {
        opened = true;
        if (const char* path = std::getenv("FM4_CP_TRACE"); path != nullptr && *path != '\0')
        {
            file = std::fopen(path, "w");
            if (file != nullptr)  // line-buffered: a stop _exits without flushing, and the failing draw's lines must survive
                std::setvbuf(file, nullptr, _IOLBF, 1 << 16);
        }
    }
    return capped ? nullptr : file;
}

void Line(const char* text, int length)
{
    if (length <= 0)
        return;
    if (written + uint64_t(length) > kCap)
    {
        std::fputs("trace capped at 64 MB\n", file);
        std::fflush(file);
        capped = true;
        return;
    }
    std::fwrite(text, 1, size_t(length), file);
    written += uint64_t(length);
}

}  // namespace

bool TraceEnabled()
{
    std::lock_guard guard(traceLock);
    return Open() != nullptr;
}

void TraceDraw(const uint8_t* base, uint64_t draw, uint32_t opcode, uint32_t vizQueryToken)
{
    std::lock_guard guard(traceLock);
    if (Open() == nullptr)
        return;
    char text[384];
    // The draw packet (VGT_DRAW_INITIATOR 21FC, after the CP wrote it) and the state the draw decoder reads (runtime/video/
    // draw_state.h): RB_COLOR_MASK 2104, RB_DEPTHCONTROL 2200, RB_BLENDCONTROL0 2201, RB_COLORCONTROL 2202, PA_CL_CLIP_CNTL
    // 2204, PA_SU_SC_MODE_CNTL 2205, PA_CL_VTE_CNTL 2206, SQ_PROGRAM_CNTL 2180 (Xenia register_table.inc).
    Line(text, std::snprintf(text, sizeof text,
                             "draw %llu op %02X surface %08X color %08X depth %08X color1 %08X mode %08X scissor_br %08X initiator %08X "
                             "colormask %08X depthctl %08X blend0 %08X colorctl %08X clip %08X sumode %08X vte %08X program %08X\n",
                             (unsigned long long)draw, opcode, ReadRegister(base, 0x2000), ReadRegister(base, 0x2001), ReadRegister(base, 0x2002),
                             ReadRegister(base, 0x2003), ReadRegister(base, 0x2208), ReadRegister(base, 0x2082), ReadRegister(base, 0x21FC),
                             ReadRegister(base, 0x2104), ReadRegister(base, 0x2200), ReadRegister(base, 0x2201), ReadRegister(base, 0x2202),
                             ReadRegister(base, 0x2204), ReadRegister(base, 0x2205), ReadRegister(base, 0x2206), ReadRegister(base, 0x2180)));
    // The rest of the decoder's inputs (runtime/video/draw_state.h): DRAW_INDX's viz-query token, PA_SC_VIZ_QUERY 2293,
    // VGT_OUTPUT_PATH_CNTL 2284, SQ_INTERPOLATOR_CNTL 2182, SQ_VS_CONST 2307, SQ_PS_CONST 2308, PA_SC_WINDOW_OFFSET 2080,
    // PA_SC_WINDOW_SCISSOR_TL 2081, PA_SU_VTX_CNTL 2302, RB_STENCILREFMASK 210D, PA_CL_VPORT_XSCALE..ZOFFSET 210F-2114.
    Line(text, std::snprintf(text, sizeof text,
                             "state viz %08X %08X outpath %08X interp %08X vsconst %08X psconst %08X window %08X %08X vtxcntl %08X stencilref %08X "
                             "vport %08X %08X %08X %08X %08X %08X\n",
                             vizQueryToken, ReadRegister(base, 0x2293), ReadRegister(base, 0x2284), ReadRegister(base, 0x2182), ReadRegister(base, 0x2307),
                             ReadRegister(base, 0x2308), ReadRegister(base, 0x2080), ReadRegister(base, 0x2081), ReadRegister(base, 0x2302),
                             ReadRegister(base, 0x210D), ReadRegister(base, 0x210F), ReadRegister(base, 0x2110), ReadRegister(base, 0x2111),
                             ReadRegister(base, 0x2112), ReadRegister(base, 0x2113), ReadRegister(base, 0x2114)));
    // DRAW_INDX (the textured draws so far): texture fetch constants of slots 0-2 (6 dwords each at 4800 + 6 * slot, Xenia
    // FETCH_00_0; the pixel shaders FM4's first screens load use slots 0-2 at most, reference/gpu/texture.md).
    // Clip-disabled draws with an 8192 x 8192 scissor (the fast clears whose extent Xenia's DrawExtentEstimator takes from the
    // vertices, reference/gpu/edram_extent.md): vf0's first three vertices, 7 dwords each from the fetch constant's address
    // (dword 0 bits 2-31, in dwords), as the guest stores them (big-endian words; the fetch's endian is dword 1 bits 0-1).
    // Also every resolve (RB_MODECONTROL 6): Xenia takes the resolve rectangle from these vertices (draw_util.cc GetResolveInfo).
    if ((((ReadRegister(base, 0x2204) >> 16 & 1) && ReadRegister(base, 0x2082) == 0x20002000) || (ReadRegister(base, 0x2208) & 7) == 6) &&
        (ReadRegister(base, 0x4800) & 3) == 3)
    {
        const uint32_t address = ReadRegister(base, 0x4800) & ~3u, endian = ReadRegister(base, 0x4801) & 3;
        const uint32_t guest = PhysicalToGuest(address);
        int n = std::snprintf(text, sizeof text, "vf0 endian %u at %08X:", endian, address);
        for (uint32_t i = 0; i < 21 && n < int(sizeof text) - 10; i++)
            n += std::snprintf(text + n, sizeof text - n, " %08X", Load32(base, guest + 4 * i));
        std::snprintf(text + n, sizeof text - n, "\n");
        Line(text, int(std::strlen(text)));
    }
    // Display gamma ramp registers (Xenia register_table.inc: DC_LUT_RW_MODE 1921, RW_INDEX 1922, SEQ_COLOR 1923, PWL_DATA 1924,
    // 30_COLOR 1925, WRITE_EN_MASK 1927, DC_LUTA_CONTROL 1930), at a few draws: whether FM4 programs a ramp.
    if (draw == 0 || draw == 1000 || draw == 10000 || draw == 20000)
        Line(text, std::snprintf(text, sizeof text, "dclut draw %llu mode %08X index %08X seq %08X pwl %08X c30 %08X wen %08X ctl %08X\n",
                                 (unsigned long long)draw, ReadRegister(base, 0x1921), ReadRegister(base, 0x1922), ReadRegister(base, 0x1923),
                                 ReadRegister(base, 0x1924), ReadRegister(base, 0x1925), ReadRegister(base, 0x1927), ReadRegister(base, 0x1930)));
    // Resolves (RB_MODECONTROL 2208 = 6): RB_COPY_CONTROL 2318, RB_COPY_DEST_BASE 2319, RB_COPY_DEST_PITCH 231A, RB_COPY_DEST_INFO
    // 231B, RB_COPY_DEST_OFFSET 231D..231F, PA_SC_WINDOW_OFFSET 2080 / WINDOW_SCISSOR_TL 2081, and fetch constant 0 (the
    // resolve's destination texture; reference/gpu/edram.md: these were in no trace before).
    if ((ReadRegister(base, 0x2208) & 7) == 6)
        Line(text, std::snprintf(text, sizeof text, "resolve copy %08X %08X %08X %08X offset %08X %08X %08X window %08X %08X fetch0 %08X %08X %08X %08X %08X %08X\n",
                                 ReadRegister(base, 0x2318), ReadRegister(base, 0x2319), ReadRegister(base, 0x231A), ReadRegister(base, 0x231B),
                                 ReadRegister(base, 0x231D), ReadRegister(base, 0x231E), ReadRegister(base, 0x231F), ReadRegister(base, 0x2080),
                                 ReadRegister(base, 0x2081), ReadRegister(base, 0x4800), ReadRegister(base, 0x4801), ReadRegister(base, 0x4802),
                                 ReadRegister(base, 0x4803), ReadRegister(base, 0x4804), ReadRegister(base, 0x4805)));
    if (opcode == 0x22)
        for (uint32_t slot = 0; slot < 3; slot++)
            Line(text, std::snprintf(text, sizeof text, "tex %u %08X %08X %08X %08X %08X %08X\n", slot, ReadRegister(base, 0x4800 + 6 * slot),
                                     ReadRegister(base, 0x4801 + 6 * slot), ReadRegister(base, 0x4802 + 6 * slot), ReadRegister(base, 0x4803 + 6 * slot),
                                     ReadRegister(base, 0x4804 + 6 * slot), ReadRegister(base, 0x4805 + 6 * slot)));
}

void TraceTextures(const uint8_t* base, uint32_t opcode, uint32_t slotMask)
{
    std::lock_guard guard(traceLock);
    if (Open() == nullptr)
        return;
    char text[128];
    for (uint32_t slot = 0; slot < 32; slot++)
        if ((slotMask >> slot & 1) && !(opcode == 0x22 && slot < 3))
            Line(text, std::snprintf(text, sizeof text, "tex %u %08X %08X %08X %08X %08X %08X\n", slot, ReadRegister(base, 0x4800 + 6 * slot),
                                     ReadRegister(base, 0x4801 + 6 * slot), ReadRegister(base, 0x4802 + 6 * slot), ReadRegister(base, 0x4803 + 6 * slot),
                                     ReadRegister(base, 0x4804 + 6 * slot), ReadRegister(base, 0x4805 + 6 * slot)));
}

void TraceShader(uint32_t type, const uint32_t* code, uint32_t words, uint64_t hash, bool immediate)
{
    std::lock_guard guard(traceLock);
    if (Open() == nullptr)
        return;
    // FM4_SHADER_DUMP=<dir> (with the trace on): each distinct shader once, as survey/shader_export.py writes the
    // disc's (<vs|ps>_<hash>.bin, big-endian words), for the offline census. Capped at 4096 files. Game data: local only.
    static std::set<uint64_t> dumped;
    if (const char* dir = std::getenv("FM4_SHADER_DUMP"); dir && dumped.size() < 4096 && dumped.insert(hash ^ type).second)
    {
        char name[64];
        std::snprintf(name, sizeof name, "/%s_%016llx.bin", type == 0 ? "vs" : "ps", (unsigned long long)hash);
        if (FILE* f = std::fopen((std::string(dir) + name).c_str(), "wb"))
        {
            for (uint32_t i = 0; i < words; i++)
            {
                const uint8_t be[4] = {uint8_t(code[i] >> 24), uint8_t(code[i] >> 16), uint8_t(code[i] >> 8), uint8_t(code[i])};
                std::fwrite(be, 1, 4, f);
            }
            std::fclose(f);
        }
    }
    char text[160];
    Line(text, std::snprintf(text, sizeof text, "shader %s type %u words %u hash %016llx\n", immediate ? "immediate" : "pointer", type, words,
                             (unsigned long long)hash));
}

}  // namespace runtime::gpu
