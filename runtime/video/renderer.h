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
//               XE_SWAP, and every draw since the previous swap is recorded into the same submission
//               (unit 8, Draw below): the shader cache, the pipeline cache, the per-slot upload ring and
//               descriptor pool, the EDRAM and texture hooks (render_hooks.h). A draw the host cannot
//               render faithfully stops the run naming the draw; a NoWrite draw follows the one policy
//               switch draw_record.h kNoWriteDraws (decision pending) and is counted in the swap line.
//   window      present, plus a visible window (unit 4b; window_sdl.h, window_policy.h, window_present.cpp): at each swap,
//               after the frame's submission retired and the capture was written, the command processor hands the
//               display image to the main thread, which acquires a swapchain image, blits the display image into it
//               (letterboxed) in its own submission through its own GpuGuard on the shared gate, and presents (FIFO).
//               Closing the window stops with StopCode::WindowClosed (14). Captures are unchanged.
// All GPU work goes through GpuGuard (gpu_guard.h): GPU_STOP, submission gate, per-slot lifetime,
// bounded fence waits, loop-work cap, GPU_STOP written on a stranding.
#pragma once
#include "gpu/submission_gate.h"
#include "render_hooks.h"
#include "stops.h"
#include "vulkan_device.h"
#include "window_policy.h"
#include <cstdint>

namespace runtime::video {

enum class Mode
{
    Headless,
    SelfCheck,
    Present,
    Window,  // Present + the window (main thread)
};

struct Options
{
    Mode mode = Mode::Headless;
    const char* gpuStopPath = nullptr;  // --gpu-stop (required)
    const char* captureDir = nullptr;   // --gpu-capture <dir> (required unless headless): the run's evidence dir
    bool validation = true;             // --vk-validation on|off (required unless headless; gpu_run.sh passes on)
    const char* fenceTimeoutEnv = nullptr;  // getenv("FM4_GPU_FENCE_TIMEOUT_MS") (fence_wait.h FenceTimeoutFromEnv)
    // --shader-classes none|<branch,predicate,loop> (required unless headless; shader_class.h): the classes of
    // shaders this run may create pipelines for. A never-seen class runs first in its own approved run.
    const char* shaderClasses = nullptr;
    const char* shaderDumpDir = nullptr;  // getenv("FM4_SHADER_DUMP"): the translated GLSL beside the CP's .bin dumps (shader_cache.h)
    // main.cpp's writable guest memory base (runtime::ReserveGuestMemory), required in present mode: the EDRAM unit
    // writes resolved rectangles into guest memory through it (edram_vk.h), never through the CP's const base.
    uint8_t* guestBase = nullptr;
    // Window mode only (required there): the window's surface factory and instance extensions (window_sdl.h OpenWindow).
    const SurfaceRequest* surface = nullptr;
};

// The EDRAM unit's and the texture unit's hooks (render_hooks.h), registered before the title runs. Without
// them a draw that needs a render target, a resolve or a texture stops (fail closed).
void SetEdramHooks(const EdramHooks* hooks);
void SetTextureHooks(const TextureHooks* hooks);

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
// DRAW_INDX / DRAW_INDX_2 (unit 8): decodes the draw state (draw_state.h), checks the loop-work cap and the
// shader-class allow-list, builds or finds the pipeline, uploads the draw's blocks into the slot's ring and
// records the draw into the open submission; a resolve goes to the EDRAM hooks. Every refusal stops, naming
// the draw number.
void Draw(const uint8_t* base, uint32_t opcode, uint32_t vizQueryToken);
// IM_LOAD / IM_LOAD_IMMEDIATE: ShaderCache::Load (translation at a hash miss, class tag); a refused shader
// stops naming its hash.
void LoadShader(uint32_t type, const uint32_t* words, uint32_t count);

// Window mode, main thread only (window_sdl.cpp RunWindowLoop; defined in window_present.cpp). Waits up to `waitNs` for
// the frame the command processor handed over and presents it: planner decision, bounded acquire, the blit submission
// through the presenter's GpuGuard, vkQueuePresentKHR, the bounded retire. True when a frame was taken (presented or
// skipped, both counted).
bool PresentHandedOverFrame(uint64_t waitNs);
// Window mode, main thread only: a window event for the swapchain planner (resize, minimize, restore).
void WindowEvent(window::EventAction action, VkExtent2D pixels);

// For stops.cpp (normal thread context, not a signal handler): begins the stop, waits (bounded) for
// submissions in flight, reports. Signal-safe parts only from a signal handler: Submissions()
// .BeginStop() and .Load().
void QuiesceForStop();
void ReportForSignalStop();

}  // namespace runtime::video
