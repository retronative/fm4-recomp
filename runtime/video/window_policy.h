// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The pure parts of the window mode (fm4recomp --renderer window), CPU-tested
// in tests/runtime/test_window.cpp: no Vulkan call, no SDL call, no device. Vulkan types appear only as plain structs.
//   - the swapchain's parameters from the surface's capability structs (format, extent, image count, composite alpha,
//     present mode, usage), each refusal naming the Vulkan rule it would break;
//   - the letterbox rectangle of the 1280x720 display image in the swapchain image;
//   - the classification of vkAcquireNextImageKHR / vkQueuePresentKHR results;
//   - when to (re)create the swapchain (window resizes are debounced; retired swapchains are kept, never destroyed in
//     flight, so their number is capped);
//   - the frame hand-over between the command processor's thread (writes the display image at each XE_SWAP) and the
//     main thread (blits it to the swapchain and presents): bounded waits only.
// Every wait is
// bounded (ForzaRecomp waited on a condition variable and on UINT64_MAX); the command processor never waits for the
// main thread to *take* a frame (a pending frame is withdrawn and counted, so a stalled event loop, e.g. a live
// window drag, does not stall the guest); 8-bit B8G8R8A8_UNORM first; resizes and out-of-date recreate.
#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace runtime::video::window {

constexpr uint32_t kWindowWidth = 1280, kWindowHeight = 720;  // the initial window, in points (SDL_CreateWindow)

// ---- Swapchain parameters --------------------------------------------------------------------------------------------

struct FormatChoice
{
    bool ok = false;
    VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    std::string reason;
};

// The display image holds display-referred values (the guest's gamma ramp is already applied, renderer.cpp
// ApplyDisplayGamma), so the swapchain must store them unchanged: a UNORM format in SRGB_NONLINEAR, never an _SRGB format
// (whose writes would encode a second time). Preference: B8G8R8A8_UNORM (MoltenVK's usual first format), then the 10-bit
// A2B10G10R10_UNORM_PACK32 (the display image's own format), then R8G8B8A8_UNORM. `blitDst(format)` is the device's
// VK_FORMAT_FEATURE_BLIT_DST_BIT on optimal tiling (VUID-vkCmdBlitImage-dstImage-02000); a format without it is skipped.
template <typename BlitDst>
FormatChoice ChooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> offered, BlitDst blitDst)
{
    static constexpr VkFormat kPreferred[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R8G8B8A8_UNORM};
    for (VkFormat wanted : kPreferred)
        for (const VkSurfaceFormatKHR& f : offered)
            if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && blitDst(wanted))
                return {true, f, {}};
    return {false, {}, "the surface offers no UNORM B8G8R8A8, A2B10G10R10 or R8G8B8A8 format in SRGB_NONLINEAR that the device can blit to "
                       "(VUID-VkSwapchainCreateInfoKHR-imageFormat-01273, VUID-vkCmdBlitImage-dstImage-02000)"};
}

