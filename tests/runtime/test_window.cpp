// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// T0 tests of the window mode's pure parts (runtime/video/window_policy.h, window_sdl.h's inline event mapping), CPU
// only: Vulkan and SDL headers are included for their types and constants, but no Vulkan or SDL function is called or
// linked, no window is opened, no device created (GPU safety). Covered: the swapchain parameters from
// capability structs, the letterbox rectangle, the acquire/present result classification, the (re)creation planner,
// the frame hand-over between the command processor's thread and the main thread (including a two-thread run that
// checks the display image is never written while the presenter reads it), SDL's event types, and the stop-on-close
// path (a close event reaches the stop and nothing else does; a stop from the main thread drains the other thread's
// submission). Expected values are hand-computed in the comment next to them; "CONTROL" marks a negative control.
//
//   tests/run_window_tests.sh
#include "gpu/submission_gate.h"
#include "window_policy.h"
#include "window_sdl.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace runtime::video::window;

namespace {
int failures = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}

uint64_t NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

VkSurfaceCapabilitiesKHR Caps(uint32_t minImages, uint32_t maxImages, VkExtent2D current)
{
    VkSurfaceCapabilitiesKHR c{};
    c.minImageCount = minImages;
    c.maxImageCount = maxImages;
    c.currentExtent = current;
    c.minImageExtent = {1, 1};
    c.maxImageExtent = {4096, 4096};
    c.maxImageArrayLayers = 1;
    c.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c.supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return c;
}

bool SameExtent(std::optional<VkExtent2D> e, uint32_t w, uint32_t h) { return e && e->width == w && e->height == h; }

