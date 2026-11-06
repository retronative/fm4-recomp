// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The single choke point between the renderer and the GPU queue (execution model §4: GPU_STOP is
// checked at Initialize, at every submission and at Present). It composes the CPU safety models of
// runtime/gpu/ so that the real binary cannot reach the queue without them:
//   - gpu_stop.h CheckGpuStop before any work (Begin, and again in Submit);
//   - submission_gate.h: every queue submission is Enter'ed first and Leave'd only on a confirmed
//     completion; an unknown completion strands it (MarkStranded) and nothing more is submitted;
//   - submission_lifetime.h, one per slot: a resource the slot's commands use is destroyed only after
//     that slot's submission is confirmed complete (never while in flight, never after a stranding);
//   - fence_wait.h: every fence wait is bounded (the timeout is the constructor's, from
//     FenceTimeoutFromEnv) and classified by WaitOutcome (wait result and fence status both
//     VK_SUCCESS, else stranded);
//   - loop_work.h LoopWorkAllowed before a draw records anything;
//   - a stranding writes GPU_STOP (gpu_stop_write.h) with the evidence.
// Pure bookkeeping over an abstract backend: the Vulkan backend (renderer.cpp) implements it, the
// CPU test (tests/test_gpu_guard.cpp) fakes it. No Vulkan type appears here.
//
// Threading (unit 4): one thread (the command processor's) calls everything, including Retire. The
// gate's per-thread count (SubmissionGate::own_ is thread_local) requires Enter and Leave/
// MarkStranded on the same thread; a completion thread (D-c, unit 5) needs a gate change first.
// New code; the order is Enter -> submit -> bounded wait -> destroy -> Leave.
#pragma once
#include "gpu/fence_wait.h"
#include "gpu/gpu_stop.h"
#include "gpu/loop_work.h"
#include "gpu/submission_gate.h"
#include "gpu/submission_lifetime.h"
#include "gpu_stop_write.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace runtime::video {

// The device side of one submission slot (a command buffer and its fence). Results are VkResult
// values (0 VK_SUCCESS, 1 VK_NOT_READY, 2 VK_TIMEOUT, negative errors).
template <typename Resource>
class SubmissionBackend
{
public:
    virtual ~SubmissionBackend() = default;
    // Resets the slot's fence (it is not in use: the guard calls this only while the slot is idle)
    // and submits the slot's recorded commands with it.
    virtual int Submit(uint32_t slot) = 0;
    // vkWaitForFences on the slot's fence with exactly `timeoutNs`.
    virtual int WaitFence(uint32_t slot, uint64_t timeoutNs) = 0;
    // vkGetFenceStatus on the slot's fence.
    virtual int FenceStatus(uint32_t slot) = 0;
    // Destroys a resource whose last use is confirmed complete.
    virtual void Destroy(Resource& r) = 0;
};

struct Verdict
{
    bool ok = false;
    bool stranded = false;  // a submission's completion is unknown (GPU_STOP was written, or its write failed)
    std::string reason;     // why not (the caller stops with it)
    explicit operator bool() const { return ok; }
};

template <typename Resource, uint32_t Slots>
class GpuGuard
{
    static_assert(Slots > 0);

public:
    // `stopPath`: the run's GPU_STOP (fm4recomp --gpu-stop, required). `fenceTimeoutNs`: the bounded
    // wait (fence_wait.h FenceTimeoutNs of FenceTimeoutFromEnv; D-g: 2 s).
    GpuGuard(SubmissionBackend<Resource>& backend, gpu::SubmissionGate& gate, std::string stopPath, uint64_t fenceTimeoutNs)
        : backend_(backend), gate_(gate), stopPath_(std::move(stopPath)), timeoutNs_(fenceTimeoutNs)
    {
    }
    GpuGuard(const GpuGuard&) = delete;
    GpuGuard& operator=(const GpuGuard&) = delete;

    uint64_t FenceTimeoutNs() const { return timeoutNs_; }
    typename gpu::SubmissionLifetime<Resource>::State SlotState(uint32_t slot) const { return lifetimes_[slot].Current(); }
    size_t Held(uint32_t slot) const { return lifetimes_[slot].Held(); }

    // Every GPU entry point (Initialize, Present, Draw, LoadShader) first: refuses while GPU_STOP
    // exists (or cannot be checked), once a stop has begun, or after a stranding.
    Verdict Begin(const char* entry) const
    {
        const gpu::GpuStopCheck flag = gpu::CheckGpuStop(stopPath_.c_str());
        if (flag.stopped)
            return {false, false, std::string(entry) + ": GPU work is stopped (" + stopPath_ + "): " + flag.reason};
        const gpu::SubmissionGate::View v = gate_.Load();
        if (v.stopping)
            return {false, false, std::string(entry) + ": the process is stopping"};
        if (v.stranded)
            return {false, false, std::string(entry) + ": an earlier submission's completion is unknown; no further GPU work"};
        return {true, false, {}};
    }