// The swapchain extent. A defined currentExtent must be used as is; 0xFFFFFFFF means the swapchain decides, so the
// window's pixel size, clamped to [minImageExtent, maxImageExtent] (Vulkan: VkSurfaceCapabilitiesKHR currentExtent).
// A zero width or height (a minimized window) yields nothing: no swapchain may have a zero extent
// (VUID-VkSwapchainCreateInfoKHR-imageExtent-01689); the frame is not presented.
inline std::optional<VkExtent2D> ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D windowPixels)
{
    VkExtent2D e = caps.currentExtent;
    if (e.width == UINT32_MAX && e.height == UINT32_MAX)
    {
        e.width = std::clamp(windowPixels.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        e.height = std::clamp(windowPixels.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (e.width == 0 || e.height == 0)
        return std::nullopt;
    return e;
}

// minImageCount + 1 (one image for the presentation engine to hold while another is drawn), at least 2, clamped to
// maxImageCount when that is not 0 (0: no limit). VUID-VkSwapchainCreateInfoKHR-minImageCount-01272: at most
// maxImageCount; the surface's own minimum is the lower bound the spec requires.
inline uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& caps)
{
    uint32_t n = std::max(caps.minImageCount + 1, 2u);
    if (caps.maxImageCount != 0)
        n = std::min(n, caps.maxImageCount);
    return n;
}

// OPAQUE when supported, else the lowest supported bit (VUID-VkSwapchainCreateInfoKHR-compositeAlpha-01280: exactly one
// bit, and it must be supported). Nothing supported is impossible per the spec ("at least one bit"), refused anyway.
inline std::optional<VkCompositeAlphaFlagBitsKHR> ChooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& caps)
{
    const VkCompositeAlphaFlagsKHR s = caps.supportedCompositeAlpha;
    if (s & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (uint32_t bit = 1; bit != 0 && bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1)
        if (s & bit)
            return VkCompositeAlphaFlagBitsKHR(bit);
    return std::nullopt;
}

// FIFO only: the spec requires every surface to support it ("VK_PRESENT_MODE_FIFO_KHR ... is required to be
// supported"), and it paces at the display's refresh. Checked anyway (VUID-VkSwapchainCreateInfoKHR-presentMode-01281).
inline bool FifoOffered(std::span<const VkPresentModeKHR> modes)
{
    return std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR) != modes.end();
}

// The swapchain images are blit and clear destinations: TRANSFER_DST must be in supportedUsageFlags
// (VUID-VkSwapchainCreateInfoKHR-presentMode-01427 for FIFO: "... are not in supportedUsageFlags", the installed layer's text).
inline bool UsageSupported(const VkSurfaceCapabilitiesKHR& caps)
{
    return (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
}

// Where the 16:9 display image goes in a swapchain image of `dst`: the largest rectangle with the source's aspect
// ratio, centred (letterbox or pillarbox), integer offsets. The bars are cleared to black first. Empty only when
// `dst` is empty.
struct Rect
{
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool operator==(const Rect&) const = default;
};
inline Rect FitRect(VkExtent2D src, VkExtent2D dst)
{
    if (src.width == 0 || src.height == 0 || dst.width == 0 || dst.height == 0)
        return {};
    // Compare dst.w / dst.h with src.w / src.h without rounding: dst.w * src.h vs dst.h * src.w (64-bit).
    uint64_t w = dst.width, h = dst.height;
    if (uint64_t(dst.width) * src.height > uint64_t(dst.height) * src.width)
        w = std::max<uint64_t>(1, uint64_t(dst.height) * src.width / src.height);  // pillarbox: full height
    else
        h = std::max<uint64_t>(1, uint64_t(dst.width) * src.height / src.width);   // letterbox: full width
    const int32_t x0 = int32_t((dst.width - w) / 2), y0 = int32_t((dst.height - h) / 2);
    return {x0, y0, x0 + int32_t(w), y0 + int32_t(h)};
}

// ---- Results -----------------------------------------------------------------------------------------------------------

enum class AcquireAction
{
    Present,              // VK_SUCCESS
    PresentThenRecreate,  // VK_SUBOPTIMAL_KHR: an image was acquired and its semaphore will be signalled, so it must be
                          // used (presented); the swapchain is recreated afterwards
    Recreate,             // VK_ERROR_OUT_OF_DATE_KHR: no image, the semaphore is untouched; recreate, skip this frame
    Skip,                 // VK_TIMEOUT / VK_NOT_READY: no image within the bound, the semaphore is untouched; skip this frame
    Fail,                 // anything else (surface or device lost, out of memory): a named stop
};
inline AcquireAction ClassifyAcquire(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return AcquireAction::Present;
    case VK_SUBOPTIMAL_KHR: return AcquireAction::PresentThenRecreate;
    case VK_ERROR_OUT_OF_DATE_KHR: return AcquireAction::Recreate;
    case VK_TIMEOUT:
    case VK_NOT_READY: return AcquireAction::Skip;
    default: return AcquireAction::Fail;
    }
}

enum class PresentAction
{
    Ok,
    Recreate,  // VK_SUBOPTIMAL_KHR or VK_ERROR_OUT_OF_DATE_KHR (the semaphore wait still executes: Vulkan vkQueuePresentKHR)
    Fail,
};
inline PresentAction ClassifyPresent(VkResult r)
{
    if (r == VK_SUCCESS)
        return PresentAction::Ok;
    if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR)
        return PresentAction::Recreate;
    return PresentAction::Fail;
}

// ---- When to (re)create --------------------------------------------------------------------------------------------------

// Main thread only (window events and presentation both run there). Recreation retires the old swapchain with
// oldSwapchain; a retired swapchain is never destroyed during the run: its presents' completion cannot be observed
// without VK_EXT_swapchain_maintenance1 present fences (VUID-vkDestroySwapchainKHR-swapchain-01282 needs every use of its
// images complete), and the process ends with _exit. So their number is capped, and window resizes are debounced:
// a new size is followed once it has been stable for kSettleNs.
class SwapchainPlanner
{
public:
    static constexpr uint64_t kSettleNs = 250'000'000;  // a live drag produces a size per event; follow it once it settles
    static constexpr uint32_t kMaxRetired = 8;

    enum class Decision
    {
        Present,        // use the current swapchain
        Create,         // no swapchain yet
        Recreate,       // retire the current one and create another (oldSwapchain)
        SkipMinimized,  // zero-sized or minimized window: no present, no swapchain change
        Fail,           // out of date with the retired cap reached: a named stop (the swapchain can no longer present)
    };

    // From the window's events (SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, _MINIMIZED, _RESTORED).
    void WindowResized(VkExtent2D pixels, uint64_t nowNs)
    {
        if (pixels.width != window_.width || pixels.height != window_.height)
            window_ = pixels, changedAtNs_ = nowNs;
    }
    void WindowMinimized(bool minimized) { minimized_ = minimized; }
    // From the results.
    void MarkSuboptimal() { suboptimal_ = true; }
    void MarkOutOfDate() { outOfDate_ = true; }

