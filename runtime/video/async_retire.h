// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, opt-in FM4_ASYNC_RETIRE=1): a completion worker that waits for the submitted parts'
// fences in submission order and does each completed part's GPU-side retire work there, so the command processor records the next
// parts meanwhile. A decorator on SubmissionBackend, as async_submit.h is (it may wrap that backend): GpuGuard and its bookkeeping
// stay on the command processor's thread; GpuGuard::Retire(slot) becomes "wait (bounded) until the worker retired that part".
//
// Adapted from ReCarbon (GPL-3.0-or-later, read-only) runtime/gpu/renderer.cpp: CompletionThread (:1027-1153: one thread
// takes the in-flight submissions in submission order, waits for each fence, runs that submission's deferred writes, then
// writewatch::SetCompleted, then frees the slot and notifies) and WaitForSubmission (:1157-1165: a thread that needs submission N
// waits on the same condition variable until the front of the in-flight list is past N). FM4 Recomp changes: the worker does only
// GPU-side work (the fence, the GPU read guard's completion, the part's resolve write-backs, the window hand-over of a present part);
// the command stream's deferred stores and interrupts stay on the command processor, which applies them after the part retired
// (deferred_store_parts.h; ReCarbon dispatches interrupts from its completion thread, command_processor.cpp:480-499, but FM4's
// RaiseInterrupts polls the GPU read guard, which may flush, and only the command processor may record or submit); the guard's
// completion is published BEFORE the write-backs (gpu_read_guard.h Completed, as RetireSlot does today), not after the writes as
// ReCarbon's SetCompleted is; every wait is bounded and a failure strands (GpuGuard) instead of being retried. New code.
//
// Rules (each tested in tests/runtime/test_async_retire.cpp with a stepped worker and a fake GPU):
//   A1 Parts retire in submission order (one FIFO, one worker).
//   A2 Per part, in this order: the fence wait (bounded by the fence timeout), completed(serial), writeBack(serial), present(swap) for
//      a present part, then the part's stage is Retired and the waiters are woken.
//   A3 WaitFence(slot) (the command processor, inside GpuGuard::Retire) returns only once that slot's stage is Retired: a drain returns
//      only after its part's write-backs landed. Bounded: VK_TIMEOUT when the worker made no progress for the fence timeout plus a
//      margin (any part retired restarts the bound, as one fence wait per part does today).
//   A4 A failed fence wait (timeout, device lost) is that slot's verdict and every later slot's, without waiting their fences (the
//      completion of later work is unknown too: GpuGuard strands each, GPU_STOP is written once). completed/writeBack/present run only
//      after a successful wait.
//   A5 A failed write-back or present is not a GPU failure: the fence verdict stays VK_SUCCESS (no GPU_STOP), the error is kept for
//      that slot (Error) and the command processor stops naming it; the worker runs no later job (fail closed), and still waits for
//      later fences so their resources can be released at the stop.
//   A6 FenceStatus never blocks: VK_NOT_READY until Retired; then the verdict (a failed verdict: the inner backend's real status).
//   A7 The worker never resets a fence and never touches a command buffer: Submit is the inner backend's (the command processor's
//      thread, or async_submit.h's worker), and a slot is submitted again only after GpuGuard::Retire saw it Retired.
#pragma once
#include "gpu_guard.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace runtime::video {

