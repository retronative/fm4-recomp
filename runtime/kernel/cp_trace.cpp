// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The command-processor trace (cp_trace.h). Register indices: Xenia 67f16c4 gpu/register_table.inc
// (RB_SURFACE_INFO 2000, RB_COLOR_INFO 2001, RB_DEPTH_INFO 2002, RB_COLOR1_INFO 2003, PA_SC_WINDOW_SCISSOR_BR 2082,
// RB_MODECONTROL 2208).
#include "cp_trace.h"
#include "graphics.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>

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
            file = std::fopen(path, "w");
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

uint64_t ShaderHash(uint32_t type, const uint32_t* words, uint32_t count)
{
    uint64_t hash = 0xcbf29ce484222325ull ^ type;
    for (uint32_t i = 0; i < count; i++)
        hash = (hash ^ __builtin_bswap32(words[i])) * 0x100000001b3ull;  // a little-endian load of big-endian bytes
    return hash;
}

bool TraceEnabled()
{
    std::lock_guard guard(traceLock);
    return Open() != nullptr;
}

void TraceDraw(const uint8_t* base, uint64_t draw, uint32_t opcode)
{
    std::lock_guard guard(traceLock);
    if (Open() == nullptr)
        return;
    char text[256];
    Line(text, std::snprintf(text, sizeof text, "draw %llu op %02X surface %08X color %08X depth %08X color1 %08X mode %08X scissor_br %08X\n",
                             (unsigned long long)draw, opcode, ReadRegister(base, 0x2000), ReadRegister(base, 0x2001), ReadRegister(base, 0x2002),
                             ReadRegister(base, 0x2003), ReadRegister(base, 0x2208), ReadRegister(base, 0x2082)));
}

void TraceShader(uint32_t type, uint32_t words, uint64_t hash, bool immediate)
{
    std::lock_guard guard(traceLock);
    if (Open() == nullptr)
        return;
    char text[160];
    Line(text, std::snprintf(text, sizeof text, "shader %s type %u words %u hash %016llx\n", immediate ? "immediate" : "pointer", type, words,
                             (unsigned long long)hash));
}

}  // namespace runtime::gpu
