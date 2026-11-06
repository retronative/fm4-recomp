// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The present path (upload a CPU-detiled front buffer, copy to a display image, read back) follows
// ForzaRecomp runtime/video/presenter.cpp:332-420 (Show; GPL-3.0-or-later, same owner),
// reviewed and re-written here. FM4 Recomp changes: offscreen (no swapchain); every submission goes
// through GpuGuard (ForzaRecomp's presenter waited with UINT64_MAX, unbounded); the staging buffer is
// a per-frame resource held by the slot's lifetime and destroyed only after a confirmed completion;
// captures go through the capped ring.
#include "renderer.h"
#include "capture_store.h"
#include "front_buffer.h"
#include "gpu_guard.h"
#include "png.h"
#include "vulkan_device.h"
#include "kernel/virtual_memory.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

// Added to stops.h / stops.cpp by the stop drain.
void SetStopQuiesce(void (*quiesce)(), void (*signalReport)());

namespace runtime::video {
namespace {

constexpr uint32_t kDisplayWidth = 1280, kDisplayHeight = 720;  // the accepted video mode (imports_video.cpp)
constexpr uint32_t kSlots = 1;                                     // unit 4; D-b's 12 slots come with unit 5
constexpr size_t kCaptureRecords = 16;
constexpr uint64_t kCaptureBytes = 64ull << 20;                    // 16 stored-deflate 1280x720 PNGs are ~44 MB

// A buffer the recorded commands use and that is destroyed only after their completion is confirmed.
struct Transient
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

Options options;
bool enabled = false;
VulkanDevice dev;
struct Slot
{
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
} slots[kSlots];
VkImage display = VK_NULL_HANDLE;
VkDeviceMemory displayMemory = VK_NULL_HANDLE;
Transient readback;  // persistent: read on the CPU only after the slot's completion is confirmed
uint64_t transientsDestroyed = 0, drawsSinceSwap = 0, shadersSinceSwap = 0;
gpu::SubmissionGate gate;

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        Stop(StopCode::Setup, "renderer: %s failed (VkResult %d)", what, int(result));
}

// The real device side of a slot.
class VulkanBackend final : public SubmissionBackend<Transient>
{
public:
    int Submit(uint32_t s) override
    {
        if (const VkResult r = vkResetFences(dev.device, 1, &slots[s].fence); r != VK_SUCCESS)
            return int(r);  // nothing submitted, but the guard treats it as a failed submit (stranded): fail closed
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slots[s].commands;
        return int(vkQueueSubmit(dev.queue, 1, &submit, slots[s].fence));
    }
    int WaitFence(uint32_t s, uint64_t timeoutNs) override { return int(vkWaitForFences(dev.device, 1, &slots[s].fence, VK_TRUE, timeoutNs)); }
    int FenceStatus(uint32_t s) override { return int(vkGetFenceStatus(dev.device, slots[s].fence)); }
    void Destroy(Transient& t) override
    {
        vkDestroyBuffer(dev.device, t.buffer, nullptr);
        vkFreeMemory(dev.device, t.memory, nullptr);  // implicitly unmapped
        t = Transient{};
        transientsDestroyed++;
    }
};

VulkanBackend backend;
std::optional<GpuGuard<Transient, kSlots>> guard;
std::optional<CaptureStore<kCaptureRecords>> captures;

// A refused verdict ends the run: a stranding with its own code (GPU_STOP was written), anything
// else as a setup failure.
void Require(const Verdict& v)
{
    if (!v.ok)
        Stop(v.stranded ? kStopGpuStranded : StopCode::Setup, "renderer: %s", v.reason.c_str());
}

Transient MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    Transient b;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    Check(vkCreateBuffer(dev.device, &info, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(dev.device, b.buffer, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        MemoryType(dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(dev.device, &allocate, nullptr, &b.memory), "vkAllocateMemory");
    Check(vkBindBufferMemory(dev.device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    Check(vkMapMemory(dev.device, b.memory, 0, size, 0, &b.mapped), "vkMapMemory");
    return b;
}

void ImageBarrier(VkCommandBuffer c, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = display;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Starts recording slot `s`: only while its previous submission is confirmed complete (idle).
VkCommandBuffer BeginSlot(uint32_t s)
{
    if (guard->SlotState(s) != gpu::SubmissionLifetime<Transient>::State::Idle)
        Stop(StopCode::Setup, "renderer: slot %u is recorded while not idle", s);
    Check(vkResetCommandBuffer(slots[s].commands, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(slots[s].commands, &begin), "vkBeginCommandBuffer");
    return slots[s].commands;
}

// The display image (already in TRANSFER_DST_OPTIMAL after `written`) -> the readback buffer, made
// visible to the host. Ends the recording.
void EndWithReadback(VkCommandBuffer c)
{
    ImageBarrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kDisplayWidth, kDisplayHeight, 1};
    vkCmdCopyImageToBuffer(c, display, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    VkBufferMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = readback.buffer;
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
    Check(vkEndCommandBuffer(c), "vkEndCommandBuffer");
}

// The read-back display image (A2B10G10R10: R bits 0-9) -> a capture: 8-bit RGB keeps each channel's
// top 8 bits (ForzaRecomp presenter.cpp:396-405).
void Capture(uint64_t id, const char* what)
{
    const uint32_t* pixels = static_cast<const uint32_t*>(readback.mapped);
    std::vector<uint8_t> rgb(size_t(kDisplayWidth) * kDisplayHeight * 3);
    for (size_t i = 0; i < size_t(kDisplayWidth) * kDisplayHeight; i++)
    {
        const uint32_t v = pixels[i];
        rgb[3 * i] = uint8_t((v & 0x3FF) >> 2), rgb[3 * i + 1] = uint8_t(((v >> 10) & 0x3FF) >> 2), rgb[3 * i + 2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
    }
    const std::vector<uint8_t> png = EncodePngRgb8(kDisplayWidth, kDisplayHeight, rgb);
    std::string error;
    const bool ok = captures->Add(id, png.size(), [&](const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return false;
        const bool written = std::fwrite(png.data(), 1, png.size(), f) == png.size();
        return std::fclose(f) == 0 && written;
    }, error);
    if (!ok)
        Stop(StopCode::Setup, "renderer: capture of %s: %s", what, error.c_str());
    std::fprintf(stderr, "[gpu] %s captured: %s (ring %zu files, %llu bytes)\n", what, captures->PathOf(id).c_str(), captures->Count(),
                 (unsigned long long)captures->Bytes());
}

void CreateResources()
{
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(dev.gpu, VK_FORMAT_A2B10G10R10_UNORM_PACK32, &format);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & needed) != needed)
        Stop(StopCode::Setup, "renderer: A2B10G10R10_UNORM images cannot be transfer sources and destinations");
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    info.extent = {kDisplayWidth, kDisplayHeight, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Check(vkCreateImage(dev.device, &info, nullptr, &display), "vkCreateImage");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(dev.device, display, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(dev.device, &allocate, nullptr, &displayMemory), "vkAllocateMemory");
    Check(vkBindImageMemory(dev.device, display, displayMemory, 0), "vkBindImageMemory");
    readback = MakeBuffer(VkDeviceSize(kDisplayWidth) * kDisplayHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (Slot& s : slots)
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = dev.queueFamily;
        Check(vkCreateCommandPool(dev.device, &poolInfo, nullptr, &s.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb.commandPool = s.pool;
        cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(dev.device, &cb, &s.commands), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        Check(vkCreateFence(dev.device, &fence, nullptr, &s.fence), "vkCreateFence");
    }
}

// Device teardown, only when nothing's completion is unknown. A kept resource keeps the device too
// (destroying it would free memory the GPU may still use): the run stops with it reported.
void Shutdown()
{
    if (const size_t kept = guard->Shutdown(); kept != 0)
        Stop(kStopGpuStranded, "renderer: %zu resource(s) kept at shutdown (completion unknown); the device is not destroyed", kept);
    for (Slot& s : slots)
    {
        vkDestroyFence(dev.device, s.fence, nullptr);
        vkDestroyCommandPool(dev.device, s.pool, nullptr);  // frees its command buffer
        s = Slot{};
    }
    backend.Destroy(readback);
    vkDestroyImage(dev.device, display, nullptr);
    vkFreeMemory(dev.device, displayMemory, nullptr);
    DestroyVulkanDevice(dev);
    enabled = false;
}

uint64_t NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Expect(bool ok, const char* what)
{
    std::fprintf(stderr, "[gpu self-check] %s %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        Stop(StopCode::Setup, "renderer: self-check failed: %s", what);
}

}  // namespace

gpu::SubmissionGate& Submissions() { return gate; }
bool Enabled() { return enabled; }

bool Initialize(const Options& o)
{
    options = o;
    if (o.mode == Mode::Headless)
        return false;
    if (o.gpuStopPath == nullptr || o.captureDir == nullptr)
        Stop(StopCode::Setup, "renderer: --gpu-stop and --gpu-capture are required with a GPU renderer mode");
    const gpu::FenceTimeout timeout = gpu::FenceTimeoutFromEnv(o.fenceTimeoutEnv);
    if (timeout.malformed)  // fail closed: a mistyped limit is not silently replaced
        Stop(StopCode::Setup, "renderer: FM4_GPU_FENCE_TIMEOUT_MS=%s is not a decimal number of milliseconds", o.fenceTimeoutEnv);
    guard.emplace(backend, gate, o.gpuStopPath, gpu::FenceTimeoutNs(timeout.ms));
    Require(guard->Begin("renderer initialize"));  // GPU_STOP before any Vulkan call
    captures.emplace(o.captureDir, "frame_", kCaptureBytes);
    SetStopQuiesce(QuiesceForStop, ReportForSignalStop);
    std::fprintf(stderr, "[gpu] renderer %s: fence timeout %u ms, captures %s (at most %zu files, %llu MB), GPU_STOP %s\n",
                 o.mode == Mode::SelfCheck ? "self-check" : "present (draws not rendered)", timeout.ms, o.captureDir, kCaptureRecords,
                 (unsigned long long)(kCaptureBytes >> 20), o.gpuStopPath);
    dev = CreateVulkanDevice(o.validation);
    CreateResources();
    enabled = true;
    return true;
}

void Present(const uint8_t* base, const uint32_t fetch[6], uint32_t physical, uint32_t width, uint32_t height, uint64_t swap)
{
    if (!enabled)
        return;
    Require(guard->Begin("present"));
    const FrontBuffer f = DecodeFrontBuffer(fetch, physical, width, height);
    if (!f.ok)
        Stop(StopCode::Unimplemented, "renderer: swap %llu: %s", (unsigned long long)swap, f.error.c_str());
    if (width != kDisplayWidth || height != kDisplayHeight)
        Stop(StopCode::Unimplemented, "renderer: swap %llu: a %ux%u front buffer (only %ux%u)", (unsigned long long)swap, width, height,
             kDisplayWidth, kDisplayHeight);
    uint32_t guest = 0;
    if (!runtime::PhysicalToGuest(f.physical, guest))
        Stop(StopCode::Unimplemented, "renderer: swap %llu: front buffer %08X is in no view", (unsigned long long)swap, f.physical);
    // A plain CPU read while the title may still write the buffer: the guarded read
    // (write watch, external host memory) is unit 7.
    Transient staging = MakeBuffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    uint32_t* linear = static_cast<uint32_t*>(staging.mapped);
    if (!Detile32(base + guest, TiledSize32(f.pitch, height), linear, width, height, f.pitch))
        Stop(StopCode::Unimplemented, "renderer: swap %llu: front buffer shorter than its tiled size", (unsigned long long)swap);
    if (f.swapRedBlue)
        for (size_t i = 0; i < size_t(width) * height; i++)
            linear[i] = SwapRedBlue1010102(linear[i]);
    const VkCommandBuffer c = BeginSlot(0);
    if (!guard->Hold(0, staging))
        Stop(StopCode::Setup, "renderer: the staging buffer cannot be held (slot not idle)");
    ImageBarrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(c, staging.buffer, display, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    EndWithReadback(c);
    Require(guard->Submit(0));
    Require(guard->Retire(0));  // one bounded wait per swap: the one-frame throttle (execution model §2)
    Capture(swap, "front buffer");
    std::fprintf(stderr, "[gpu] swap %llu presented offscreen; %llu draw(s) and %llu shader load(s) since the last swap NOT rendered (unit 8)\n",
                 (unsigned long long)swap, (unsigned long long)drawsSinceSwap, (unsigned long long)shadersSinceSwap);
    drawsSinceSwap = shadersSinceSwap = 0;
}

void Draw(const uint8_t* base, uint32_t opcode)
{
    (void)base, (void)opcode;
    if (!enabled)
        return;
    // Unit 8 inserts here, in this order, before anything of the draw is recorded:
    //   1. the gate's in-memory state (no file access per draw; GPU_STOP is checked at every Submit,
    //      the single choke point to the queue, execution model §4);
    //   2. GpuGuard::DrawLoopWork(vs loop constants, index count, ps loop constants,
    //      gpu::PixelInvocations(PA_SC_WINDOW_SCISSOR_TL, _BR, RB_SURFACE_INFO)): a refusal stops;
    //   3. a shader class never run before (loops, jumps, calls) stops unless the run approves it
    //      (the isolation rule, D-h);
    //   4. recording into the open slot, resources held by the slot's lifetime.
    const gpu::SubmissionGate::View v = gate.Load();
    if (v.stopping || v.stranded)
        Stop(v.stranded ? kStopGpuStranded : StopCode::Setup, "renderer: draw after the GPU stopped");
    drawsSinceSwap++;
}

void LoadShader(uint32_t type, const uint32_t* words, uint32_t count)
{
    (void)type, (void)words, (void)count;
    if (enabled)
        shadersSinceSwap++;  // unit 8: runtime/shader TranslateShader, refused shaders stop with their hash
}

void QuiesceForStop()
{
    if (!gate.BeginStop())
        return;  // another stop drains
    const uint64_t limit = (guard ? guard->FenceTimeoutNs() : gpu::FenceTimeoutNs(gpu::kFenceTimeoutDefaultMs)) + 1000000000ull;
    const auto outcome = gate.AwaitIdle(limit, NowNs, [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    const gpu::SubmissionGate::View v = gate.Load();
    static const char* const names[] = {"idle", "completed", "STRANDED (completion unknown)", "TIMED OUT", "own thread in flight"};
    std::fprintf(stderr, "[gpu at stop] %s; %u submission(s) counted in flight\n", names[int(outcome)], v.inFlight);
}

void ReportForSignalStop()
{
    // Signal-safe only: an atomic or and load, write(2). Not waited for: completion UNKNOWN at exit.
    gate.BeginStop();
    const gpu::SubmissionGate::View v = gate.Load();
    if (v.inFlight == 0)
        return;
    char text[] = "[gpu at stop] signal stop: 0000000000 submission(s) in flight, not waited for; GPU completion UNKNOWN at exit\n";
    uint32_t n = v.inFlight;
    for (int i = 0; i < 10; i++, n /= 10)
        text[36 - i] = char('0' + n % 10);
    (void)!::write(STDERR_FILENO, text, sizeof text - 1);
}

void RunSelfCheck()
{
    if (!enabled || options.mode != Mode::SelfCheck)
        Stop(StopCode::Setup, "renderer: the self-check needs --renderer self-check");
    const std::string dir = options.captureDir;

    // 1. One clear and one upload into the display image, read back: the device works and the
    //    lifetime defers the staging buffer's destruction until the completion is confirmed.
    //    Expected words: A2B10G10R10 packs R in bits 0-9, G 10-19, B 20-29, A 30-31 (Vulkan
    //    VK_FORMAT_A2B10G10R10_UNORM_PACK32). Clear (1, 0, 1, 1): exact in UNORM (0 and 1 map to 0
    //    and 1023) -> 3 << 30 | 1023 << 20 | 0 << 10 | 1023 = 0xFFF003FF. The pattern is copied
    //    bit for bit: texel (x, y) of a 32x32 block at (64, 64) = 3 << 30 | 0x155 << 20 | (32 y) << 10 | 32 x.
    Require(guard->Begin("self-check clear"));
    Transient staging = MakeBuffer(32 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++)
            static_cast<uint32_t*>(staging.mapped)[y * 32 + x] = 3u << 30 | 0x155u << 20 | (32 * y) << 10 | (32 * x);
    const VkCommandBuffer c = BeginSlot(0);
    Expect(guard->Hold(0, staging), "the staging buffer is held by idle slot 0");
    ImageBarrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue magenta{{1.0f, 0.0f, 1.0f, 1.0f}};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, display, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &all);
    ImageBarrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {64, 64, 0};
    copy.imageExtent = {32, 32, 1};
    vkCmdCopyBufferToImage(c, staging.buffer, display, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    EndWithReadback(c);
    const uint64_t destroyedBefore = transientsDestroyed;
    Require(guard->Submit(0));
    // Observed while in flight, judged after the retire: a failing Expect stops, and a stop with this
    // thread's own submission in flight would not wait for it (SubmissionGate::AwaitIdle OwnThread).
    const bool heldInFlight = guard->Held(0) == 1 && transientsDestroyed == destroyedBefore;
    Require(guard->Retire(0));
    Expect(heldInFlight, "in flight: the staging buffer is held, not destroyed");
    Expect(guard->Held(0) == 0 && transientsDestroyed == destroyedBefore + 1, "confirmed: the staging buffer destroyed after the fence");
    const uint32_t* px = static_cast<const uint32_t*>(readback.mapped);
    Expect(px[0] == 0xFFF003FFu && px[size_t(719) * kDisplayWidth + 1279] == 0xFFF003FFu, "clear read back as 0xFFF003FF");
    Expect(px[size_t(64) * kDisplayWidth + 64] == (3u << 30 | 0x155u << 20) &&
               px[size_t(64 + 31) * kDisplayWidth + 64 + 31] == (3u << 30 | 0x155u << 20 | 992u << 10 | 992u) &&
               px[size_t(63) * kDisplayWidth + 64] == 0xFFF003FFu,
           "upload read back bit for bit, the clear around it");
    Capture(0, "self-check clear");

    // 2. The bounded fence wait on the real driver, on a scratch gate, guard and flag: a fence that is
    //    created and never submitted (valid Vulkan: waiting on it simply times out) behind a test seam
    //    whose Submit submits nothing. The minimum limit (100 ms) must time out, strand, keep the
    //    held resource and write the scratch GPU_STOP.
    {
        VkFence never = VK_NULL_HANDLE;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        Check(vkCreateFence(dev.device, &fi, nullptr, &never), "vkCreateFence");
        struct NeverSubmitted final : SubmissionBackend<int>
        {
            VkFence fence;
            int destroyed = 0, submits = 0;
            int Submit(uint32_t) override { return submits++, 0; }  // the seam: nothing reaches the queue
            int WaitFence(uint32_t, uint64_t ns) override { return int(vkWaitForFences(dev.device, 1, &fence, VK_TRUE, ns)); }
            int FenceStatus(uint32_t) override { return int(vkGetFenceStatus(dev.device, fence)); }
            void Destroy(int&) override { destroyed++; }
        } seam;
        seam.fence = never;
        gpu::SubmissionGate scratch;
        const std::string flag = dir + "/selfcheck_stranded_GPU_STOP";
        GpuGuard<int, 1> g(seam, scratch, flag, gpu::FenceTimeoutNs(gpu::kFenceTimeoutMinMs));
        Expect(g.Hold(0, 7) && g.Submit(0).ok && seam.submits == 1, "scratch: held and 'submitted' through the seam");
        const uint64_t t0 = NowNs();
        const Verdict r = g.Retire(0);
        const uint64_t waited = NowNs() - t0;
        std::fprintf(stderr, "[gpu self-check] scratch retire: %s (waited %llu ms)\n", r.reason.c_str(), (unsigned long long)(waited / 1000000));
        Expect(!r.ok && r.stranded && waited >= 100000000ull && waited < 2000000000ull, "a never-signalled fence times out within the bound and strands");
        Expect(seam.destroyed == 0 && g.Held(0) == 1 && scratch.Load().stranded, "stranded: the resource is kept and the gate refuses");
        Expect(gpu::CheckGpuStop(flag.c_str()).stopped, "stranded: GPU_STOP written (scratch path)");
        Expect(!g.Submit(0).ok && seam.submits == 1, "after the stranding nothing more is submitted");
        Expect(g.Shutdown() == 1 && seam.destroyed == 0, "shutdown keeps the stranded resource");
        vkDestroyFence(dev.device, never, nullptr);  // never submitted: not in use
    }

    // 3. GPU_STOP honoured in-process: a planted scratch flag refuses before the backend is reached.
    {
        const std::string flag = dir + "/selfcheck_planted_GPU_STOP";
        std::string error;
        Expect(WriteGpuStop(flag.c_str(), "planted by the self-check", error), "scratch GPU_STOP planted");
        struct Counting final : SubmissionBackend<int>
        {
            int calls = 0;
            int Submit(uint32_t) override { return calls++, 0; }
            int WaitFence(uint32_t, uint64_t) override { return calls++, 0; }
            int FenceStatus(uint32_t) override { return calls++, 0; }
            void Destroy(int&) override { calls++; }
        } counting;
        gpu::SubmissionGate scratch;
        GpuGuard<int, 1> g(counting, scratch, flag, gpu::FenceTimeoutNs(gpu::kFenceTimeoutMinMs));
        const Verdict v = g.Submit(0);
        Expect(!v.ok && v.reason.find("planted by the self-check") != std::string::npos && counting.calls == 0 && scratch.Load().inFlight == 0,
               "a planted GPU_STOP refuses the submission, naming its reason; the backend is not called");
    }

    // 4. The loop-work cap (2^26): two loops of 64 (low bytes of 0x01020340 and 0x00000040) are
    //    4096 = 2^12 per invocation, so 2^14 = 16384 pixel invocations reach 2^26 exactly (allowed)
    //    and 16385 exceed it (refused).
    {
        const uint32_t loops[] = {0x01020340u, 0x00000040u};
        using G = GpuGuard<Transient, kSlots>;
        Expect(G::DrawLoopWork({}, 1u << 30, loops, 16384).ok, "loop work 2^26 exactly is allowed (a stage without loops is unbounded)");
        Expect(!G::DrawLoopWork({}, 0, loops, 16385).ok, "loop work 2^26 + 2^12 is refused");
    }

    // 5. The capped capture ring with real files: two records at most, so the third capture deletes
    //    the first; a capture larger than the byte cap is refused and nothing is written.
    {
        const std::vector<uint8_t> tiny = EncodePngRgb8(1, 1, {0, 0, 0});
        CaptureStore<2> ring(dir, "selfcheck_ring_", tiny.size() * 2);
        auto write = [&](const std::string& path) {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (f == nullptr)
                return false;
            const bool ok = std::fwrite(tiny.data(), 1, tiny.size(), f) == tiny.size();
            return std::fclose(f) == 0 && ok;
        };
        std::string error;
        for (uint64_t id = 1; id <= 3; id++)
            Expect(ring.Add(id, tiny.size(), write, error), "a tiny capture is accepted");
        Expect(::access(ring.PathOf(1).c_str(), F_OK) != 0 && ::access(ring.PathOf(2).c_str(), F_OK) == 0 &&
                   ::access(ring.PathOf(3).c_str(), F_OK) == 0,
               "the oldest capture's file was deleted on eviction, the newest two kept");
        CaptureStore<2> small(dir, "selfcheck_toolarge_", tiny.size() - 1);
        Expect(!small.Add(9, tiny.size(), write, error) && ::access(small.PathOf(9).c_str(), F_OK) != 0, "an oversized capture is refused, no file");
    }

    Shutdown();
    Stop(kStopGpuSelfCheck, "GPU self-check passed: device, clear and upload read back, deferred destruction, bounded fence wait (stranding "
                            "on a scratch flag), GPU_STOP refusal, loop-work cap, capped captures; %llu validation message(s)",
         (unsigned long long)ValidationMessageCount());
}

}  // namespace runtime::video
