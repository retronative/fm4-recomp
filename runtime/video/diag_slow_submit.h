// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (FM4_DIAG_SLOW_SUBMIT=1; never a default): wall-clock-stamped lines for (1) a queue submission whose
// queue-lock wait or vkQueueSubmit (MoltenVK's Metal encoding, and any pipeline it builds lazily, run inside it) takes at
// least kSlowSubmitNs, (2) a swap whose fence wait reaches kLongFenceMs, and (3) a swap of at least kLongSwapMs (a stall). The stamps line up with the system log's
// Metal "Compiling Shader" events, which carry wall-clock times too.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace runtime::video::diag {

constexpr uint64_t kSlowSubmitNs = 50'000'000;
constexpr double kLongFenceMs = 100.0;
constexpr double kLongSwapMs = 250.0;

inline bool SlowSubmitEnabled()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_SLOW_SUBMIT");
        const bool b = v != nullptr && v[0] == '1';
        if (b)
            std::fprintf(stderr, "[diag] slow submits and long fence waits stamped (FM4_DIAG_SLOW_SUBMIT=1)\n");
        return b;
    }();
    return on;
}

// "HH:MM:SS.mmm" of the local wall clock.
inline void WallStamp(char (&out)[16])
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t s = std::chrono::system_clock::to_time_t(now);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
    localtime_r(&s, &tm);
    std::snprintf(out, sizeof out, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

inline void ReportSubmit(uint32_t slot, uint64_t lockNs, uint64_t submitNs)
{
    if (lockNs < kSlowSubmitNs && submitNs < kSlowSubmitNs)
        return;
    char t[16];
    WallStamp(t);
    std::fprintf(stderr, "[diag] %s slow submit: slot %u, queue lock %.1f ms, vkQueueSubmit %.1f ms\n", t, slot, lockNs / 1e6, submitNs / 1e6);
}

inline void ReportFence(unsigned long long swap, double fenceMs, double gpuBusyMs)
{
    if (fenceMs < kLongFenceMs)
        return;
    char t[16];
    WallStamp(t);
    std::fprintf(stderr, "[diag] %s long fence wait: swap %llu, fences %.1f ms, GPU busy %.1f ms\n", t, swap, fenceMs, gpuBusyMs);
}

// `glslMs`/`glsl` and `pipelineMs`/`pipelines`: this swap's shader-variant compiles and pipeline creations (diag_compile.h).
inline void ReportSwap(unsigned long long swap, double frameMs, double waitMs, double glslMs = 0, uint64_t glsl = 0, double pipelineMs = 0,
                       uint64_t pipelines = 0, uint64_t glslCached = 0, double translateMs = 0, uint64_t translations = 0, double dumpMs = 0,
                       uint64_t dumps = 0)
{
    if (frameMs < kLongSwapMs)
        return;
    char t[16];
    WallStamp(t);
    std::fprintf(stderr, "[diag] %s long swap: swap %llu, %.1f ms since the last swap, submit + wait %.1f ms; compiles: %llu variant(s) + %llu from the "
                         "disk cache %.1f ms, %llu pipeline(s) %.1f ms, %llu translation(s) %.1f ms, %llu shader dump file(s) %.1f ms\n",
                 t, swap, frameMs, waitMs, (unsigned long long)glsl, (unsigned long long)glslCached, glslMs, (unsigned long long)pipelines, pipelineMs,
                 (unsigned long long)translations, translateMs, (unsigned long long)dumps, dumpMs);
}

}  // namespace runtime::video::diag