    Decision Decide(uint64_t nowNs) const
    {
        if (minimized_ || window_.width == 0 || window_.height == 0)
            return Decision::SkipMinimized;
        if (!created_)
            return Decision::Create;
        const bool canRetire = retired_ < kMaxRetired;
        if (outOfDate_)
            return canRetire ? Decision::Recreate : Decision::Fail;
        // Compared with the window size the swapchain was made for (not its own extent, which the surface may set
        // differently): otherwise a surface that reports another size would be recreated at every frame.
        const bool resized = window_.width != createdFor_.width || window_.height != createdFor_.height;
        if ((resized || suboptimal_) && canRetire && nowNs - changedAtNs_ >= kSettleNs)
            return Decision::Recreate;
        return Decision::Present;  // still presentable (the compositor scales a suboptimal one); followed once settled
    }

    // After a successful vkCreateSwapchainKHR for the current window size.
    void Created(bool retiredOne)
    {
        retired_ += retiredOne ? 1 : 0;
        created_ = true;
        createdFor_ = window_;
        suboptimal_ = outOfDate_ = false;
    }

    uint32_t Retired() const { return retired_; }
    VkExtent2D Window() const { return window_; }

private:
    VkExtent2D window_{0, 0}, createdFor_{0, 0};
    uint64_t changedAtNs_ = 0;
    uint32_t retired_ = 0;
    bool created_ = false, minimized_ = false, suboptimal_ = false, outOfDate_ = false;
};

// ---- The frame hand-over (command processor -> main thread) ---------------------------------------------------------------

// The display image is shared: the command processor writes it at each XE_SWAP (upload, readback for the capture), the
// main thread reads it (blit to the swapchain). One frame at a time:
//   Idle --Offer (CP, after the frame's submission retired)--> Pending --Take (main)--> Presenting --Finish (main)--> Idle
//   Pending --BeginWrite (CP, the next swap, before it writes the display image)--> Idle (the frame is dropped, counted)
// BeginWrite waits only while the main thread is Presenting, bounded by `timeoutNs` (the presenter's own waits are bounded:
// acquire timeout, the queue lock, its fence); a timeout is a named stop by the caller. So the guest is never blocked by
// an event loop that does not take frames (a live window drag runs a nested loop on macOS), only by a present in progress.
// At most one present per guest swap: a frame is taken once.
class FrameHandOver
{
public:
    enum class State
    {
        Idle,
        Pending,
        Presenting,
    };
    enum class Write
    {
        Ready,     // the display image is free
        Dropped,   // it was free after withdrawing a frame the main thread had not taken
        TimedOut,  // the main thread is still presenting from it: the caller stops
    };

    Write BeginWrite(uint64_t timeoutNs)
    {
        std::unique_lock lock(mutex_);
        if (state_ == State::Pending)
        {
            state_ = State::Idle;
            dropped_++;
            return Write::Dropped;
        }
        if (!changed_.wait_for(lock, std::chrono::nanoseconds(timeoutNs), [this] { return state_ != State::Presenting; }))
            return Write::TimedOut;
        return Write::Ready;
    }
    // False (a contract breach, the caller stops) unless Idle: BeginWrite must come first.
    bool Offer(uint64_t swap)
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::Idle)
            return false;
        state_ = State::Pending;
        swap_ = swap;
        offered_++;
        changed_.notify_all();
        return true;
    }
    // Main thread: waits up to `timeoutNs` for a pending frame and takes it.
    std::optional<uint64_t> Take(uint64_t timeoutNs)
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::nanoseconds(timeoutNs), [this] { return state_ == State::Pending; }))
            return std::nullopt;
        state_ = State::Presenting;
        return swap_;
    }
    // Main thread, after its submission was retired (or the frame skipped). False unless Presenting.
    bool Finish()
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::Presenting)
            return false;
        state_ = State::Idle;
        changed_.notify_all();
        return true;
    }

    State Current() const
    {
        std::lock_guard lock(mutex_);
        return state_;
    }
    uint64_t Offered() const
    {
        std::lock_guard lock(mutex_);
        return offered_;
    }
    uint64_t Dropped() const
    {
        std::lock_guard lock(mutex_);
        return dropped_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    State state_ = State::Idle;
    uint64_t swap_ = 0, offered_ = 0, dropped_ = 0;
};

// ---- Window events ------------------------------------------------------------------------------------------------------

// What the main thread's loop does with an event (window_sdl.cpp maps SDL's event types onto these; the mapping is tested
// against SDL's own constants in tests/runtime/test_window.cpp).
enum class EventAction
{
    None,
    Close,      // SDL_EVENT_QUIT, SDL_EVENT_WINDOW_CLOSE_REQUESTED: a named stop (StopCode::WindowClosed), never a crash
    Resized,    // SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: SwapchainPlanner::WindowResized
    Minimized,  // SDL_EVENT_WINDOW_MINIMIZED
    Restored,   // SDL_EVENT_WINDOW_RESTORED
};

}  // namespace runtime::video::window
