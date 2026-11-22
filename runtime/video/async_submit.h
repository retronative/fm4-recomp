// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (opt-in FM4_ASYNC_SUBMIT=1): the queue submission on its own thread. MoltenVK encodes a Vulkan command buffer into
// Metal inside vkQueueSubmit, on the submitting thread: 19.5 % of the command processor's time in a race
// profile (MVKQueue::submit -> MVKCommandEncoder::encode). Here the command processor hands a slot to a FIFO worker that resets the
// slot's fence and submits it, under the queue lock the window presenter shares; the command processor records the next part
// meanwhile. The technique is ShiftGlue's asynchronous submission (the submission thread its
// command processor feeds), implemented independently around GpuGuard's contract:
//   - GpuGuard's bookkeeping (slot lifetimes, the submission gate, GPU_STOP) stays on the command processor's thread: Submit here
//     only queues; the worker calls the real backend's Submit in queue order, so submissions reach the device in the order the
//     guard saw them;
//   - a fence is reset and submitted by the worker, and waited for or queried only after the worker has processed the slot
//     (WaitFence / FenceStatus first await that, bounded by the fence timeout): no fence is reset while another thread waits on it;
//   - a failed real submit (vkResetFences or vkQueueSubmit) is remembered: the slot's WaitFence and FenceStatus return that
//     VkResult, which GpuGuard::Retire turns into a stranding (completion unknown, GPU_STOP, nothing further submitted), and the
//     worker submits nothing after it; fail closed, one step later than the synchronous path;
//   - Stop() drains the queue and joins the worker; the renderer calls it after GpuGuard::Shutdown and before the device goes.
// Header only, std only: tests/runtime/test_gpu_guard.cpp drives it with a mock backend (CPU only).
#pragma once
#include "thread_qos.h"
#include "gpu_guard.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace runtime::video {

template <typename Resource>
class AsyncSubmitBackend final : public SubmissionBackend<Resource>
{
public:
    // `lockQueue`: taken around each real Submit (null: none); `slots`: the slot count (indices below it).
    AsyncSubmitBackend(SubmissionBackend<Resource>& inner, uint32_t slots, std::function<std::unique_lock<std::timed_mutex>()> lockQueue)
        : inner_(inner), lockQueue_(std::move(lockQueue)), submitted_(slots, 0), result_(slots, 0)
    {
        worker_ = std::thread([this] { Run(); });
    }
    AsyncSubmitBackend(const AsyncSubmitBackend&) = delete;
    AsyncSubmitBackend& operator=(const AsyncSubmitBackend&) = delete;
    ~AsyncSubmitBackend() override { Stop(); }

    int Submit(uint32_t slot) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot >= submitted_.size())
            return -1;  // VK_ERROR_OUT_OF_HOST_MEMORY stands in for "no such slot": the guard strands it
        submitted_[slot] = 0, result_[slot] = 0;
        queue_.push_back(slot);
        submits_++;
        if (queue_.size() > maxDepth_)
            maxDepth_ = queue_.size();
        wake_.notify_one();
        return 0;
    }
    int WaitFence(uint32_t slot, uint64_t timeoutNs) override
    {
        if (!AwaitSubmitted(slot, timeoutNs))
            return 2;  // VK_TIMEOUT: the worker did not reach the slot within the fence timeout (completion unknown)
        if (const int r = Result(slot); r != 0)
            return r;
        return inner_.WaitFence(slot, timeoutNs);
    }
    int FenceStatus(uint32_t slot) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot >= submitted_.size() || !submitted_[slot])
                return 1;  // VK_NOT_READY
            if (result_[slot] != 0)
                return result_[slot];
        }
        return inner_.FenceStatus(slot);
    }
    void Destroy(Resource& r) override { inner_.Destroy(r); }  // the command processor's thread, after a confirmed completion

    // Drains the queue (every queued slot is submitted or refused after a failure) and joins the worker. Idempotent.
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
    // Diagnostic hook (null by default): called on the worker after each real submit with the queue-lock wait and the
    // submit's own duration, in ns. Set before the first Submit.
    void SetTimingHook(std::function<void(uint32_t, uint64_t, uint64_t)> hook) { timingHook_ = std::move(hook); }
    uint64_t Submits() const { return submits_; }
    size_t MaxQueueDepth() const { return maxDepth_; }
    int Failed() const { std::lock_guard<std::mutex> lock(mutex_); return failed_; }

private:
    bool AwaitSubmitted(uint32_t slot, uint64_t timeoutNs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (slot >= submitted_.size())
            return false;
        return done_.wait_for(lock, std::chrono::nanoseconds(timeoutNs), [&] { return submitted_[slot] != 0; });
    }
    int Result(uint32_t slot)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return result_[slot];
    }
    void Run()
    {
        runtime::ApplyThreadQos();  // FM4_THREAD_QOS (experiment, off by default): the thread that submits to the queue
        for (;;)
        {
            uint32_t slot;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return !queue_.empty() || stopping_; });
                if (queue_.empty())
                    return;  // stopping and drained
                slot = queue_.front();
                queue_.pop_front();
            }
            int r;
            {
                const auto t0 = std::chrono::steady_clock::now();
                std::unique_lock<std::timed_mutex> queueLock = lockQueue_ ? lockQueue_() : std::unique_lock<std::timed_mutex>();
                const auto t1 = std::chrono::steady_clock::now();
                std::unique_lock<std::mutex> lock(mutex_);
                const int failed = failed_;
                lock.unlock();
                r = failed != 0 ? failed : inner_.Submit(slot);  // nothing reaches the queue after a failure
                if (timingHook_)
                {
                    const auto ns = [](auto d) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()); };
                    timingHook_(slot, ns(t1 - t0), ns(std::chrono::steady_clock::now() - t1));
                }
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (r != 0 && failed_ == 0)
                failed_ = r;
            result_[slot] = r;
            submitted_[slot] = 1;
            done_.notify_all();
        }
    }

    SubmissionBackend<Resource>& inner_;
    std::function<std::unique_lock<std::timed_mutex>()> lockQueue_;
    std::function<void(uint32_t, uint64_t, uint64_t)> timingHook_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::deque<uint32_t> queue_;
    std::vector<uint8_t> submitted_;
    std::vector<int> result_;
    int failed_ = 0;
    bool stopping_ = false;
    uint64_t submits_ = 0;
    size_t maxDepth_ = 0;
    std::thread worker_;
};

}  // namespace runtime::video
