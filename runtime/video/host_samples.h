// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7 GPU cost; opt-in FM4_HOST_MSAA_1X=1; after
// SpeedBreaker 312ce69 runtime/gpu/renderer.cpp:611, :2944, one sample everywhere): the host sample count of an EDRAM key's image.
// Off (default): the guest's (1x 1, 2x 2, 4x 4: native multisampled images, edram_vk.cpp GetTarget). On: 1 for every key, at the
// same host pixel size (Key::Width x Key::Height); the guest model (keys, tile ownership, MsaaMode in the key arithmetic, pitches,
// the transfer and resolve sample arithmetic of edram_vk_plan.h) is unchanged. Only two places derive a host sample count from the
// guest's, both through HostSampleCount: edram_vk.cpp GetTarget (the image, its render passes and framebuffers) and draw_record.cpp
// KeyFromDrawState (the draw's render pass and pipeline). Every other consumer copies Target::samples or HostTargets::samples, and a
// 1-sample source takes the non-multisampled shader variants, whose texelFetch reads sample 0.
#pragma once
#include "render_scale.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace runtime::video {

// The host image's sample count for a key whose guest MSAA mode has `guestSamples` samples (1, 2 or 4).
constexpr uint32_t HostSampleCount(uint32_t guestSamples, bool host1x)
{
    return host1x ? 1u : guestSamples;
}

// FM4_HOST_MSAA_1X=1, read once; one line when on. FM4 Recomp (M9): an internal resolution above 1x (FM4_INTERNAL_RES, render_scale.h)
// keeps the guest's sample counts (2x and 4x keys stay multisampled images at the scale times their guest size); with
// FM4_HOST_MSAA_1X=1 as well every key is a 1-sample image at that size, so a 4x key's host pixels stand for its guest samples
// (supersampling instead of MSAA; research/m9/qhd_plan/RECARBON_MAP.md 5, the first QHD runs).
inline bool HostMsaa1x()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_HOST_MSAA_1X");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[edram] host 1x MSAA on (FM4_HOST_MSAA_1X=1): 2x and 4x EDRAM keys get 1-sample host images (the key's pixel size%s); "
                                 "transfers and resolves read the one host sample\n",
                         scale::InternalHalvesQuiet() > scale::kHalves1x ? " times the internal scale" : "");
        return enabled;
    }();
    return on;
}

}  // namespace runtime::video
