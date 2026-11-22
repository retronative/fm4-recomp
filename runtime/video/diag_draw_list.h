// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9 diagnostic, opt-in, off by default; Extended View's question of which draws are the HUD): FM4_DIAG_DRAW_LIST=
// <swap>[,<swap>...] lists every draw of those frames, one line each (renderer.cpp Draw): its shaders, render target, viewport and
// scissor, in the order the game issues them. A probe only.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <set>

namespace runtime::video::diag {

inline bool DrawListSwap(uint64_t swap)
{
    static const std::set<uint64_t> swaps = [] {
        std::set<uint64_t> s;
        const char* v = std::getenv("FM4_DIAG_DRAW_LIST");
        for (const char* at = v; at != nullptr && *at;)
        {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(at, &end, 10);
            if (end == at)
                break;
            s.insert(uint64_t(n));
            at = *end == ',' ? end + 1 : end;
        }
        return s;
    }();
    return !swaps.empty() && swaps.count(swap) != 0;
}

}  // namespace runtime::video::diag
