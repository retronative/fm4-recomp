// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9): FM4_INTERNAL_RES, read once (render_scale.h).
#include "render_scale.h"
#include "stops.h"
#include <cstdio>
#include <cstdlib>

namespace runtime::video::scale {

uint32_t InternalHalves()
{
    // FM4 Recomp (M9, live resolution, step 1): the value goes to render_scale.h's detail::currentHalves (runtime state) instead of
    // a constant here; the read, its two Stops and its log lines still happen once, at the first call.
    static const bool read = [] {
        uint32_t h = kHalves1x;
        std::string error;
        if (!ParseInternalRes(std::getenv("FM4_INTERNAL_RES"), h, error))
            Stop(StopCode::Setup, "%s", error.c_str());
        bool native = true;
        if (!ParseInternalOutput(std::getenv("FM4_INTERNAL_OUTPUT"), native, error))
            Stop(StopCode::Setup, "%s", error.c_str());
        if (h > kHalves1x)
            std::fprintf(stderr, "[renderer] display image %ux%u (%s)\n", native ? HostEdge(1280, h) : 1280, native ? HostEdge(720, h) : 720,
                         native ? "the internal resolution: the scaled front buffer; a swap without one is its 1x image enlarged, counted"
                                : "FM4_INTERNAL_OUTPUT=720p: the 1x front buffer, the scaled targets box-downsampled");
        if (h > kHalves1x)
            std::fprintf(stderr, "[renderer] internal resolution %sx (FM4_INTERNAL_RES=%s): EDRAM host targets %sx the guest size on both axes "
                                 "(a 1280x720 surface renders at %ux%u) with the guest's sample counts (FM4_HOST_MSAA_1X=1: 1 sample); resolves write 1x words to guest "
                                 "memory (box-downsampled)\n",
                         HalvesName(h), std::getenv("FM4_INTERNAL_RES"), HalvesName(h), HostEdge(1280, h), HostEdge(720, h));
        detail::currentHalves.store(h, std::memory_order_relaxed);  // what InternalHalvesQuiet stored, if it ran first
        return true;
    }();
    (void)read;
    return detail::currentHalves.load(std::memory_order_relaxed);
}

}  // namespace runtime::video::scale
