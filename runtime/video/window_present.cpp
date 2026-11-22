// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// The window mode's presenter (window_present.h). The swapchain creation and the blit-and-present follow ForzaRecomp
// runtime/video/presenter.cpp:200-251 and 332-398 (Show; GPL-3.0-or-later, same owner), reviewed and
// re-written here. FM4 Recomp changes: the presenter only blits the display image the command processor already
// uploaded, gamma-ramped and captured (no upload, no capture of its own); every wait is bounded (ForzaRecomp acquired
// and waited its fence with UINT64_MAX); the submission goes through a GpuGuard (GPU_STOP, the gate, the bounded
// retire, a stranding writes GPU_STOP); one "rendered" semaphore per swapchain image instead of one in all (below);
// the acquire semaphore's wait stage is the transfer stage and the image's first barrier waits on that stage (below);
// the queue is shared under a bounded lock; resizes and out-of-date recreate (window_policy.h SwapchainPlanner); the
// display image is letterboxed and the bars cleared.
//
// Vulkan rules this code depends on (Vulkan 1.4 specification; VUIDs as the installed validation layer 1.4.363 names them):
//   - Acquire: the semaphore must be unsignalled with no pending operation (VUID-vkAcquireNextImageKHR-semaphore-01779).
//     One `acquired` semaphore suffices because presents are serialized: the submission that waited on it is retired
//     (fence confirmed) before the next acquire, and an acquire that returns no image (timeout, out of date) leaves it
//     untouched.
//   - The submission waits on `acquired` at VK_PIPELINE_STAGE_TRANSFER_BIT, and the swapchain image's first barrier
//     (UNDEFINED -> TRANSFER_DST_OPTIMAL) has srcStageMask TRANSFER: the layout transition must come after the semaphore
//     wait, which only a barrier whose first scope includes the wait's stage guarantees (the semaphore wait's second
//     synchronization scope is limited to pWaitDstStageMask). UNDEFINED discards the old contents, which the clear
//     replaces anyway.
//   - The image must be in PRESENT_SRC_KHR when presented (VUID-VkPresentInfoKHR-pImageIndices-01430): the last barrier
//     goes TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR with dstStageMask BOTTOM_OF_PIPE and dstAccessMask 0, since
//     vkQueuePresentKHR performs the visibility operations itself (spec, WSI "vkQueuePresentKHR").
//   - The present's wait semaphore: a binary semaphore signalled by vkQueueSubmit must be unsignalled with no pending
//     wait (VUID-vkQueueSubmit-pSignalSemaphores-00067), and a present's semaphore wait is not covered by any fence. So
//     one `rendered` semaphore per swapchain image, indexed by the acquired image: when image i is acquired again, the
//     presentation engine has finished its previous present, whose wait on rendered[i] has therefore executed (Khronos
//     Vulkan Guide, "Swapchain Semaphore Reuse").
//   - Blit: BLIT_SRC on the display format and BLIT_DST on the swapchain format (VUID-vkCmdBlitImage-srcImage-01999,
//     -dstImage-02000), LINEAR filtering only with SAMPLED_IMAGE_FILTER_LINEAR on the source (-filter-02001), else
//     NEAREST. Clear: TRANSFER_DST on the swapchain format (vkCmdClearColorImage).
//   - A retired swapchain (oldSwapchain) is never destroyed during the run: VUID-vkDestroySwapchainKHR-swapchain-01282
//     requires every use of its images complete, and present completion is not observable without
//     VK_EXT_swapchain_maintenance1 (window_policy.h SwapchainPlanner caps how many are kept). Its `rendered`
//     semaphores are kept with it for the same reason.
#include "window_present.h"
#include "gpu_guard.h"
#include "renderer.h"
#include "stops.h"
#include "kernel/extended_view.h"
#include "window_sdl.h"
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

