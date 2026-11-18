// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (FM4_DIAG_CP_DELAY_NS=<n>, never a default): a busy-wait of n nanoseconds on the command processor at every draw, a
// sensitivity test of the frame to command-processor CPU time (if the frame grows about as much as the added CP time, CP work is on the
// critical path; if it grows much less, it is not). Rendering is unchanged.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace runtime::video::diag {

inline uint64_t CpDelayNs()
{
    static const uint64_t ns = [] {
        const char* v = std::getenv("FM4_DIAG_CP_DELAY_NS");
        const uint64_t n = v != nullptr && *v != '\0' ? std::strtoull(v, nullptr, 10) : 0;
        if (n != 0)
            std::fprintf(stderr, "[diag] FM4_DIAG_CP_DELAY_NS=%llu: the command processor busy-waits that long at every draw\n", (unsigned long long)n);
        return n;
    }();
    return ns;
}

inline void CpDelay()
{
    const uint64_t ns = CpDelayNs();
    if (ns == 0)
        return;
    const auto end = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
    while (std::chrono::steady_clock::now() < end)
        ;
}

}  // namespace runtime::video::diag
