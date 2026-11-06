// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// A diagnostic trace of the headless command processor (M4 unit 0, approved execution model D-i): off unless
// FM4_CP_TRACE names a file. One line per draw (its surface registers, to confirm FM4's 4xMSAA / 1x EDRAM tile
// aliasing on our disc) and per shader load (type, size, and the census hash survey/fxobj.py computes, ReCarbon's
// recipe: FNV-1a-64 one 32-bit word per step, seed cbf29ce484222325 ^ type (vertex 0, pixel 1), each word loaded
// little-endian from the guest's big-endian bytes; prime 100000001b3). Capped at 64 MB (an uncapped dump once filled ~50 GB): past the
// cap one final line, then nothing. Not part of any stop line or result.
#pragma once
#include "shader_hash.h"
#include <cstdint>

namespace runtime::gpu {

// ShaderHash: kernel/shader_hash.h (shared with runtime/video/shader_cache.h).
bool TraceEnabled();
void TraceDraw(const uint8_t* base, uint64_t draw, uint32_t opcode, uint32_t vizQueryToken);
// FM4_SHADER_DUMP=<dir> also writes each distinct shader once (<vs|ps>_<hash>.bin, big-endian words; capped at 4096).
void TraceShader(uint32_t type, const uint32_t* code, uint32_t words, uint64_t hash, bool immediate);

}  // namespace runtime::gpu