namespace runtime::video {
namespace {

constexpr uint64_t kAcquireTimeoutNs = 100'000'000;  // MoltenVK acquires without waiting for a drawable (it takes the drawable
                                                     // in the submission, below); a bound anyway
constexpr uint32_t kMaxConsecutiveSkips = 120;       // acquire timeouts in a row (~2 s of 60 Hz frames): a named stop
constexpr uint64_t kReportEvery = 300;               // presents between two timing lines

PresenterSetup setup;
window::FrameHandOver handOver;
window::SwapchainPlanner planner;
VkSwapchainKHR swapchain = VK_NULL_HANDLE;
VkSurfaceFormatKHR surfaceFormat{};
VkExtent2D extent{};
std::vector<VkImage> images;
std::vector<VkSemaphore> rendered;  // one per image of the current swapchain
struct Retired
{
    VkSwapchainKHR swapchain;
    std::vector<VkSemaphore> rendered;
};
std::vector<Retired> retired;  // kept to the end of the run (above)
VkSemaphore acquired = VK_NULL_HANDLE;
VkCommandPool pool = VK_NULL_HANDLE;
VkCommandBuffer commands = VK_NULL_HANDLE;
VkFence fence = VK_NULL_HANDLE;
VkFilter filter = VK_FILTER_NEAREST;
uint32_t submitImage = 0;  // the acquired image of the submission being made (its rendered semaphore)
uint32_t consecutiveSkips = 0;

struct Stats
{
    uint64_t presented = 0, minimized = 0, acquireSkips = 0, outOfDate = 0, recreated = 0;
    uint64_t maxLockNs = 0, maxAcquireNs = 0, maxSubmitNs = 0, maxPresentNs = 0, maxRetireNs = 0;
} stats;

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        Stop(StopCode::Setup, "window: %s failed (VkResult %d)", what, int(result));
}

void Require(const Verdict& v)
{
    if (!v.ok)
        Stop(v.stranded ? StopCode::GpuStranded : StopCode::Setup, "window: %s", v.reason.c_str());
}

uint64_t NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* FormatName(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    default: return "other";
    }
}

// The presenter's slot: its command buffer and fence, waiting on `acquired` and signalling rendered[submitImage].
class PresenterBackend final : public SubmissionBackend<int>
{
public:
    int Submit(uint32_t) override
    {
        if (const VkResult r = vkResetFences(setup.dev->device, 1, &fence); r != VK_SUCCESS)
            return int(r);
        const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &wait;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[submitImage];
        return int(vkQueueSubmit(setup.dev->queue, 1, &submit, fence));
    }
    int WaitFence(uint32_t, uint64_t timeoutNs) override { return int(vkWaitForFences(setup.dev->device, 1, &fence, VK_TRUE, timeoutNs)); }
    int FenceStatus(uint32_t) override { return int(vkGetFenceStatus(setup.dev->device, fence)); }
    void Destroy(int&) override {}  // the presenter holds no per-frame resource
} backend;
std::optional<GpuGuard<int, 1>> guard;

bool hdrSwapchain = false, hdrFallbackNamed = false;  // FM4_HDR: the current swapchain is the HDR pair; the fallback was said
std::atomic<VkImage> hdrImage{VK_NULL_HANDLE};        // the linear-light picture (PresenterSetup::hdr, or SetPresenterHdr later)
std::atomic<bool> hdrRefused{false};
std::atomic<float> stretch{1.0f};  // Extended View: the handed-over frame's horizontal stretch (SetPresenterStretch)
VkFilter hdrFilter = VK_FILTER_NEAREST;

bool FormatUsable(VkFormat f)
{
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(setup.dev->gpu, f, &p);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (p.optimalTilingFeatures & needed) == needed;
}

