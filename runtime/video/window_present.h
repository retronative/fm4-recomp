// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The window mode's presenter (renderer-internal; renderer.h declares its main-thread entry points). It owns the
// swapchain, its semaphores, and one submission slot (command buffer and fence) with its own GpuGuard on the renderer's
// process-wide gate: Enter and Leave both happen on the main thread, which SubmissionGate's per-thread count requires
// The one VkQueue is shared with the command processor's thread, so every
// vkQueueSubmit and vkQueuePresentKHR holds the renderer's queue lock (Vulkan: a queue is externally synchronized),
// taken with a bound.
#pragma once
#include "gpu/submission_gate.h"
#include "vulkan_device.h"
#include "window_policy.h"
#include <mutex>

namespace runtime::video {

struct PresenterSetup
{
    const VulkanDevice* dev = nullptr;
    VkImage display = VK_NULL_HANDLE;  // A2B10G10R10, TRANSFER_SRC_OPTIMAL whenever a frame is handed over
    VkExtent2D displayExtent{};
    gpu::SubmissionGate* gate = nullptr;
    const char* gpuStopPath = nullptr;
    uint64_t fenceTimeoutNs = 0;
    std::timed_mutex* queueLock = nullptr;
};

// Main thread, from Initialize: checks the device's blit support, creates the swapchain for the window's current size,
// logs the "[window] swapchain" line scripts/gpu_run.sh requires. Stops (9) on any failure.
void InitializePresenter(const PresenterSetup& setup);
// The hand-over both threads use (window_policy.h FrameHandOver).
window::FrameHandOver& HandOver();
// The bound of every queue-lock wait and of the command processor's BeginWrite: the fence limit plus 1 s, which covers a
// CAMetalLayer nextDrawable wait inside the presenter's vkQueueSubmit (Apple: nextDrawable times out after 1 s when
// allowsNextDrawableTimeout is YES, the default) plus the
// acquire bound.
uint64_t PresenterWaitBoundNs(uint64_t fenceTimeoutNs);
// Takes `lock` within `boundNs` or stops (9), naming `who`.
std::unique_lock<std::timed_mutex> LockQueue(std::timed_mutex& lock, uint64_t boundNs, const char* who);

}  // namespace runtime::video
