// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (FM4_LOAD_WAITS_RELEASES=1; config/title_hooks.toml "resource heap: allocate"). When the camera's area changes, FM4
// queues the old area's resources for release on the main thread, which releases a few per frame, and starts the new area's loads on
// its loader threads. On the console the loads are paced by the disc, so the releases come first. Here a load takes milliseconds:
// when the main thread spends the next frame busy (the first race frame takes several hundred milliseconds), the loaders fill the
// track resource heap before anything is released, the heap returns null, and the game drops that model or texture data without a
// retry: the road near the car is never drawn (evidence in the hook's comment). With this hook such a failed allocation waits for
// the requests queued for other threads and is retried, so the order the game relies on holds. Nothing changes while the heap has
// room.
#include "release_wait.h"
#include "title_symbols.h"
#include "ppc_recomp_shared.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace fm4::title {

void HeapAllocateWaitsForReleases(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const uint64_t heap = ctx.r3.u64, size = ctx.r4.u64, alignment = ctx.r5.u64, tag = ctx.r6.u64;
    original(ctx, base);
    if (ctx.r3.u32 != 0 || uint32_t(heap) != sym::kResourceHeap)
        return;
    const uint32_t manager = PPC_LOAD_U32(sym::kResourceManagerPointer);
    if (manager == 0)
        return;
    const uint32_t self = PPC_LOAD_U32(PPC_LOAD_U32(ctx.r13.u32 + sym::kCurrentThread) + sym::kThreadId);
    const RequestQueues layout{sym::kRequestThreads, sym::kRequestThreadTable, sym::kRequestQueueFirst, sym::kRequestQueueStride};
    constexpr uint32_t kLimitMs = 10000;
    static std::atomic<bool> gaveUpShared{false};
    static std::atomic<uint64_t> waits{0};
    bool gaveUp = gaveUpShared.load();
    const bool gaveUpBefore = gaveUp;
    uint32_t waitedMs = 0, firstPending = 0;
    const bool served = WaitForReleases(
        [&] {
            const uint32_t pending = PendingForOthers(layout, manager, self, [&](uint32_t address) { return PPC_LOAD_U32(address); });
            if (firstPending == 0)
                firstPending = pending;
            return pending;
        },
        [&] {
            ctx.r3.u64 = heap, ctx.r4.u64 = size, ctx.r5.u64 = alignment, ctx.r6.u64 = tag;
            original(ctx, base);
            return ctx.r3.u32 != 0;
        },
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }, gaveUp, kLimitMs, waitedMs);
    gaveUpShared.store(gaveUp);
    if (waitedMs == 0 && !(gaveUp && !gaveUpBefore))
        return;
    const uint64_t n = ++waits;
    if (gaveUp && !gaveUpBefore)
        std::fprintf(stderr, "[load] resource heap: an allocation of %u bytes waited %u ms for %u bytes of queued requests and was not served; "
                             "later failures do not wait until the queues are empty\n", uint32_t(size), waitedMs, firstPending);
    else if (n <= 8 || (n & (n - 1)) == 0)
        std::fprintf(stderr, "[load] resource heap: wait %llu: an allocation of %u bytes waited %u ms for queued requests (%u bytes at first), %s\n",
                     (unsigned long long)n, uint32_t(size), waitedMs, firstPending, served ? "then succeeded" : "and still failed");
}

}  // namespace fm4::title