    // A resource the slot's commands being recorded use. Refused unless the slot is idle.
    bool Hold(uint32_t slot, Resource r) { return slot < Slots && lifetimes_[slot].Hold(std::move(r)); }

    // Hands the slot's recorded commands to the queue. Nothing reaches the backend unless GPU_STOP is
    // absent, the gate admits it and the slot is idle. A failed submit strands (completion unknown).
    Verdict Submit(uint32_t slot)
    {
        if (slot >= Slots)
            return {false, false, "submit: no slot " + std::to_string(slot)};
        if (Verdict v = Begin("submit"); !v)
            return v;
        gpu::SubmissionLifetime<Resource>& life = lifetimes_[slot];
        if (life.Current() != gpu::SubmissionLifetime<Resource>::State::Idle)
            return {false, false, "submit: slot " + std::to_string(slot) + " is not idle"};
        if (!gate_.Enter())
            return {false, false, "submit: the submission gate refused (stopping, stranded or full)"};
        life.Submitted();
        if (const int r = backend_.Submit(slot); r != 0)
        {
            life.Rejected();
            gate_.MarkStranded();
            return Strand("submit: vkQueueSubmit failed (VkResult " + std::to_string(r) + ") on slot " + std::to_string(slot));
        }
        return {true, false, {}};
    }

    // The slot's submission: a bounded wait, then its held resources are destroyed (confirmed) or
    // kept for good (stranded: GPU_STOP is written, nothing further is submitted).
    Verdict Retire(uint32_t slot)
    {
        if (slot >= Slots || lifetimes_[slot].Current() != gpu::SubmissionLifetime<Resource>::State::InFlight)
            return {false, false, "retire: slot " + std::to_string(slot) + " has no submission in flight"};
        const int waited = backend_.WaitFence(slot, timeoutNs_);
        const int status = backend_.FenceStatus(slot);
        const gpu::Completion c = gpu::WaitOutcome(waited, status);
        for (Resource& r : lifetimes_[slot].Completed(c))
            backend_.Destroy(r);
        if (c != gpu::Completion::Confirmed)
        {
            gate_.MarkStranded();
            return Strand("retire: slot " + std::to_string(slot) + " completion unknown (vkWaitForFences " + std::to_string(waited) +
                          " within " + std::to_string(timeoutNs_ / 1000000) + " ms, vkGetFenceStatus " + std::to_string(status) +
                          "); " + std::to_string(lifetimes_[slot].Held()) + " resource(s) kept");
        }
        gate_.Leave();  // after the resources it released are destroyed
        return {true, false, {}};
    }

    // Before the device is destroyed: each slot's last submission is waited for (bounded) where that
    // can establish anything, and its resources destroyed only if confirmed. Returns the number of
    // resources kept (stranded); the device must then not be destroyed either.
    size_t Shutdown()
    {
        size_t kept = 0;
        for (uint32_t s = 0; s < Slots; s++)
        {
            gpu::SubmissionLifetime<Resource>& life = lifetimes_[s];
            gpu::Completion last = gpu::Completion::Stranded;
            const bool inFlight = life.Current() == gpu::SubmissionLifetime<Resource>::State::InFlight;
            if (life.Awaitable())
                last = gpu::WaitOutcome(backend_.WaitFence(s, timeoutNs_), backend_.FenceStatus(s));
            for (Resource& r : life.Shutdown(last))
                backend_.Destroy(r);
            if (inFlight)
                last == gpu::Completion::Confirmed ? gate_.Leave() : gate_.MarkStranded();
            kept += life.Held();
        }
        return kept;
    }

    // The per-draw loop-work cap (loop_work.h, 2^26), before anything of the draw is recorded.
    // `loopConstants`: the SHADER_CONSTANT_LOOP values of every loop each stage reads.
    static Verdict DrawLoopWork(std::span<const uint32_t> vsLoops, uint64_t vsInvocations, std::span<const uint32_t> psLoops,
                                uint64_t psInvocations)
    {
        if (!gpu::LoopWorkAllowed(vsLoops, vsInvocations))
            return {false, false, "draw: vertex shader loop work over 2^26 (" + std::to_string(vsInvocations) + " invocations)"};
        if (!gpu::LoopWorkAllowed(psLoops, psInvocations))
            return {false, false, "draw: pixel shader loop work over 2^26 (" + std::to_string(psInvocations) + " invocations)"};
        return {true, false, {}};
    }

private:
    Verdict Strand(std::string reason)
    {
        std::string error;
        if (!WriteGpuStop(stopPath_.c_str(), reason, error))
            reason += "; GPU_STOP NOT WRITTEN: " + error;
        else
            reason += "; GPU_STOP written to " + stopPath_;
        return {false, true, std::move(reason)};
    }

    SubmissionBackend<Resource>& backend_;
    gpu::SubmissionGate& gate_;
    const std::string stopPath_;
    const uint64_t timeoutNs_;
    std::array<gpu::SubmissionLifetime<Resource>, Slots> lifetimes_{};
};

}  // namespace runtime::video
