// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic (off unless set): FM4_CAPTURE_KEEP_EVERY=<n> keeps every n-th captured swap in a second capped ring
// (keep_<id>.png beside the 16-frame ring; renderer.cpp kKeepRecords / kKeepBytes), so the screens a run passes through
// survive, not only its last quarter second. A value that is not a decimal number from 1 to 1,000,000 is malformed (the
// renderer stops: a mistyped interval is not silently replaced).
#pragma once
#include <cstdint>

namespace runtime::video {

struct KeepEvery { uint64_t every; bool malformed; };

inline KeepEvery KeepEveryFromEnv(const char* text)
{
    if (text == nullptr || *text == '\0')
        return {0, false};
    uint64_t n = 0;
    for (const char* p = text; *p; p++)
    {
        if (*p < '0' || *p > '9' || n > 1000000)
            return {0, true};
        n = n * 10 + uint64_t(*p - '0');
    }
    return n >= 1 && n <= 1000000 ? KeepEvery{n, false} : KeepEvery{0, true};
}

}  // namespace runtime::video