// (Re)creates the swapchain for the planner's window size. False when the surface reports a zero extent (minimized
// between the event and now): nothing changes and the frame is skipped.
bool CreateSwapchain()
{
    const VulkanDevice& d = *setup.dev;
    VkSurfaceCapabilitiesKHR caps{};
    Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.gpu, d.surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    uint32_t count = 0;
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, d.surface, &count, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> formats(count);
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, d.surface, &count, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    count = 0;
    Check(vkGetPhysicalDeviceSurfacePresentModesKHR(d.gpu, d.surface, &count, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR");
    std::vector<VkPresentModeKHR> modes(count);
    Check(vkGetPhysicalDeviceSurfacePresentModesKHR(d.gpu, d.surface, &count, modes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");

    // FM4 Recomp (M9, FM4_HDR): the HDR pair when asked and offered; otherwise standard range, said once.
    window::FormatChoice format;
    if (hdrImage.load() != VK_NULL_HANDLE)
    {
        format = window::ChooseHdrSurfaceFormat(formats, FormatUsable);
        if (!format.ok)
            hdrRefused = true;
        if (!format.ok && !hdrFallbackNamed)
        {
            hdrFallbackNamed = true;
            std::fprintf(stderr, "[window] FM4_HDR: %s (%zu format(s) offered%s): standard range\n", format.reason.c_str(), formats.size(),
                         d.swapchainColorspace ? "" : "; VK_EXT_swapchain_colorspace is not available");
        }
    }
    hdrSwapchain = format.ok;
    if (!format.ok)
        format = window::ChooseSurfaceFormat(formats, FormatUsable);
    if (!format.ok)
        Stop(StopCode::Setup, "window: %s", format.reason.c_str());
    if (!window::UsageSupported(caps))
        Stop(StopCode::Setup, "window: swapchain images cannot be transfer destinations (supportedUsageFlags %X)", unsigned(caps.supportedUsageFlags));
    if (!window::FifoOffered(modes))
        Stop(StopCode::Setup, "window: the surface does not list VK_PRESENT_MODE_FIFO_KHR (the spec requires it)");
    const std::optional<VkCompositeAlphaFlagBitsKHR> composite = window::ChooseCompositeAlpha(caps);
    if (!composite)
        Stop(StopCode::Setup, "window: the surface supports no composite alpha mode");
    const std::optional<VkExtent2D> chosen = window::ChooseExtent(caps, planner.Window());
    if (!chosen)
        return false;

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = d.surface;
    info.minImageCount = window::ChooseImageCount(caps);
    info.imageFormat = format.format.format;
    info.imageColorSpace = format.format.colorSpace;
    info.imageExtent = *chosen;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;  // one queue family (vulkan_device.cpp)
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = *composite;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;  // VK_NULL_HANDLE at creation; the current one (retired by this call) on recreation
    VkSwapchainKHR created = VK_NULL_HANDLE;
    Check(vkCreateSwapchainKHR(d.device, &info, nullptr, &created), "vkCreateSwapchainKHR");
    const bool retiring = swapchain != VK_NULL_HANDLE;
    if (retiring)
        retired.push_back({swapchain, std::move(rendered)});  // never destroyed during the run (file comment)
    rendered.clear();
    swapchain = created;
    surfaceFormat = format.format;
    extent = *chosen;
    count = 0;
    Check(vkGetSwapchainImagesKHR(d.device, swapchain, &count, nullptr), "vkGetSwapchainImagesKHR");
    images.resize(count);
    Check(vkGetSwapchainImagesKHR(d.device, swapchain, &count, images.data()), "vkGetSwapchainImagesKHR");
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    rendered.resize(count);
    for (VkSemaphore& s : rendered)
        Check(vkCreateSemaphore(d.device, &semaphore, nullptr, &s), "vkCreateSemaphore");
    planner.Created(retiring);
    stats.recreated += retiring ? 1 : 0;
    std::fprintf(stderr, "[window] swapchain: %s, %ux%u, FIFO, %u images (window %ux%u pixels; %u retired, kept)%s\n", FormatName(surfaceFormat.format),
                 extent.width, extent.height, count, planner.Window().width, planner.Window().height, planner.Retired(),
                 hdrSwapchain ? "; HDR: extended linear sRGB (FM4_HDR)" : "");
    return true;
}

void ImageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Clear the acquired image black, blit the display image letterboxed into it, make it presentable.
void Record(VkImage target)
{
    Check(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer");  // its previous submission is retired (Retire below)
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
    ImageBarrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    ImageBarrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);  // clear, then blit (write after write)
    // The display image is in TRANSFER_SRC_OPTIMAL: the command processor's submission that left it there (renderer.cpp
    // RecordReadback, whose barrier made the upload visible to transfer reads for every later command in submission
    // order) was retired before the hand-over.
    // FM4 Recomp (Extended View): a frame drawn with a widened main view fills the window's width up to its factor's aspect.
    const float factor = stretch.load(std::memory_order_relaxed);
    window::Rect r = window::FitRect(setup.displayExtent, extent);
    if (factor != 1.0f)
    {
        const runtime::view::Placement p = runtime::view::PlaceStretched(extent.width, extent.height, factor);
        r = {p.x0, p.y0, p.x1, p.y1};
    }
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(setup.displayExtent.width), int32_t(setup.displayExtent.height), 1};
    blit.dstOffsets[0] = {r.x0, r.y0, 0};
    blit.dstOffsets[1] = {r.x1, r.y1, 1};
    // FM4_HDR: an HDR swapchain takes the linear-light picture (hdr_output.h), left in TRANSFER_SRC_OPTIMAL by the same submission.
    vkCmdBlitImage(commands, hdrSwapchain ? hdrImage.load() : setup.display, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, hdrSwapchain ? hdrFilter : filter);
    ImageBarrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    Check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
}

void Report(uint64_t swap)
{
    std::fprintf(stderr, "[window] swap %llu: %llu presented, %llu dropped by the command processor (not taken in time), %llu skipped minimized, "
                         "%llu acquire timeouts, %llu out of date, %llu recreated; max ms: queue lock %.2f, acquire %.2f, submit %.2f "
                         "(includes MoltenVK's drawable wait), present %.2f, retire %.2f\n",
                 (unsigned long long)swap, (unsigned long long)stats.presented, (unsigned long long)handOver.Dropped(), (unsigned long long)stats.minimized,
                 (unsigned long long)stats.acquireSkips, (unsigned long long)stats.outOfDate, (unsigned long long)stats.recreated, stats.maxLockNs / 1e6,
                 stats.maxAcquireNs / 1e6, stats.maxSubmitNs / 1e6, stats.maxPresentNs / 1e6, stats.maxRetireNs / 1e6);
    stats.maxLockNs = stats.maxAcquireNs = stats.maxSubmitNs = stats.maxPresentNs = stats.maxRetireNs = 0;
}

void Finish()
{
    if (!handOver.Finish())
        Stop(StopCode::Setup, "window: the hand-over was not presenting at Finish (contract breach)");
}

}  // namespace

window::FrameHandOver& HandOver() { return handOver; }

uint64_t PresenterWaitBoundNs(uint64_t fenceTimeoutNs) { return fenceTimeoutNs + 1'000'000'000ull + kAcquireTimeoutNs; }

std::unique_lock<std::timed_mutex> LockQueue(std::timed_mutex& lock, uint64_t boundNs, const char* who)
{
    std::unique_lock<std::timed_mutex> held(lock, std::defer_lock);
    if (!held.try_lock_for(std::chrono::nanoseconds(boundNs)))
        Stop(StopCode::Setup, "renderer: %s: the Vulkan queue lock was not free within %llu ms (the other thread's submission or present "
                              "is blocked)", who, (unsigned long long)(boundNs / 1000000));
    return held;
}

void InitializePresenter(const PresenterSetup& s)
{
    setup = s;
    hdrImage = s.hdr;
    const VulkanDevice& d = *setup.dev;
    if (d.surface == VK_NULL_HANDLE || !d.swapchain)
        Stop(StopCode::Setup, "window: the device was created without a surface or VK_KHR_swapchain");
    VkFormatProperties display{};
    vkGetPhysicalDeviceFormatProperties(d.gpu, VK_FORMAT_A2B10G10R10_UNORM_PACK32, &display);
    if (!(display.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
        Stop(StopCode::Setup, "window: A2B10G10R10_UNORM images cannot be blit sources (VUID-vkCmdBlitImage-srcImage-01999)");
    filter = (display.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkFormatProperties linearLight{};
    vkGetPhysicalDeviceFormatProperties(d.gpu, VK_FORMAT_R16G16B16A16_SFLOAT, &linearLight);  // FM4_HDR: its source (the renderer checked BLIT_SRC)
    hdrFilter = (linearLight.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    guard.emplace(backend, *setup.gate, setup.gpuStopPath, setup.fenceTimeoutNs);
    Require(guard->Begin("window initialize"));
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = d.queueFamily;
    Check(vkCreateCommandPool(d.device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(d.device, &cb, &commands), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    Check(vkCreateFence(d.device, &fenceInfo, nullptr, &fence), "vkCreateFence");
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    Check(vkCreateSemaphore(d.device, &semaphore, nullptr, &acquired), "vkCreateSemaphore");
    planner.WindowResized(window::WindowPixels(), NowNs());
    if (planner.Decide(NowNs()) != window::SwapchainPlanner::Decision::Create || !CreateSwapchain())
        Stop(StopCode::Setup, "window: no swapchain at start (window %ux%u pixels)", planner.Window().width, planner.Window().height);
    if (hdrSwapchain)  // FM4_HDR
    {
        float potential = 1.0f;
        const float now = window::HdrHeadroom(&potential);
        std::fprintf(stderr, "[window] HDR output on: the display's headroom is %.2fx SDR white now, %.2fx at most%s\n", now, potential,
                     potential <= 1.01f ? " (a standard-range display: the picture stays the standard one unless the window moves to an HDR display)" : "");
    }
    std::fprintf(stderr, "[window] presenter: blit filter %s, acquire bound %llu ms, queue-lock and hand-over bound %llu ms\n",
                 filter == VK_FILTER_LINEAR ? "LINEAR" : "NEAREST", (unsigned long long)(kAcquireTimeoutNs / 1000000),
                 (unsigned long long)(PresenterWaitBoundNs(setup.fenceTimeoutNs) / 1000000));
}

void WindowEvent(window::EventAction action, VkExtent2D pixels)
{
    if (action == window::EventAction::Minimized || action == window::EventAction::Restored)
        planner.WindowMinimized(action == window::EventAction::Minimized);
    planner.WindowResized(pixels, NowNs());
}

bool PresenterHdr() { return hdrSwapchain; }
void SetPresenterHdr(VkImage image) { hdrImage = image; }
// FM4 Recomp (M9, live resolution, step 2): plain stores. The command processor calls this only between its BeginWrite and its next
// Offer, while this thread reads `setup.display` and `setup.displayExtent` only between Take and Finish (Record); the hand-over's
// mutex orders the two, and this thread's own submission that read the old image was retired before its Finish. The swapchain is
// untouched: it has the window's size, and the blit's source rectangle follows the new extent.
void SetPresenterDisplay(VkImage image, VkExtent2D displayExtent) { setup.display = image, setup.displayExtent = displayExtent; }
void SetPresenterStretch(float factor) { stretch.store(factor, std::memory_order_relaxed); }
bool PresenterHdrRefused() { return hdrRefused.load(); }

bool PresentHandedOverFrame(uint64_t waitNs)
{
    if (hdrSwapchain)  // FM4_HDR: the display's headroom now (it follows the brightness setting), for the next frame's pass
        SetHdrHeadroom(window::HdrHeadroom());
    const std::optional<uint64_t> swap = handOver.Take(waitNs);
    if (!swap)
        return false;
    Require(guard->Begin("window present"));  // GPU_STOP, a stop in progress, a stranding
    // FM4_HDR switched on while the game runs: the HDR picture exists from this frame on, the swapchain is still the standard one.
    // One rebuild through the planner (our own submission is retired, below, every frame); never the last retirements, which
    // window resizes need.
    if (hdrImage.load() != VK_NULL_HANDLE && !hdrSwapchain && !hdrRefused.load())
    {
        if (planner.Retired() + 2 < window::SwapchainPlanner::kMaxRetired)
            planner.MarkSuboptimal();  // Decide: a rebuild (the window's size has long settled)
        else
        {
            hdrRefused = true;
            std::fprintf(stderr, "[window] FM4_HDR: %u retired swapchains are kept already: no rebuild for HDR in this session (standard range; "
                                 "it applies at the next start)\n", planner.Retired());
        }
    }
    using Decision = window::SwapchainPlanner::Decision;
    switch (planner.Decide(NowNs()))
    {
    case Decision::SkipMinimized:
        stats.minimized++;
        Finish();
        return true;
    case Decision::Fail:
        Stop(StopCode::Unimplemented, "window: swap %llu: the swapchain is out of date and %u retired swapchains are kept already (the cap; "
                                      "destroying one needs VK_EXT_swapchain_maintenance1 present fences)", (unsigned long long)*swap, planner.Retired());
    case Decision::Create:
    case Decision::Recreate:
        // Our own submission is retired (below, every frame); presents still pending keep the retired swapchain alive.
        if (!CreateSwapchain())
        {
            stats.minimized++;
            Finish();
            return true;
        }
        break;
    case Decision::Present: break;
    }

    uint64_t t = NowNs();
    uint32_t index = 0;
    const VkResult acquiredResult = vkAcquireNextImageKHR(setup.dev->device, swapchain, kAcquireTimeoutNs, acquired, VK_NULL_HANDLE, &index);
    stats.maxAcquireNs = std::max(stats.maxAcquireNs, NowNs() - t);
    const window::AcquireAction a = window::ClassifyAcquire(acquiredResult);
    if (a == window::AcquireAction::Fail)
        Stop(StopCode::Setup, "window: swap %llu: vkAcquireNextImageKHR failed (VkResult %d)", (unsigned long long)*swap, int(acquiredResult));
    if (a == window::AcquireAction::Skip || a == window::AcquireAction::Recreate)
    {
        if (a == window::AcquireAction::Recreate)
        {
            planner.MarkOutOfDate();
            stats.outOfDate++;
            consecutiveSkips = 0;
        }
        else
        {
            stats.acquireSkips++;
            if (++consecutiveSkips >= kMaxConsecutiveSkips)
                Stop(StopCode::Setup, "window: swap %llu: %u acquires in a row timed out (%llu ms each)", (unsigned long long)*swap, consecutiveSkips,
                     (unsigned long long)(kAcquireTimeoutNs / 1000000));
        }
        Finish();
        return true;
    }
    consecutiveSkips = 0;
    if (a == window::AcquireAction::PresentThenRecreate)
        planner.MarkSuboptimal();
    if (index >= images.size())
        Stop(StopCode::Setup, "window: vkAcquireNextImageKHR returned image %u of %zu", index, images.size());

    Record(images[index]);
    submitImage = index;
    VkResult presentedResult = VK_SUCCESS;
    {
        t = NowNs();
        std::unique_lock<std::timed_mutex> queue = LockQueue(*setup.queueLock, PresenterWaitBoundNs(setup.fenceTimeoutNs), "window present");
        stats.maxLockNs = std::max(stats.maxLockNs, NowNs() - t);
        t = NowNs();
        Require(guard->Submit(0));
        stats.maxSubmitNs = std::max(stats.maxSubmitNs, NowNs() - t);
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        t = NowNs();
        presentedResult = vkQueuePresentKHR(setup.dev->queue, &present);  // judged after the retire (a stop with our own
        stats.maxPresentNs = std::max(stats.maxPresentNs, NowNs() - t);  // submission in flight would not wait for it)
    }
    t = NowNs();
    Require(guard->Retire(0));  // bounded; then the display image is free for the command processor (Finish)
    stats.maxRetireNs = std::max(stats.maxRetireNs, NowNs() - t);
    const window::PresentAction p = window::ClassifyPresent(presentedResult);
    if (p == window::PresentAction::Fail)
        Stop(StopCode::Setup, "window: swap %llu: vkQueuePresentKHR failed (VkResult %d)", (unsigned long long)*swap, int(presentedResult));
    if (p == window::PresentAction::Recreate)
    {
        if (presentedResult == VK_ERROR_OUT_OF_DATE_KHR)
        {
            planner.MarkOutOfDate();
            stats.outOfDate++;
        }
        else
            planner.MarkSuboptimal();
    }
    stats.presented++;
    if (stats.presented == 1)
        std::fprintf(stderr, "[window] first present: swap %llu, image %u of %zu\n", (unsigned long long)*swap, index, images.size());
    if (stats.presented % kReportEvery == 0)
        Report(*swap);
    Finish();
    return true;
}

}  // namespace runtime::video
