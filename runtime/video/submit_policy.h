// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7): when the renderer submits the open part at a draw entry (renderer.cpp SplitSubmit). Pure, CPU-tested
// (tests/runtime/test_submit_policy.cpp).
//   bySize (FM4_SUBMIT_SPLIT=<split>): the part holds `split` draws; with FM4_SPLIT_AT_PASS_END, inside a render pass only once it
//     holds 4 x `split`.
//   byIdle (FM4_SUBMIT_IF_IDLE=<idle>; SpeedBreaker 312ce69 runtime/gpu/renderer.cpp:10848-10859 SubmitIfIdle, technique adapted):
//     not bySize, the part holds `idle` draws (4 x `idle` inside a render pass) and no submitted part still runs. `queueEmpty` is asked
//     last, only when the draw counts allow a split (it queries fences).
#pragma once
#include <cstdint>

namespace runtime::video {

struct SplitDecision
{
    bool bySize = false, byIdle = false;
    bool Split() const { return bySize || byIdle; }
};

template <class QueueEmpty>
SplitDecision DecideSplit(uint64_t draws, uint32_t split, bool splitAtPassEnd, bool inPass, uint32_t idle, QueueEmpty queueEmpty)
{
    SplitDecision d;
    if (split == 0)
        return d;
    d.bySize = draws >= split && !(splitAtPassEnd && inPass && draws < 4ull * split);
    d.byIdle = !d.bySize && idle != 0 && draws >= idle && (!inPass || draws >= 4ull * idle) && queueEmpty();
    return d;
}

}  // namespace runtime::video
