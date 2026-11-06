// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The M4 renderer skeleton (execution model unit 4: device, clear, present). It runs on the command
// processor's thread and never changes the CP's control flow (execution model §3 "headless stays
// the oracle"): Initialize returns false headless,
// and every other call is then a no-op.
//
// Modes (fm4recomp --renderer, required; no silent default):
//   headless    no Vulkan at all (M3 behaviour; the CPU-only oracle).
//   self-check  the first approved GPU run: device + validation, one clear and an upload into an
//               offscreen display image, read back and compared on the CPU, captured through the dump
//               ring; then each safety path exercised in-process; stops with
//               StopCode::GpuSelfCheck (12) before the title runs any code.
//   present     the guest front buffer is detiled and presented (offscreen, captured) at every
//               XE_SWAP. Draws are NOT rendered yet: each swap logs how many were skipped, and the
//               mode is diagnostic only, never a default (no draw skipping by default).
// All GPU work goes through GpuGuard (gpu_guard.h): GPU_STOP, submission gate, per-slot lifetime,
// bounded fence waits, loop-work cap, GPU_STOP written on a stranding.
#pragma once
#include "gpu/submission_gate.h"
#include "stops.h"
#include <cstdint>

namespace runtime::video {

enum class Mode
{
    Headless,
    SelfCheck,
    Present,
};

struct Options
{
    Mode mode = Mode::Headless;
    const char* gpuStopPath = nullptr;  // --gpu-stop (required)
    const char* captureDir = nullptr;   // --gpu-capture <dir> (required unless headless): the run's evidence dir
    bool validation = true;             // --vk-validation on|off (required unless headless; gpu_run.sh passes on)
    const char* fenceTimeoutEnv = nullptr;  // getenv("FM4_GPU_FENCE_TIMEOUT_MS") (fence_wait.h FenceTimeoutFromEnv)
};

// The process-wide submission gate (stops.cpp drains it before _exit).
gpu::SubmissionGate& Submissions();

// From main, before the title starts. False headless. Stops on any failure (fail closed).
bool Initialize(const Options& options);
bool Enabled();

// The self-check (Mode::SelfCheck): never returns.
[[noreturn]] void RunSelfCheck();

// Command processor hook points (kernel/command_processor.cpp). Each checks GPU_STOP first.
// XE_SWAP, after the type-0 write of the fetch constant: `fetch` = registers 4800-4805 (host order).
void Present(const uint8_t* base, const uint32_t fetch[6], uint32_t physical, uint32_t width, uint32_t height, uint64_t swap);
// DRAW_INDX / DRAW_INDX_2: unit 8 records the draw here; until then it is counted (Mode::Present).
void Draw(const uint8_t* base, uint32_t opcode);
// IM_LOAD / IM_LOAD_IMMEDIATE: unit 8 translates (runtime/shader) and caches here; nothing yet.
void LoadShader(uint32_t type, const uint32_t* words, uint32_t count);

// For stops.cpp (normal thread context, not a signal handler): begins the stop, waits (bounded) for
// submissions in flight, reports. Signal-safe parts only from a signal handler: Submissions()
// .BeginStop() and .Load().
void QuiesceForStop();
void ReportForSignalStop();

}  // namespace runtime::video
