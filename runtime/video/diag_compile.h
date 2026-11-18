// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (with FM4_DIAG_SLOW_SUBMIT=1; never a default): the time the command processor spent this swap compiling shader
// variants (GLSL -> SPIR-V, glslang) and creating graphics pipelines (vkCreateGraphicsPipelines: MoltenVK's SPIR-V -> MSL and the
// Metal compile), so a long swap names its first-use compilation. Counted always (two relaxed atomics per compile, not per draw);
// printed only beside a long swap (diag_slow_submit.h).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace runtime::video::diag {

struct CompileTally
{
    std::atomic<uint64_t> glslNs{0}, glsl{0}, glslCached{0}, pipelineNs{0}, pipelines{0};  // glslCached: FM4_SHADER_CACHE hits
    std::atomic<uint64_t> translateNs{0}, translations{0}, dumpNs{0}, dumps{0};  // Xenos -> GLSL; FM4_SHADER_DUMP file writes
};
inline CompileTally& Compiles()
{
    static CompileTally t;
    return t;
}
inline uint64_t CompileClockNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace runtime::video::diag
