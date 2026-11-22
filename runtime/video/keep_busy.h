// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9 experiment, opt-in FM4_GPU_KEEP_BUSY=1, off by default; owner request 2026-11-21): keeps the GPU lightly busy between
// the game's frames, to see whether Apple's GPU governor then raises the clock (OBSERVED with powermetrics: the GPU stays at 338-636 of
// 1578 MHz while the game drops frames, never saturated; research/m9/perf/RESULTS.md) and whether the game's frames get shorter.
//
// What it does: a second VkDevice on the same GPU (its own queue, command buffer, fence, pipeline and 1 MB buffer; nothing shared
// with the renderer's device, slots, guard or submission gate) and a thread that submits one fixed compute dispatch after another,
// waiting for each. The work is bounded: FM4_GPU_KEEP_BUSY_GROUPS (1..256, default 64) squared workgroups of 8x8 invocations, each a
// 32-step loop and one store.
// GPU safety: valid work only; the command buffer is recorded once and submitted again only after its previous submission
// completed; each fence wait is bounded (the renderer guard's bound) and a miss writes GPU_STOP, stops submitting and leaves every object alone
// (completion unknown); GPU_STOP present stops it; nothing is destroyed until the thread has ended with nothing in flight.
// FM4_GPU_KEEP_BUSY_CYCLE=<seconds> (1..600): alternate OFF/ON windows of that length from the first presented swap, OFF first; each
// switch is one log line with the swap number and the wall clock (Unix seconds), for an A/B inside one run.
#pragma once
#include "vulkan_device.h"
#include <cstdint>

namespace runtime::video {
class SpirvCompiler;
}

namespace runtime::video::keepbusy {

// After the renderer's device and compiler exist. Nothing unless FM4_GPU_KEEP_BUSY=1; a refusal is named and the run goes on without it.
// `rendererIdle`: whether the renderer has no submission in flight (any thread); used with FM4_GPU_KEEP_BUSY_GATED=1, where the unit
// submits only then (the game's own work is not shared with the filler). FM4_GPU_KEEP_BUSY_QOS=interactive: the thread's class.
void Initialize(const VulkanDevice& renderer, SpirvCompiler& compiler, const char* gpuStopPath, uint64_t fenceTimeoutNs, bool (*rendererIdle)());
// The command processor, once per presented swap: the OFF/ON cycle's switches.
void OnSwap(uint64_t swap);
// A stop is draining: no further submission (the one in flight completes within its fence bound).
void Quiesce();
// Before the instance is destroyed: ends the thread and destroys the unit's objects. False when a submission's completion is unknown:
// nothing was destroyed and the caller must not destroy the instance either.
bool Shutdown();

}  // namespace runtime::video::keepbusy
