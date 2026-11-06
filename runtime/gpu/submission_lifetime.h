// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp (forzarecomp-recarbon) runtime/video/submission_lifetime.h
// (GPL-3.0-or-later, same owner), reviewed and re-tested here (tests/run_gpu_safety_tests.sh).
// FM4 Recomp changes: namespace runtime::gpu; Completion and WaitOutcome moved to fence_wait.h
// (Unknown is named Stranded there).
//
// Completion-based retirement of a batch's GPU resources: a resource the open batch's commands
// use is held until that batch's submission is confirmed complete, and is handed back for
// destruction only then. A failed or unknown completion (a failed or refused submit, a fence wait
// that failed or timed out) strands everything held: nothing is released, no further submission
// is accepted, and at shutdown the holder releases only after a confirmed completion (otherwise it
// keeps the resources and reports them).
//
// Idle -> InFlight (Submitted) -> Idle (Completed, confirmed) or Stranded (Completed, stranded;
// Rejected). Every other transition is refused (false) or changes nothing. Pure bookkeeping, no
// GPU calls.
#pragma once
#include "fence_wait.h"
#include <cstddef>
#include <utility>
#include <vector>

namespace runtime::gpu {

template <typename Resource>
class SubmissionLifetime
{
public:
    enum class State
    {
        Idle,      // nothing submitted is unfinished (resources may be held by commands not yet submitted)
        InFlight,  // one submission is out, its completion not yet established
        Stranded,  // a completion was unknown: held resources may be in use, forever as far as we know
    };

    State Current() const { return state_; }
    size_t Held() const { return held_.size(); }
    // The upload space (and anything else the last submission read) may be overwritten or replaced.
    bool Reusable() const { return state_ == State::Idle; }

    // A resource the commands being recorded use. Refused (false) while stranded or in flight:
    // recording happens only between confirmed submissions.
    bool Hold(Resource r)
    {
        if (state_ != State::Idle)
            return false;
        held_.push_back(std::move(r));
        return true;
    }
    // The recorded commands are being handed to the queue (Rejected if that fails). Refused unless
    // idle (one in flight at most): checked before the submission's fence is reset.
    bool Submitted()
    {
        if (state_ != State::Idle)
            return false;
        state_ = State::InFlight;
        fenceOut_ = true;
        return true;
    }
    // The submission's wait result. Confirmed: every held resource is returned, oldest first, for
    // destruction now. Stranded: nothing is returned and the holder is stranded. Without a
    // submission in flight nothing changes and nothing is returned.
    std::vector<Resource> Completed(Completion c)
    {
        if (state_ != State::InFlight)
            return {};
        if (c != Completion::Confirmed)
        {
            state_ = State::Stranded;
            return {};
        }
        state_ = State::Idle;
        return std::exchange(held_, {});
    }
    // The submit call itself failed (or the submission was refused before it): nothing was waited
    // for and no fence of it can be waited on. Stranded as after an unknown completion, but a final
    // wait is pointless.
    void Rejected()
    {
        if (state_ != State::InFlight)
            return;
        state_ = State::Stranded;
        fenceOut_ = false;
    }
    // Whether a final wait on the last submission's fence can establish anything: a submission
    // reached the queue and its completion is not yet confirmed.
    bool Awaitable() const { return state_ != State::Idle && fenceOut_; }
    // Shutdown. With nothing unfinished (idle: anything held was never submitted) or a confirmed
    // final wait (`last`, made by the caller only when Current() is not Idle), everything held is
    // returned; otherwise nothing is, and Held() says how many resources are kept.
    std::vector<Resource> Shutdown(Completion last)
    {
        if (state_ != State::Idle && last != Completion::Confirmed)
            return {};
        state_ = State::Idle;
        return std::exchange(held_, {});
    }

private:
    State state_ = State::Idle;
    bool fenceOut_ = false;  // the last submission reached the queue (its fence can be waited on)
    std::vector<Resource> held_;
};

}  // namespace runtime::gpu
