// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (FM4_LOAD_WAITS_RELEASES): what a failed allocation from the game's track resource heap does while resource requests
// are still queued for other threads (release_wait.cpp has the reason). Pure logic, no guest call: tests/runtime/test_release_wait.cpp.
#pragma once
#include <cstdint>

namespace fm4::title {

// The game's resource manager keeps one request queue per registered thread: `threads` thread ids at manager + threadTable, and
// the queue of entry i at manager + (i + queueFirst) * queueStride, holding its begin and end pointers in its first two words.
struct RequestQueues
{
    uint32_t threads, threadTable, queueFirst, queueStride;
};

// The bytes of requests queued for every registered thread but `self`. `load` reads a big-endian guest word.
template <typename Load>
uint32_t PendingForOthers(const RequestQueues& layout, uint32_t manager, uint32_t self, Load load)
{
    uint32_t pending = 0;
    for (uint32_t i = 0; i < layout.threads; i++)
    {
        const uint32_t thread = load(manager + layout.threadTable + 4 * i);
        if (thread == 0 || thread == self)
            continue;
        const uint32_t queue = manager + (i + layout.queueFirst) * layout.queueStride, begin = load(queue), end = load(queue + 4);
        if (end > begin)
            pending += end - begin;
    }
    return pending;
}

// After an allocation failed: retries it every `sleep()` (one millisecond) while other threads still have requests queued, and
// once more when none are left (the last release may have landed after the failure); then the failure stands, as in the game.
// A wait that reaches `limitMs` sets `gaveUp`, and no later failure waits again until the queues have been seen empty, so a
// thread that can never be served costs the limit once, not once per allocation. Returns whether a retry succeeded, and the
// milliseconds waited in `waitedMs`.
template <typename Pending, typename Retry, typename Sleep>
bool WaitForReleases(Pending pending, Retry retry, Sleep sleep, bool& gaveUp, uint32_t limitMs, uint32_t& waitedMs)
{
    waitedMs = 0;
    for (;;)
    {
        if (pending() == 0)
        {
            gaveUp = false;
            return retry();
        }
        if (gaveUp)
            return false;
        if (waitedMs >= limitMs)
        {
            gaveUp = true;
            return false;
        }
        sleep();
        waitedMs++;
        if (retry())
            return true;
    }
}

}  // namespace fm4::title