void TestFormats()
{
    auto all = [](VkFormat) { return true; };
    const std::vector<VkSurfaceFormatKHR> mvk = {{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                 {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                 {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    FormatChoice c = ChooseSurfaceFormat(mvk, all);
    Expect(c.ok && c.format.format == VK_FORMAT_B8G8R8A8_UNORM, "format: B8G8R8A8_UNORM first, whatever the surface's order");
    // CONTROL: an _SRGB format would encode the already gamma-ramped values a second time.
    const std::vector<VkSurfaceFormatKHR> srgbOnly = {{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    Expect(!ChooseSurfaceFormat(srgbOnly, all).ok, "format: an _SRGB-only surface is refused (CONTROL)");
    const std::vector<VkSurfaceFormatKHR> p3 = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT},
                                                {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    c = ChooseSurfaceFormat(p3, all);
    Expect(c.ok && c.format.format == VK_FORMAT_R8G8B8A8_UNORM, "format: another colour space is skipped (R8G8B8A8 in sRGB chosen)");
    c = ChooseSurfaceFormat(mvk, [](VkFormat f) { return f != VK_FORMAT_B8G8R8A8_UNORM; });
    Expect(c.ok && c.format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32, "format: one the device cannot blit to is skipped (10-bit next)");
    c = ChooseSurfaceFormat(mvk, [](VkFormat) { return false; });
    Expect(!c.ok && c.reason.find("imageFormat-01273") != std::string::npos, "format: nothing blittable is refused, naming the rule");
    Expect(!ChooseSurfaceFormat(std::span<const VkSurfaceFormatKHR>{}, all).ok, "format: an empty list is refused");
}

void TestExtentAndCounts()
{
    // A defined currentExtent is used as is, even if the window reports another size.
    Expect(SameExtent(ChooseExtent(Caps(2, 3, {2560, 1440}), {1280, 720}), 2560, 1440), "extent: a defined currentExtent wins");
    // 0xFFFFFFFF: the window's pixels, clamped to [1, 4096].
    Expect(SameExtent(ChooseExtent(Caps(2, 3, {UINT32_MAX, UINT32_MAX}), {2560, 1440}), 2560, 1440), "extent: undefined -> window pixels");
    Expect(SameExtent(ChooseExtent(Caps(2, 3, {UINT32_MAX, UINT32_MAX}), {9000, 300}), 4096, 300), "extent: clamped to maxImageExtent");
    Expect(!ChooseExtent(Caps(2, 3, {0, 0}), {1280, 720}), "extent: a zero currentExtent (minimized) gives none (imageExtent-01689)");
    Expect(SameExtent(ChooseExtent(Caps(2, 3, {UINT32_MAX, UINT32_MAX}), {0, 720}), 1, 720),
           "extent: an undefined extent with a zero window width clamps to minImageExtent 1 (never zero; the planner skips it first)");
    // minImageCount + 1, at least 2, at most maxImageCount (0 = no limit): (2,3) -> 3, (3,3) -> 3, (1,0) -> 2, (2,0) -> 3, (4,8) -> 5.
    Expect(ChooseImageCount(Caps(2, 3, {})) == 3 && ChooseImageCount(Caps(3, 3, {})) == 3 && ChooseImageCount(Caps(1, 0, {})) == 2 &&
               ChooseImageCount(Caps(2, 0, {})) == 3 && ChooseImageCount(Caps(4, 8, {})) == 5,
           "image count: min + 1, at least 2, clamped to max");
    VkSurfaceCapabilitiesKHR c = Caps(2, 3, {});
    Expect(ChooseCompositeAlpha(c) == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, "composite alpha: OPAQUE when supported");
    c.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    Expect(ChooseCompositeAlpha(c) == VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, "composite alpha: else the lowest supported bit (0x4)");
    c.supportedCompositeAlpha = 0;
    Expect(!ChooseCompositeAlpha(c), "composite alpha: none supported is refused");
    const VkPresentModeKHR withFifo[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR}, without[] = {VK_PRESENT_MODE_MAILBOX_KHR};
    Expect(FifoOffered(withFifo) && !FifoOffered(without), "present mode: FIFO found, its absence detected");
    c = Caps(2, 3, {});
    Expect(UsageSupported(c), "usage: TRANSFER_DST supported");
    c.supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    Expect(!UsageSupported(c), "usage: without TRANSFER_DST refused (CONTROL)");
}

void TestFitRect()
{
    const VkExtent2D src{1280, 720};
    Expect(FitRect(src, {1280, 720}) == Rect{0, 0, 1280, 720}, "fit: same size fills");
    Expect(FitRect(src, {2560, 1440}) == Rect{0, 0, 2560, 1440}, "fit: 2x fills");
    // 2560x1600 (16:10): full width, h = 2560*720/1280 = 1440, y0 = (1600-1440)/2 = 80.
    Expect(FitRect(src, {2560, 1600}) == Rect{0, 80, 2560, 1520}, "fit: 16:10 letterbox (0,80)-(2560,1520)");
    // 1000x1000: h = 1000*720/1280 = 562.5 -> 562, y0 = (1000-562)/2 = 219.
    Expect(FitRect(src, {1000, 1000}) == Rect{0, 219, 1000, 781}, "fit: square letterbox (0,219)-(1000,781)");
    // 3000x1000: w = 1000*1280/720 = 1777.7 -> 1777, x0 = (3000-1777)/2 = 611.
    Expect(FitRect(src, {3000, 1000}) == Rect{611, 0, 2388, 1000}, "fit: wide pillarbox (611,0)-(2388,1000)");
    // 1x1: 1*720/1280 = 0 -> at least 1.
    Expect(FitRect(src, {1, 1}) == Rect{0, 0, 1, 1}, "fit: 1x1 never empty");
    Expect(FitRect(src, {0, 720}) == Rect{}, "fit: an empty target gives an empty rectangle");
}

void TestResults()
{
    Expect(ClassifyAcquire(VK_SUCCESS) == AcquireAction::Present && ClassifyAcquire(VK_SUBOPTIMAL_KHR) == AcquireAction::PresentThenRecreate &&
               ClassifyAcquire(VK_ERROR_OUT_OF_DATE_KHR) == AcquireAction::Recreate && ClassifyAcquire(VK_TIMEOUT) == AcquireAction::Skip &&
               ClassifyAcquire(VK_NOT_READY) == AcquireAction::Skip && ClassifyAcquire(VK_ERROR_SURFACE_LOST_KHR) == AcquireAction::Fail &&
               ClassifyAcquire(VK_ERROR_DEVICE_LOST) == AcquireAction::Fail,
           "acquire: success, suboptimal (use it), out of date, timeout/not ready, lost");
    Expect(ClassifyPresent(VK_SUCCESS) == PresentAction::Ok && ClassifyPresent(VK_SUBOPTIMAL_KHR) == PresentAction::Recreate &&
               ClassifyPresent(VK_ERROR_OUT_OF_DATE_KHR) == PresentAction::Recreate && ClassifyPresent(VK_ERROR_SURFACE_LOST_KHR) == PresentAction::Fail &&
               ClassifyPresent(VK_TIMEOUT) == PresentAction::Fail,
           "present: ok, recreate, fail (VK_TIMEOUT is not a present result: fail closed)");
}

void TestPlanner()
{
    using D = SwapchainPlanner::Decision;
    constexpr uint64_t s = 1'000'000'000, settle = SwapchainPlanner::kSettleNs;
    SwapchainPlanner p;
    Expect(p.Decide(0) == D::SkipMinimized, "planner: no window size yet -> skip");
    p.WindowResized({1280, 720}, 0);
    Expect(p.Decide(0) == D::Create, "planner: a size and no swapchain -> create");
    p.Created(false);
    Expect(p.Decide(1) == D::Present && p.Retired() == 0, "planner: created -> present");
    p.WindowResized({1600, 900}, 10 * s);
    Expect(p.Decide(10 * s + settle - 1) == D::Present, "planner: a resize is not followed before it settles (250 ms - 1 ns)");
    Expect(p.Decide(10 * s + settle) == D::Recreate, "planner: followed once settled (250 ms)");
    p.Created(true);
    Expect(p.Decide(11 * s) == D::Present && p.Retired() == 1, "planner: recreated, one retired");
    p.WindowResized({1600, 900}, 20 * s);  // the same size again: not a change (its time is not reset)
    Expect(p.Decide(20 * s) == D::Present, "planner: the same size again is no resize");
    p.MarkOutOfDate();
    Expect(p.Decide(20 * s) == D::Recreate, "planner: out of date recreates at once (no settling)");
    p.Created(true);
    p.MarkSuboptimal();
    Expect(p.Decide(20 * s) == D::Recreate, "planner: suboptimal recreates when the size has settled");
    p.Created(true);
    p.WindowMinimized(true);
    Expect(p.Decide(30 * s) == D::SkipMinimized, "planner: minimized -> skip");
    p.WindowMinimized(false);
    Expect(p.Decide(30 * s) == D::Present, "planner: restored -> present");
    p.WindowResized({0, 0}, 31 * s);
    Expect(p.Decide(40 * s) == D::SkipMinimized, "planner: a zero-sized window -> skip, never a zero swapchain");
    p.WindowResized({1600, 900}, 41 * s);
    Expect(p.Decide(41 * s) == D::Present, "planner: back to the swapchain's window size -> present");
    // CONTROL: the surface may give the swapchain another extent than the window; comparing with the window size the
    // swapchain was made for means no endless recreation.
    for (int i = 0; i < 100; i++)
        if (p.Decide(50 * s + uint64_t(i) * s) != D::Present)
        {
            Expect(false, "planner: a steady window never recreates (CONTROL)");
            break;
        }
    // The cap: 3 retired so far; 5 more reach kMaxRetired (8).
    for (uint64_t i = 0; p.Retired() < SwapchainPlanner::kMaxRetired; i++)
    {
        p.WindowResized({uint32_t(1000 + i), 700}, 100 * s + i * s);
        if (p.Decide(100 * s + i * s + settle) != D::Recreate)
            break;
        p.Created(true);
    }
    Expect(p.Retired() == SwapchainPlanner::kMaxRetired, "planner: resizes recreate up to the cap (8 retired)");
    p.WindowResized({1920, 1080}, 200 * s);
    Expect(p.Decide(201 * s) == D::Present, "planner: at the cap a resize is no longer followed (presented scaled)");
    p.MarkOutOfDate();
    Expect(p.Decide(201 * s) == D::Fail, "planner: out of date at the cap -> a named stop");
}

void TestHandOverSequences()
{
    using W = FrameHandOver::Write;
    FrameHandOver h;
    Expect(h.Current() == FrameHandOver::State::Idle && !h.Take(1'000'000), "hand-over: idle, nothing to take (1 ms bound)");
    Expect(!h.Finish(), "hand-over: Finish without a frame is refused");
    Expect(h.BeginWrite(0) == W::Ready && h.Offer(1), "hand-over: write, offer swap 1");
    Expect(!h.Offer(2), "hand-over: a second offer while one is pending is refused (BeginWrite first)");
    Expect(h.BeginWrite(0) == W::Dropped && h.Dropped() == 1 && h.Current() == FrameHandOver::State::Idle,
           "hand-over: the next write withdraws an untaken frame (dropped 1), never waits for the main thread to take it");
    Expect(h.Offer(3) && h.Take(0) == std::optional<uint64_t>(3) && h.Current() == FrameHandOver::State::Presenting,
           "hand-over: the main thread takes swap 3");
    Expect(!h.Take(0), "hand-over: a frame is taken once (at most one present per swap)");
    const uint64_t t0 = NowNs();
    const W w = h.BeginWrite(50'000'000);
    const uint64_t waited = NowNs() - t0;
    Expect(w == W::TimedOut && waited >= 50'000'000 && h.Current() == FrameHandOver::State::Presenting,
           "hand-over: a write while presenting times out after the bound (50 ms) and changes nothing");
    std::thread main([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        h.Finish();
    });
    const uint64_t t1 = NowNs();
    const W after = h.BeginWrite(5'000'000'000ull);
    const uint64_t blocked = NowNs() - t1;
    main.join();
    Expect(after == W::Ready && blocked >= 20'000'000 && blocked < 4'000'000'000ull, "hand-over: a write waits for Finish (30 ms), not for the bound");
    Expect(h.Offered() == 2 && h.Dropped() == 1, "hand-over: 2 offered, 1 dropped");
}

// Two threads as in the window mode: the "command processor" writes the display image between BeginWrite and Offer; the
// "main thread" reads it between Take and Finish. The display must never be written while it is read.
void TestHandOverThreads()
{
    using W = FrameHandOver::Write;
    FrameHandOver h;
    std::atomic<bool> writing{false}, reading{false}, overlap{false}, done{false}, timedOut{false}, offerRefused{false};
    std::atomic<uint64_t> taken{0};
    constexpr uint64_t kSwaps = 2000;
    std::thread cp([&] {
        for (uint64_t swap = 1; swap <= kSwaps; swap++)
        {
            if (h.BeginWrite(2'000'000'000ull) == W::TimedOut)
                timedOut = true;
            writing = true;
            if (reading)
                overlap = true;
            std::this_thread::yield();
            writing = false;
            if (!h.Offer(swap))
                offerRefused = true;
        }
        done = true;
    });
    while (!done || h.Current() == FrameHandOver::State::Pending)
        if (h.Take(1'000'000))
        {
            reading = true;
            if (writing)
                overlap = true;
            std::this_thread::yield();
            reading = false;
            taken++;
            h.Finish();
        }
    cp.join();
    Expect(!overlap && !timedOut && !offerRefused, "hand-over, two threads: 2000 swaps, the display never written while read, no timeout");
    Expect(h.Offered() == kSwaps && taken + h.Dropped() == kSwaps && taken > 0,
           "hand-over, two threads: every offered frame was either presented once or dropped (taken + dropped = 2000)");
}

void TestEvents()
{
    Expect(ClassifyWindowEvent(SDL_EVENT_QUIT) == EventAction::Close && ClassifyWindowEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED) == EventAction::Close &&
               ClassifyWindowEvent(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) == EventAction::Resized &&
               ClassifyWindowEvent(SDL_EVENT_WINDOW_MINIMIZED) == EventAction::Minimized &&
               ClassifyWindowEvent(SDL_EVENT_WINDOW_RESTORED) == EventAction::Restored,
           "events: SDL's quit and close-requested close; pixel size, minimize, restore");
    Expect(ClassifyWindowEvent(SDL_EVENT_KEY_DOWN) == EventAction::None && ClassifyWindowEvent(SDL_EVENT_WINDOW_RESIZED) == EventAction::None &&
               ClassifyWindowEvent(SDL_EVENT_WINDOW_DESTROYED) == EventAction::None,
           "events: a key, a resize in points (the pixel event follows) and others do nothing (CONTROL)");
    int closes = 0, resizes = 0, minimized = 0, restored = 0;
    auto close = [&](const char*) { closes++; };
    auto resized = [&] { resizes++; };
    auto minimize = [&](bool m) { (m ? minimized : restored)++; };
    for (EventAction a : {EventAction::None, EventAction::Resized, EventAction::Minimized, EventAction::Restored})
        Dispatch(a, "test", close, resized, minimize);
    Expect(closes == 0 && resizes == 1 && minimized == 1 && restored == 1, "events: dispatch reaches exactly its handler; nothing else closes");
}

// The stop-on-close path in a child process: the close handler is a stand-in for window_sdl.cpp CloseStop (which calls
// Stop(StopCode::WindowClosed); its value 14 is asserted there), writing a stop line and _exit(14). The parent checks
// the child's exit code.
int ChildExit(uint32_t sdlEvent)
{
    const pid_t pid = fork();
    if (pid == 0)
    {
        Dispatch(ClassifyWindowEvent(sdlEvent), "SDL event", [](const char* what) {
            std::fprintf(stderr, "[stop 14] the window was closed by the user (%s)\n", what);
            _exit(14);
        }, [] {}, [](bool) {});
        _exit(0);  // not closed
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void TestCloseStop()
{
    Expect(ChildExit(SDL_EVENT_WINDOW_CLOSE_REQUESTED) == 14 && ChildExit(SDL_EVENT_QUIT) == 14, "close: close-requested and quit end in stop 14");
    Expect(ChildExit(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) == 0 && ChildExit(SDL_EVENT_KEY_DOWN) == 0, "close: other events do not stop (CONTROL)");
    // The stop runs on the main thread, which has no submission of its own (the presenter retires before returning to
    // the loop): it waits for the command processor's submission and completes. CONTROL: with a submission of its own
    // in flight it would not wait (OwnThread), which is why the presenter retires first.
    runtime::gpu::SubmissionGate gate;
    std::atomic<bool> entered{false};
    std::thread cp([&] {
        gate.Enter();
        entered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        gate.Leave();
    });
    while (!entered)
        std::this_thread::yield();
    gate.BeginStop();
    const auto outcome = gate.AwaitIdle(2'000'000'000ull, NowNs, [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    cp.join();
    Expect(outcome == runtime::gpu::SubmissionGate::Outcome::Completed, "close: the main thread's stop waits for the command processor's submission");
    runtime::gpu::SubmissionGate own;
    own.Enter();
    own.BeginStop();
    Expect(own.AwaitIdle(2'000'000'000ull, NowNs, [] {}) == runtime::gpu::SubmissionGate::Outcome::OwnThread,
           "close: a stop with the stopping thread's own submission in flight does not wait (CONTROL: why the presenter retires first)");
    own.Leave();
}
}  // namespace

int main()
{
    TestFormats();
    TestExtentAndCounts();
    TestFitRect();
    TestResults();
    TestPlanner();
    TestHandOverSequences();
    TestHandOverThreads();
    TestEvents();
    TestCloseStop();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
