// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9 diagnostic, opt-in, off by default): FM4_DIAG_RES_CYCLE=<res>[,<res>...] (FM4_INTERNAL_RES's spellings: 720p, fhd,
// qhd, 4k) with FM4_DIAG_RES_WINDOW=<swaps> (1..100000, default 300) asks for the next resolution of the list (renderer.h
// RequestInternalRes) at every swap whose number is a multiple of the window, as the GRAPHICS menu would, so one bounded offscreen
// run exercises repeated switches of the internal resolution under validation. The first request, at the first multiple, is the
// list's first entry. It needs FM4_GRAPHICS_LIVE=1, as the menu does. A probe only, never a setting.
#pragma once
#include "render_scale.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace runtime::video::diag {

// The resolution to ask for at `swap`, or null (off, or not a window's first swap).
inline const char* ResCycleAt(uint64_t swap)
{
    struct Cycle
    {
        std::vector<std::string> list;
        uint64_t window = 300;
    };
    static const Cycle cycle = [] {
        Cycle c;
        const char* v = std::getenv("FM4_DIAG_RES_CYCLE");
        if (v == nullptr || *v == '\0')
            return c;
        for (std::string rest = v; !rest.empty();)
        {
            const size_t comma = rest.find(',');
            const std::string one = rest.substr(0, comma);
            rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
            uint32_t halves = 0;
            std::string error;
            if (one.empty() || !scale::ParseInternalRes(one.c_str(), halves, error))
            {
                std::fprintf(stderr, "[diag res] FM4_DIAG_RES_CYCLE '%s': '%s' is not a resolution (720p, fhd, qhd, 4k): off\n", v, one.c_str());
                return Cycle{};
            }
            c.list.push_back(one);
        }
        if (const char* w = std::getenv("FM4_DIAG_RES_WINDOW"); w != nullptr && *w != '\0')
        {
            const unsigned long n = std::strtoul(w, nullptr, 10);
            if (n < 1 || n > 100000)
            {
                std::fprintf(stderr, "[diag res] FM4_DIAG_RES_WINDOW '%s' is not 1..100000: off\n", w);
                return Cycle{};
            }
            c.window = n;
        }
        std::fprintf(stderr, "[diag res] on (FM4_DIAG_RES_CYCLE=%s): the next of %zu resolution(s) is asked for at every swap that is a multiple of %llu "
                             "(DIAGNOSTIC)\n", v, c.list.size(), (unsigned long long)c.window);
        return c;
    }();
    if (cycle.list.empty() || swap == 0 || swap % cycle.window != 0)
        return nullptr;
    return cycle.list[size_t((swap / cycle.window - 1) % cycle.list.size())].c_str();
}

}  // namespace runtime::video::diag
