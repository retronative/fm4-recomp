// FM4 Recomp (FM4_NO_DRAIN): which resolves the command processor waits for before the next fence or interrupt.
//
// Adapted from the ShiftGlue SDK, src/graphics/fm4_resolve_readbacks.cpp (Fm4ResolveReadbacks::Resolve: runs of resolves to one range
// with gaps of at most kRunGapFrames frames, and the "extra" rule, more resolves to a range this frame than in the run's previous
// frame), Copyright (c) 2026 i2kiselev, BSD 3-Clause License (THIRD_PARTY_NOTICES.md). Our change, more conservative than the SDK's
// selective readbacks: a resolve waits until its run has had kSteadyResolves resolves, and an extra one always waits. So a range's
// first captures wait, and so does one re-captured a few frames after its first (the SDK's home-space environment map,
// fm4-static-recomp #52: it is the run's second resolve) or drawn once more into one of the frame's own targets (#399); a capture
// repeated every frame or every other frame (double-buffered) stops waiting from its third resolve. Those land when their part
// retires, so the guest CPU may read the previous capture of the range there (not guest-exact).
#pragma once

#include <cstdint>
#include <unordered_map>

namespace runtime::video {

class CaptureRuns
{
public:
    static constexpr uint64_t kRunGapFrames = 4;
    static constexpr uint32_t kSteadyResolves = 3;

    // A resolve to [address, address + length) in frame `frame` (frames never decrease): true when the command processor must wait
    // for its write-back before the next fence or interrupt.
    bool Resolve(uint32_t address, uint32_t length, uint64_t frame)
    {
        const uint64_t key = (uint64_t(address) << 32) | length;
        const auto [it, inserted] = ranges_.try_emplace(key);
        Range& r = it->second;
        if (inserted || frame > r.frame + kRunGapFrames)
            r = Range{frame, 0, 0, 0};
        else if (frame != r.frame)
            r.previousCount = r.count, r.count = 0, r.frame = frame;
        r.count++;
        r.resolves += r.resolves < kSteadyResolves ? 1 : 0;
        const bool extra = r.previousCount != 0 && r.count > r.previousCount;
        return r.resolves < kSteadyResolves || extra;
    }

private:
    // `frame`: the run's latest frame; `count`: resolves in it; `previousCount`: in the run's frame before (0: none); `resolves`: in the
    // run so far (capped at kSteadyResolves).
    struct Range { uint64_t frame = 0; uint32_t count = 0, previousCount = 0, resolves = 0; };
    std::unordered_map<uint64_t, Range> ranges_;
};

}  // namespace runtime::video
