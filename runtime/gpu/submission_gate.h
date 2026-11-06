// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp (forzarecomp-recarbon) runtime/video/submission_gate.h
// (GPL-3.0-or-later, same owner), reviewed and re-tested here (tests/run_gpu_safety_tests.sh).
// FM4 Recomp changes: namespace runtime::gpu; Strand is MarkStranded; Enter also refuses once a
// submission is stranded (fail closed: after an unknown completion nothing more reaches the GPU;
// ForzaRecomp left that refusal to its lifetime model); no process-wide instance yet (the renderer
// will declare it).
//
// Orders GPU submissions against the process stop. Every stop ends with _exit, which tears the
// process down without waiting for the GPU; a submission still executing then has its resources
// reclaimed under it. The gate counts the submissions whose completion is not yet established, and
// the stop waits (bounded) until none is left before exiting.
//
// One atomic word holds the count and two flags, so a submission and a stop are ordered by a
// single transition: Enter succeeds only while both flags are clear (compare-and-swap), and
// BeginStop sets the stopping flag in one step. Either the submission entered first (and the stop
// waits for it) or the stop came first (and nothing more is submitted).
//
// A submission whose completion is unknown (a failed submit, a fence wait that failed or timed
// out) is stranded: it stays counted, the stranded flag is set, and the stop does not wait for it
// (the submitter's own bounded wait already gave up); the stop reports it instead.
//
// Pure bookkeeping, no GPU calls. Load and BeginStop are lock-free atomic operations and safe in a
// signal handler; AwaitIdle sleeps and is not.
#pragma once
#include <atomic>
#include <cstdint>

namespace runtime::gpu {

class SubmissionGate
{
public:
    static constexpr uint32_t kStopping = 1u << 31, kStranded = 1u << 30, kCountMask = kStranded - 1;

    struct View
    {
        bool stopping = false, stranded = false;
        uint32_t inFlight = 0;  // stranded submissions included
    };
    enum class Outcome
    {
        Idle,       // nothing in flight when the stop began
        Completed,  // the stop waited, and every submission in flight completed
        Stranded,   // a submission's completion is unknown: not waited for
        TimedOut,   // submissions were still in flight when the wait's limit passed
        OwnThread,  // the stopping thread has a submission in flight itself: waiting would wait for itself
    };

    constexpr SubmissionGate() = default;
    SubmissionGate(const SubmissionGate&) = delete;
    SubmissionGate& operator=(const SubmissionGate&) = delete;

    // Immediately before the queue submission. False once a stop has begun or a submission is
    // stranded, or when the count is full: nothing may be submitted.
    bool Enter()
    {
        uint32_t s = state_.load(std::memory_order_acquire);
        do
        {
            if ((s & (kStopping | kStranded)) || (s & kCountMask) == kCountMask)
                return false;
        } while (!state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire));
        own_++;
        return true;
    }
    // The entered submission's fence signalled: its completion is confirmed.
    void Leave()
    {
        own_--;
        state_.fetch_sub(1, std::memory_order_acq_rel);
    }
    // The entered submission's completion is unknown: it stays counted for good, and the stop
    // reports it instead of waiting.
    void MarkStranded()
    {
        own_--;
        state_.fetch_or(kStranded, std::memory_order_acq_rel);
    }

    View Load() const
    {
        const uint32_t s = state_.load(std::memory_order_acquire);
        return View{(s & kStopping) != 0, (s & kStranded) != 0, s & kCountMask};
    }
    // Refuses every later Enter. True for the call that began the stop.
    bool BeginStop() { return (state_.fetch_or(kStopping, std::memory_order_acq_rel) & kStopping) == 0; }

    // After BeginStop: waits until nothing is in flight, polling with `sleep()` until `now()` has
    // advanced `limitNs` from the first call. Never waits for a stranded submission or for one the
    // calling thread entered itself.
    template <typename Now, typename Sleep>
    Outcome AwaitIdle(uint64_t limitNs, Now now, Sleep sleep) const
    {
        const uint64_t start = now();
        bool waited = false;
        for (;;)
        {
            const View v = Load();
            if (v.stranded)
                return Outcome::Stranded;
            if (v.inFlight == 0)
                return waited ? Outcome::Completed : Outcome::Idle;
            if (own_ > 0)
                return Outcome::OwnThread;
            if (now() - start >= limitNs)
                return Outcome::TimedOut;
            sleep();
            waited = true;
        }
    }

private:
    std::atomic<uint32_t> state_{0};
    // Submissions this thread entered and has not left or stranded.
    static inline thread_local uint32_t own_ = 0;
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "read from signal handlers");
};

}  // namespace runtime::gpu
