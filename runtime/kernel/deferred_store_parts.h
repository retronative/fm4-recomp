// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, opt-in FM4_ASYNC_RETIRE=1, with FM4_DEFER_FENCES): the command processor's deferred-store queue (command_processor.cpp
// DeferredStore) split by the submission part each entry follows, so that a part's stores can land once that part retired, before
// the whole slot is retired. The queue itself stays on the command processor's thread, where it is produced and consumed today
// (its WAIT_REG_MEM self-wait check reads it unchanged); only the boundaries are new. Std only: tests/runtime/test_async_retire.cpp.
//
// Rules (each tested):
//   P1 Submitted(serial) closes the entries queued since the previous boundary as part `serial` (nothing when none were queued).
//   P2 Entries queued after the last boundary belong to no part yet (the open part): ApplyThrough never takes them; the full drain
//      (ApplyDeferredStores, after every part retired) does.
//   P3 ApplyThrough takes entries from the front only, one at a time and before applying each, so a nested apply (a store's own
//      flush) continues in queue order; it stops at the first entry of a part after `serial`.
//   P4 It stops at an interrupt: an interrupt and everything queued behind it wait for the full drain, which raises it after every
//      prior part (the idle-asked rule: an interrupt after a WAIT_UNTIL waits for every prior part) and only then lands the stores
//      behind it, exactly as today.
//   P5 It stops at a store `canStore` refuses (the GPU read guard: a later part still reads its page): that store and every entry
//      behind it stay queued for a later reap or the full drain; nothing overtakes it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace runtime::gpu {

class StorePartition
{
public:
    void Queued() { queued_++; }
    // P1: at a part's submission (command processor's thread).
    void Submitted(uint64_t serial)
    {
        if (queued_ > taken_ && (parts_.empty() || parts_.back().end != queued_))
            parts_.push_back({serial, queued_});
    }
    // The full drain took every entry.
    void TakenAll()
    {
        taken_ = queued_;
        parts_.clear();
    }
    void Taken() { taken_++; }
    // Whether the front entry belongs to a part up to `serial` (P2: an entry after the last boundary belongs to none).
    bool FrontThrough(uint64_t serial)
    {
        while (!parts_.empty() && parts_.front().end <= taken_)
            parts_.pop_front();
        return !parts_.empty() && parts_.front().serial <= serial;
    }
    size_t Parts() const { return parts_.size(); }

private:
    struct Part { uint64_t serial, end; };  // end: the queue count after its last entry
    std::deque<Part> parts_;
    uint64_t queued_ = 0, taken_ = 0;
};

// P3-P5: the entries of parts up to `serial`, from the front of `queue`, applied in order. Returns how many.
template <class Entry, class CanStore, class Apply>
size_t ApplyStoresThrough(std::vector<Entry>& queue, StorePartition& parts, uint64_t serial, CanStore canStore, Apply apply)
{
    size_t applied = 0;
    while (!queue.empty() && parts.FrontThrough(serial) && !queue.front().interrupt && canStore(queue.front()))
    {
        const Entry e = queue.front();
        queue.erase(queue.begin());  // taken before it is applied (P3)
        parts.Taken();
        apply(e);
        applied++;
    }
    return applied;
}

}  // namespace runtime::gpu
