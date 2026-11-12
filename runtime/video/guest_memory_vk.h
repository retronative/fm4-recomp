// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, guest memory for the GPU; opt-in FM4_GUEST_MEMORY_GPU=1, with FM4_WRITE_WATCH=1): guest physical memory as one
// GPU storage buffer, imported without a copy (VK_EXT_external_memory_host) from a dedicated host view of the physical memory
// (guest_memory.h MapPhysicalHostView: never protected, so the heaps' and the write watch's protections on the guest views do not
// reach the GPU mapping). Units read guest bytes from it at GPU execution time, guarded by the GPU read guard (gpu_read_guard.h):
// guest stores and host writes to a range wait until every part that reads it has completed. ReCarbon imports guest memory the
// same way (runtime/gpu/renderer.cpp:6531-6582 ImportSharedMemory, GPL-3.0; adapted).
//
// Lifetime: created once at device setup (InitGuestMemoryGpu, after the device), destroyed only in the renderer's Shutdown after
// every submission is confirmed complete; the host view is never unmapped (the process ends after).
#pragma once
#include "gpu_read_guard.h"
#include <vulkan/vulkan.h>

namespace runtime::video {

struct VulkanDevice;

struct GuestMemoryGpu
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;  // physical address p is at byte p
};
// Null when off or the import failed: callers keep their copy paths.
const GuestMemoryGpu* GuestMemoryGpuView();
// The guard every direct reader uses (null exactly when GuestMemoryGpuView is). Command-processor thread: NoteReadThen before
// protecting and recording a read, with the part serial that records it (render_hooks.h currentPartSerial).
GpuReadGuard* GuestMemoryGpuGuard();
bool GuestMemoryGpuAsked();  // FM4_GUEST_MEMORY_GPU=1 (vulkan_device.cpp enables the extension only then)

// Device setup (renderer.cpp CreateResources): one line names the result; on any refusal the view stays null. `fenceTimeoutNs`
// (GpuGuard::FenceTimeoutNs) sets the guard's wait bound (twice it plus 1 s: the command processor's own fence timeout stops first
// with its evidence). Installs the guard's fault and heap hooks when the import succeeded.
void InitGuestMemoryGpu(const VulkanDevice& dev, uint64_t fenceTimeoutNs);
// Renderer Shutdown, after every submission is confirmed complete: the hooks off, the buffer and its memory freed.
void ShutdownGuestMemoryGpu(const VulkanDevice& dev);

}  // namespace runtime::video
