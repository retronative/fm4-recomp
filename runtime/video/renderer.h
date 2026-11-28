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
// FM4 Recomp (FM4_GRAPHICS_LIVE): edge smoothing (off, soft, on, strong) from the next presented swap; false when this run cannot
// switch it (the change then applies at the next start). Any thread.
bool SetPostAaLive(const char* spelling);
// FM4 Recomp (M9, live resolution; FM4_GRAPHICS_LIVE): asks for another internal resolution (FM4_INTERNAL_RES's spellings: 720p, fhd,
// qhd, 4k and their synonyms) while the game runs. Any thread; it only records the request. False when this run cannot switch (no
// FM4_GRAPHICS_LIVE=1, not a present or window run, FM4_INTERNAL_OUTPUT=720p, FM4_DIAG_POST_AA_PAIR or FM4_GPU_PRESENT_CHECK set, an
// unknown spelling): the change then applies at the next start. True: the command processor's thread makes the change at one of
// the next swaps, when nothing is in flight, or refuses it with one "[renderer] resolution: ... is refused" line (the device cannot
// make something at that size, or the renderer was not idle for 120 swaps) and the resolution stays. So a caller that shows the
// setting reads InternalResNow() afterwards: it changes only when the switch was made; InternalResPending() says a request is waiting.
// `purpose` (optional, a string literal such as "GRAPHICS rollback"): when this request is served, one more line names it and says
// whether the change was made or refused, and what is in effect then.
bool RequestInternalRes(const char* spelling, const char* purpose = nullptr);
// The internal resolution in effect, as "720p", "fhd", "qhd" or "4k". Any thread.
const char* InternalResNow();
bool InternalResPending();
// What the resolution will be once a waiting request is served (InternalResNow without one). Any thread.
const char* InternalResTarget();
// The edge smoothing at the next presented swap, as graphics.conf spells it ("off", "soft", "on", "strong"). Any thread.
const char* PostAaNow();
// The HDR preference (SetHdrPreference): true for Auto. Any thread.
bool HdrPreferenceNow();
// FM4 Recomp (M9): the GRAPHICS submenu's Y: Preview (kernel/menu_preview.h): from the next swap, the pause blur copies the picture
// instead and the menus are drawn only in the help bar's corner; off: every draw as the game asks. Any thread.
void SetMenuPreview(bool on);
// FM4 Recomp (M9, FM4_HDR; hdr_output.h): the display's current headroom (1: none), from the window's thread at each present; the
// next swap's pass expands the picture's highlights into it. Changes are logged (the first 20).
void SetHdrHeadroom(float headroom);
// FM4 Recomp (the in-game GRAPHICS menu and the Settings panel): the HDR preference, Off or Auto, from the next presented swap. Any
// thread. Auto shows HDR only where this run can present it and the display the window is on supports it; otherwise the picture is
// standard range. The first time HDR is in effect the pass's objects are created at that swap and the window's swapchain is
// rebuilt once; after that a change only moves the expansion (not in effect: the standard picture through the same swapchain).
void SetHdrPreference(bool automatic);
// The window's thread, about once a second: whether the display the window is on can show HDR.
void SetHdrDisplay(bool supported);
// What is shown now, and why it is standard range ("HDR is off", "this display does not support HDR", "HDR is not available on
// this system", "restart required"; empty when active). Any thread.
struct HdrOutput
{
    bool active;
    const char* reason;
};
HdrOutput HdrOutputNow();

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
// FM4 Recomp (FM4_RESOLVE_DEFER): before a command-processor packet the guest CPU can synchronise on, the pending resolves'
// write-backs land in guest memory (a flush when any is pending; nothing otherwise).
void SyncGuestWrites(uint32_t opcode);
// FM4 Recomp (FM4_DEFER_FENCES=1, opt-in; needs FM4_RESOLVE_DEFER=1): the command stream's memory stores land at GPU completion
// (command_processor.h deferStore / ringIdle / ApplyDeferredStores): DeferStore marks the 4 bytes pending while the slot is
// open (true: queued); RingIdle submits and retires the open slot when the ring is drained with stores queued.
bool DeferFences();
bool DeferStore(uint32_t physical);
// FM4 Recomp (M9, opt-in FM4_OCCLUSION_QUERIES=1; command_processor.h RendererHooks::zpassDone / zpdCount): occlusion queries
// counted on the GPU. ZpassDone answers 0 when off.
uint32_t ZpassDone(uint32_t physical);
uint32_t ZpdCount(uint32_t token);
void RingIdle();
bool PendingAt(uint32_t physical, uint32_t bytes);
// FM4 Recomp (M7, FM4_GUEST_MEMORY_GPU=1; gpu_read_guard.h G5; command_processor.h honourGpuReadGuard / beforeCpWrite):
// HonourGpuReadGuard submits and retires up to the part a waiting store needs; BeforeCpWrite does the same inline before the
// command processor itself stores over a range that unretired GPU work reads. Both nothing while the import is off.
void HonourGpuReadGuard();
void BeforeCpWrite(uint32_t physical, uint32_t bytes);
// FM4 Recomp (opt-in FM4_ASYNC_RETIRE=1, async_retire.h; command_processor.h SetReapHook): whether the completion worker runs (after
// Initialize), and the reap the command processor calls between packets: the parts the worker retired are retired here without
// waiting, then their deferred stores land in queue order (never an interrupt; never a store a later part's read guards).
bool AsyncRetireOn();
void ReapRetired();
// FM4 Recomp (opt-in FM4_NO_DRAIN=1, not guest-exact; renderer.cpp NoDrain): stream stores and interrupts land when the command processor
// reaches them; CpuVisible (command_processor.h RendererHooks::cpuVisible) first writes back a resolve that starts a new capture run.
bool NoDrain();
void CpuVisible();
bool WorkOpen();  // FM4_DEFER_FENCES: the slot holds recorded work not yet submitted  // a GPU write or deferred store over the range waits for the open slot  // opcode: the PM4 type-3 packet (counted per packet in the swap timing line)

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