template <typename Resource>
class AsyncRetireBackend final : public SubmissionBackend<Resource>
{
public:
    enum class Stage : uint8_t { Idle, Queued, Fenced, Retired };
    struct Hooks
    {
        std::function<void(uint64_t serial)> completed;                      // GpuReadGuard::Completed (null: none)
        std::function<bool(uint64_t serial, std::string& error)> writeBack;  // the part's resolve write-backs (null: none)
        std::function<bool(uint64_t swap, std::string& error)> present;      // a present part's window hand-over (null: none)
        std::function<void()> onWorkerStart;                                 // the worker thread names itself (null: none)
    };
    struct Clock
    {
        std::function<uint64_t()> now;           // nanoseconds
        std::function<void()> idle;              // stepped mode: one step of the test's world in place of a condition wait
    };
    // `threaded` false: no thread (tests call Step()); `clock` then must be set. `marginNs`: added to the fence timeout of a
    // command-processor wait (A3).
    AsyncRetireBackend(SubmissionBackend<Resource>& inner, uint32_t slots, uint64_t fenceTimeoutNs, uint64_t marginNs, Hooks hooks, bool threaded = true,
                       Clock clock = {})
        : inner_(inner), hooks_(std::move(hooks)), clock_(std::move(clock)), timeoutNs_(fenceTimeoutNs), marginNs_(marginNs), threaded_(threaded),
          stage_(slots, Stage::Idle), verdict_(slots, 0), error_(slots), tag_(slots)
    {
        if (!clock_.now)
            clock_.now = [] { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
        if (threaded_)
            worker_ = std::thread([this] {
                if (hooks_.onWorkerStart)
                    hooks_.onWorkerStart();
                while (Step())
                {
                }
            });
    }
    AsyncRetireBackend(const AsyncRetireBackend&) = delete;
    AsyncRetireBackend& operator=(const AsyncRetireBackend&) = delete;
    ~AsyncRetireBackend() override { Stop(); }

    // The command processor, before GpuGuard::Submit(slot): the part that submission carries, and the swap it presents (0: none).
    void Tag(uint32_t slot, uint64_t serial, uint64_t presentSwap)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot < tag_.size())
            tag_[slot] = {slot, serial, presentSwap};
    }

    int Submit(uint32_t slot) override
    {
        if (slot >= stage_.size())
            return -1;  // VK_ERROR_OUT_OF_HOST_MEMORY stands in for "no such slot": the guard strands it
        const int r = inner_.Submit(slot);
        if (r != 0)
            return r;  // the guard strands it at once; nothing is queued for the worker
        std::lock_guard<std::mutex> lock(mutex_);
        stage_[slot] = Stage::Queued, verdict_[slot] = 0, error_[slot].clear();
        queue_.push_back(tag_[slot]);
        tag_[slot] = {slot, 0, 0};
        submits_++;
        wake_.notify_one();
        return 0;
    }
    // A3: the command processor (GpuGuard::Retire). `timeoutNs` is GpuGuard's fence timeout.
    int WaitFence(uint32_t slot, uint64_t timeoutNs) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (slot >= stage_.size())
            return -1;
        if (stage_[slot] == Stage::Idle)  // never handed to the worker (GpuGuard::Shutdown of an unused slot)
        {
            lock.unlock();
            return inner_.WaitFence(slot, timeoutNs);
        }
        const uint64_t start = clock_.now();
        uint64_t progressAt = start, seen = progress_;
        for (;;)
        {
            if (RetiredLocked(slot))
            {
                blockedNs_ += clock_.now() - start;
                return verdict_[slot];
            }
            if (progress_ != seen)
                seen = progress_, progressAt = clock_.now();
            if (clock_.now() - progressAt > timeoutNs + marginNs_)
            {
                cpTimeouts_++;
                blockedNs_ += clock_.now() - start;
                return 2;  // VK_TIMEOUT: the worker did not retire the part within the bound (completion unknown)
            }
            if (threaded_)
                done_.wait_for(lock, std::chrono::milliseconds(1));
            else
            {
                lock.unlock();
                clock_.idle();
                lock.lock();
            }
        }
    }
    // A6.
    int FenceStatus(uint32_t slot) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot >= stage_.size())
                return -1;
            if (stage_[slot] == Stage::Idle)
                return inner_.FenceStatus(slot);
            if (!RetiredLocked(slot))
                return 1;  // VK_NOT_READY
            if (verdict_[slot] == 0)
                return 0;
        }
        return inner_.FenceStatus(slot);
    }
    void Destroy(Resource& r) override { inner_.Destroy(r); }  // the command processor's thread, after a confirmed completion

    // A5: the job error of the slot's last retire (empty: none). The command processor, after GpuGuard::Retire confirmed it.
    std::string Error(uint32_t slot) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return slot < error_.size() ? error_[slot] : std::string();
    }
    Stage SlotStage(uint32_t slot) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return slot < stage_.size() ? stage_[slot] : Stage::Idle;
    }
    // Whether the worker is done with the slot's submission (a successful or a failed verdict): GpuGuard::Retire then never waits.
    bool Retired(uint32_t slot) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return slot < stage_.size() && RetiredLocked(slot);
    }

    // The worker's body: one queued part (A1, A2, A4, A5). False when stopping with nothing queued (threaded) or nothing queued (stepped).
    bool Step()
    {
        Tagged e;
        int failed;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (threaded_)
                wake_.wait(lock, [&] { return !queue_.empty() || stopping_; });
            if (queue_.empty())
                return false;
            e = queue_.front();
            queue_.pop_front();
            failed = failed_;
        }
        const uint64_t fenceStart = clock_.now();
        const int r = failed != 0 ? failed : inner_.WaitFence(e.slot, timeoutNs_);  // A4: nothing waited for after a failure
        const uint64_t fenceNs = clock_.now() - fenceStart;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (r == 0)
                stage_[e.slot] = Stage::Fenced;
        }
        std::string error;
        uint64_t jobNs = 0;
        if (r == 0)
        {
            if (hooks_.completed)
                hooks_.completed(e.serial);  // before any write-back (gpu_read_guard.h Completed)
            const uint64_t jobStart = clock_.now();
            std::string earlier;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                earlier = jobFailed_;
            }
            if (!earlier.empty())
                error = earlier;  // A5: no later job runs
            else if (hooks_.writeBack && !hooks_.writeBack(e.serial, error))
                error = "resolve write-back (completion worker): " + error;
            else if (e.presentSwap != 0 && hooks_.present && !hooks_.present(e.presentSwap, error))
                error = "present (completion worker): " + error;
            jobNs = clock_.now() - jobStart;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (r != 0 && failed_ == 0)
            failed_ = r;
        if (!error.empty() && jobFailed_.empty())
            jobFailed_ = error;
        verdict_[e.slot] = r;
        error_[e.slot] = error;
        stage_[e.slot] = Stage::Retired;
        progress_++;
        fenceNs_ += fenceNs, jobNs_ += jobNs, retired_++;
        retiredParts_.fetch_add(1, std::memory_order_release);
        done_.notify_all();
        return true;
    }

    // Drains the queue (every queued part is retired or refused after a failure) and joins the worker. Idempotent.
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            wake_.notify_all();
        }
        if (worker_.joinable())
            worker_.join();
    }

    // Counters, each since the last call (the per-swap timing line): the command processor's time blocked in WaitFence, the
    // worker's fence waits and its write-back/present jobs, parts retired.
    struct Counters { uint64_t blockedNs = 0, fenceNs = 0, jobNs = 0, retired = 0, cpTimeouts = 0; };
    Counters TakeCounters()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Counters c{blockedNs_, fenceNs_, jobNs_, retired_, cpTimeouts_};
        blockedNs_ = fenceNs_ = jobNs_ = retired_ = cpTimeouts_ = 0;
        return c;
    }
    // Parts the worker has retired since the start, lock-free (the command processor's reap reads it between every two packets).
    uint64_t RetiredParts() const { return retiredParts_.load(std::memory_order_acquire); }
    uint64_t Submits() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return submits_;
    }
    int Failed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }

private:
    struct Tagged { uint32_t slot = 0; uint64_t serial = 0, presentSwap = 0; };
    bool RetiredLocked(uint32_t slot) const { return stage_[slot] == Stage::Retired; }  // A3, A6: the one completion condition

    SubmissionBackend<Resource>& inner_;
    const Hooks hooks_;
    Clock clock_;
    const uint64_t timeoutNs_, marginNs_;
    const bool threaded_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::deque<Tagged> queue_;
    std::vector<Stage> stage_;
    std::vector<int> verdict_;
    std::vector<std::string> error_;
    std::vector<Tagged> tag_;
    int failed_ = 0;
    std::string jobFailed_;
    bool stopping_ = false;
    uint64_t progress_ = 0, submits_ = 0;
    uint64_t blockedNs_ = 0, fenceNs_ = 0, jobNs_ = 0, retired_ = 0, cpTimeouts_ = 0;
    std::atomic<uint64_t> retiredParts_{0};
    std::thread worker_;
};

}  // namespace runtime::video
