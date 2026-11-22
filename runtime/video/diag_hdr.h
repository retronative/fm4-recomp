// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9 diagnostic, opt-in, off by default): FM4_DIAG_HDR_TOGGLE=<swaps> (1..100000) switches the HDR preference (renderer.h
// SetHdrPreference) at every swap whose number is a multiple of it, as the GRAPHICS menu would, so one bounded offscreen run exercises
// repeated OFF/ON switches under validation. A probe only, never a setting.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace runtime::video::diag {

inline uint64_t HdrToggleEvery()
{
    static const uint64_t every = [] {
        const char* v = std::getenv("FM4_DIAG_HDR_TOGGLE");
        if (v == nullptr || *v == '\0')
            return uint64_t(0);
        const unsigned long n = std::strtoul(v, nullptr, 10);
        if (n < 1 || n > 100000)
        {
            std::fprintf(stderr, "[diag hdr] FM4_DIAG_HDR_TOGGLE '%s' is not 1..100000: off\n", v);
            return uint64_t(0);
        }
        std::fprintf(stderr, "[diag hdr] on (FM4_DIAG_HDR_TOGGLE=%lu): HDR output is switched at every swap that is a multiple of it (DIAGNOSTIC)\n", n);
        return uint64_t(n);
    }();
    return every;
}

}  // namespace runtime::video::diag
