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
//
// The draw path (unit 8, Draw below) records every draw since the previous swap into the slot the swap
// then submits (one submission per frame in unit 4's single slot; unit 5 adds slots and submissions
// within a frame). Per draw, in this order and before anything is recorded: the gate's in-memory
// state; DecodeDrawState (draw_state.h, fail closed); a resolve to the EDRAM hooks, a NoWrite draw to
// the one policy switch (draw_record.h kNoWriteDraws); the host indices; GpuGuard::DrawLoopWork; the
// shader variants and the pipeline key (draw_record.h); PipelineCache::Get, whose first step is the
// shader-class allow-list (a never-seen class runs first in its own approved run); the
// render target from the EDRAM hooks; the uploads into the slot's ring; the descriptor set from the
// slot's pool; then one render pass with one draw. The ring and the pool are reset exactly in
// OpenSlot, after the slot's previous submission was confirmed complete (RetireSlot) and before the
// next one records anything; the xe_error dwords of the slot's draws are read in RetireSlot, after the
// confirmed completion and before that reset.
#include "renderer.h"
#include "capture_store.h"
#include "draw_record.h"
#include "draw_state.h"
#include "edram_vk.h"
#include "front_buffer.h"
#include "gpu_guard.h"
#include "guest_writes.h"
#include "pipeline_cache.h"
#include "png.h"
#include "shader_cache.h"
#include "shader_class.h"
#include "texture_vk.h"
#include "vulkan_device.h"
#include "kernel/graphics.h"
#include "kernel/virtual_memory.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

// Added to stops.h / stops.cpp by the stop drain.
void SetStopQuiesce(void (*quiesce)(), void (*signalReport)());

namespace runtime::video {
namespace {

constexpr uint32_t kDisplayWidth = 1280, kDisplayHeight = 720;  // the accepted video mode (imports_video.cpp)
constexpr uint32_t kSlots = 1;                                     // unit 4; D-b's 12 slots come with unit 5
constexpr size_t kCaptureRecords = 16;
constexpr uint64_t kCaptureBytes = 64ull << 20;                    // 16 stored-deflate 1280x720 PNGs are ~44 MB
constexpr uint64_t kRingBytes = 16ull << 20;                       // per slot (D-b): ~9.7 KB of blocks per draw plus its vertex windows
constexpr uint32_t kDrawsPerSlot = 4096;                           // descriptor sets per slot pool (a race frame is ~3,000 draws, L §3.4); a set
                                                                   // costs every binding of the layout (32 samplers), written or not

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
    // Unit 8: the slot's persistent upload ring (uniform, storage and index data of its draws), its
    // descriptor pool, and the xe_error dword of each draw recorded into the open submission.
    Transient ring;
    std::optional<UploadRing> allocator;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    bool open = false;  // recording (OpenSlot) and not yet submitted
    std::vector<std::pair<uint64_t, uint64_t>> errorDwords;  // (ring offset, draw number)
} slots[kSlots];
VkImage display = VK_NULL_HANDLE;
VkDeviceMemory displayMemory = VK_NULL_HANDLE;
Transient readback;  // persistent: read on the CPU only after the slot's completion is confirmed
uint64_t transientsDestroyed = 0, drawsSinceSwap = 0, shadersSinceSwap = 0;
uint64_t noWriteSinceSwap = 0, resolvesSinceSwap = 0, drawNumber = 0;
gpu::SubmissionGate gate;

// Unit 8.
std::optional<GlslangCompiler> compiler;
std::optional<ShaderCache> shaders;
std::optional<PipelineCache> pipelines;
ShaderClass allowedClasses = kClassStraight;
const EdramHooks* edramHooks = nullptr;
const TextureHooks* textures = nullptr;
struct FramebufferKey
{
    VkRenderPass pass;
    VkImageView color, depth;
    uint32_t width, height;
    bool operator==(const FramebufferKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
    struct Hasher
    {
        size_t operator()(const FramebufferKey& k) const
        {
            const auto* b = reinterpret_cast<const unsigned char*>(&k);
            uint64_t h = 0xcbf29ce484222325ull;
            for (size_t i = 0; i < sizeof k; i++)
                h = (h ^ b[i]) * 0x100000001b3ull;
            return size_t(h);
        }
    };
};
std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKey::Hasher> framebuffers;

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
        Stop(v.stranded ? StopCode::GpuStranded : StopCode::Setup, "renderer: %s", v.reason.c_str());
}

// A refused draw-time result: the host cannot render this draw faithfully (unimplemented), named by its number.
void RequireDraw(uint64_t draw, const KeyResult& r)
{
    if (!r.ok)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, r.reason.c_str());
}
void RequireDraw(uint64_t draw, const Verdict& v)
{
    if (!v.ok)
        Stop(v.stranded ? StopCode::GpuStranded : StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, v.reason.c_str());
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

// The slot's open command buffer (draws and the swap record into the same submission). The first call
// after the slot's previous submission was retired opens it: this is the one reset point of the slot's
// upload ring and descriptor pool (pipeline_key.h UploadRing: never while the slot is in flight; the
// idle check is BeginSlot's).
VkCommandBuffer OpenSlot(uint32_t s)
{
    Slot& slot = slots[s];
    if (slot.open)
        return slot.commands;
    const VkCommandBuffer c = BeginSlot(s);
    slot.allocator->Reset();
    Check(vkResetDescriptorPool(dev.device, slot.descriptors, 0), "vkResetDescriptorPool");
    slot.errorDwords.clear();
    slot.open = true;
    return c;
}

// The slot's submission, confirmed complete (bounded wait), then the xe_error dword of each of its draws
// (shader_glsl.h binding 5: bit 0 a vertex fetch outside its slot's data, bit 1 a half float with
// exponent 31): a set bit stops naming the draw. Read before the next OpenSlot resets the ring.
void RetireSlot(uint32_t s)
{
    Require(guard->Retire(s));
    // The EDRAM unit's read-back resolves of this submission into guest memory (render_hooks.h EdramHooks::retired),
    // before the pending ranges are declared complete.
    if (edramHooks != nullptr)
        if (std::string error; !edramHooks->retired(error))
            Stop(StopCode::Unimplemented, "renderer: resolve read-back: %s", error.c_str());
    GuestWritesRetired();  // the resolves recorded into it wrote guest memory (guest_writes.h; unit 4's one slot)
    Slot& slot = slots[s];
    slot.open = false;
    for (const auto& [offset, draw] : slot.errorDwords)
    {
        uint32_t error = 0;
        std::memcpy(&error, static_cast<const uint8_t*>(slot.ring.mapped) + offset, 4);
        if (error != 0)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: the vertex shader flagged xe_error %u (bit 0: a vertex fetch outside its slot's data; "
                                          "bit 1: a 16-bit float with exponent 31)", (unsigned long long)draw, error);
    }
    slot.errorDwords.clear();
}

// The open slot's recorded work, submitted and confirmed complete (a bounded wait), so that guest memory
// holds what it wrote (resolves) before the CPU reads it. Nothing to do when the slot is not open.
void FlushSlot(uint32_t s)
{
    if (!slots[s].open)
        return;
    Check(vkEndCommandBuffer(slots[s].commands), "vkEndCommandBuffer");
    Require(guard->Submit(s));
    RetireSlot(s);  // confirmed completion, then the draws' xe_error dwords
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
    if (png.empty())  // the encoder refused its input (png.h: a size mismatch or an empty image); never a 0-byte capture
        Stop(StopCode::Setup, "renderer: capture %llu: the PNG encoder refused %zu RGB bytes for %ux%u", (unsigned long long)id, rgb.size(), kDisplayWidth, kDisplayHeight);
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

// The display gamma ramp, applied on the CPU to the detiled front buffer before the upload, so the display image and the
// capture show what the TV would. Xenia applies the PWL ramp to a 2_10_10_10 front buffer at every swap, the guest's
// ramp or its default (vulkan/vulkan_command_processor.cc IssueSwap, use_pwl_gamma_ramp; shaders/apply_gamma_pwl.xesli;
// kernel/gamma_ramp.h), assuming an increment of 8 (apply_gamma_pwl.xesli:78-80, DC_LUTA_CONTROL 3; 0 before the guest
// writes it, register_table.inc:470-478): any other DC_LUTA_CONTROL stops. The ramp is logged when it changes.
uint64_t loggedRampDigest = 0;
bool rampLogged = false;
void ApplyDisplayGamma(const uint8_t* base, uint32_t* pixels, size_t count, uint64_t swap)
{
    namespace gamma = runtime::gpu::gamma;
    const uint32_t control = runtime::gpu::ReadRegister(base, gamma::reg::kLutaControl);
    if (control != 0 && control != 3)
        Stop(StopCode::Unimplemented, "renderer: swap %llu: DC_LUTA_CONTROL %08X (only 0 or 3, the PWL increment 8 Xenia assumes)",
             (unsigned long long)swap, control);
    const runtime::gpu::GammaRampView v = runtime::gpu::CurrentGammaRamp();
    const uint64_t digest = gamma::PwlDigest(v.ramp.pwl);
    if (!rampLogged || digest != loggedRampDigest)
    {
        static const uint64_t defaultDigest = gamma::PwlDigest(gamma::DefaultRamp().pwl);
        const char* kind = gamma::IsD3dLinearPwl(v.ramp.pwl) ? "Direct3D 9 linear" : digest == defaultDigest ? "Xenia's default" : "title";
        const gamma::PwlEntry* r = &v.ramp.pwl[0][0];
        std::fprintf(stderr, "[gpu] display gamma ramp: PWL, 128 entries, digest %016llX (%s), from swap %llu; DC_LUTA_CONTROL %08X; "
                     "red base/delta at 00 %04X/%04X, 01 %04X/%04X, 40 %04X/%04X, 7E %04X/%04X, 7F %04X/%04X; ramp-state writes: %llu "
                     "by the command processor, %llu by CPU stores\n",(unsigned long long)digest, kind, (unsigned long long)swap, control,
                     r[0].base, r[0].delta, r[3].base, r[3].delta, r[0x40 * 3].base, r[0x40 * 3].delta, r[0x7E * 3].base, r[0x7E * 3].delta,
                     r[0x7F * 3].base, r[0x7F * 3].delta, (unsigned long long)v.cpWrites, (unsigned long long)v.cpuWrites);
        loggedRampDigest = digest, rampLogged = true;
    }
    gamma::ApplyPwlToFrame(pixels, count, v.ramp.pwl);
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
    // The ring alignments are the device's (never assumed; MoltenVK reports Metal's).
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(dev.gpu, &properties);
    const uint64_t uniformAlign = properties.limits.minUniformBufferOffsetAlignment, storageAlign = properties.limits.minStorageBufferOffsetAlignment;
    if (uniformAlign == 0 || (uniformAlign & (uniformAlign - 1)) || storageAlign == 0 || (storageAlign & (storageAlign - 1)))
        Stop(StopCode::Setup, "renderer: buffer offset alignments %llu / %llu are not powers of two", (unsigned long long)uniformAlign,
             (unsigned long long)storageAlign);
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
        // Unit 8: the ring (uniform, storage and index data) and the descriptor pool, persistent per slot.
        // Unit 7: also the staging source of texture uploads (vkCmdCopyBufferToImage, texture_vk.cpp).
        s.ring = MakeBuffer(kRingBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        s.allocator.emplace(kRingBytes, uniformAlign, storageAlign);
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * kDrawsPerSlot},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kDrawsPerSlot},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTextureBindings * kDrawsPerSlot},  // the layout's 16 sampler bindings per set
        };
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = kDrawsPerSlot;
        dp.poolSizeCount = uint32_t(std::size(sizes));
        dp.pPoolSizes = sizes;
        Check(vkCreateDescriptorPool(dev.device, &dp, nullptr, &s.descriptors), "vkCreateDescriptorPool");
    }
}

// Device teardown, only when nothing's completion is unknown. A kept resource keeps the device too
// (destroying it would free memory the GPU may still use): the run stops with it reported.
void Shutdown()
{
    if (const size_t kept = guard->Shutdown(); kept != 0)
        Stop(StopCode::GpuStranded, "renderer: %zu resource(s) kept at shutdown (completion unknown); the device is not destroyed", kept);
    for (auto& [key, fb] : framebuffers)  // before the render passes and the EDRAM unit's views they reference
        vkDestroyFramebuffer(dev.device, fb, nullptr);
    framebuffers.clear();
    if (edramHooks != nullptr)  // its images, views and pipelines: after the framebuffers over its views, before the device
        edramHooks->shutdown();
    edramHooks = nullptr;
    pipelines.reset();  // pipelines, render passes, modules, layouts
    shaders.reset();
    compiler.reset();
    textures = nullptr;
    ShutdownTextureUnit();  // views, samplers, images: every submission that used them is confirmed complete (above)
    for (Slot& s : slots)
    {
        vkDestroyDescriptorPool(dev.device, s.descriptors, nullptr);
        backend.Destroy(s.ring);
        vkDestroyFence(dev.device, s.fence, nullptr);
        vkDestroyCommandPool(dev.device, s.pool, nullptr);  // frees its command buffer
        s.allocator.reset();
        s.descriptors = VK_NULL_HANDLE, s.fence = VK_NULL_HANDLE, s.commands = VK_NULL_HANDLE, s.pool = VK_NULL_HANDLE;
        s.open = false;
        s.errorDwords.clear();
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

// ---- Unit 8 helpers -------------------------------------------------------------------------------

uint32_t ReadRegisterHook(const void* context, uint32_t index) { return runtime::gpu::ReadRegister(static_cast<const uint8_t*>(context), index); }

// A guest index buffer element as the GPU loads it (little-endian, before the DMA endian swap in
// draw_record.cpp SwapIndex). The buffer's range was checked against physical memory by DrawState; the
// view that holds it is found per element (PhysicalToGuest).
struct IndexSource
{
    const uint8_t* base;
    const IndexBuffer* indices;
    uint64_t draw;
};
uint32_t ReadGuestIndex(const void* context, uint32_t i)
{
    const IndexSource& s = *static_cast<const IndexSource*>(context);
    const uint32_t size = s.indices->is32Bit ? 4 : 2, physical = s.indices->physical + i * size;
    uint32_t guest = 0;
    if (!runtime::PhysicalToGuest(physical, guest))
        Stop(StopCode::Unimplemented, "renderer: draw %llu: index %u at physical %08X is in no view", (unsigned long long)s.draw, i, physical);
    uint32_t raw = 0;
    std::memcpy(&raw, s.base + guest, size);  // the GPU's little-endian element load
    return raw;
}

// The framebuffer of a render pass over the EDRAM unit's views (one per distinct set for the run; the
// views live for the run, render_hooks.h).
VkFramebuffer Framebuffer(VkRenderPass pass, const HostTargets& t, bool color, bool depth)
{
    const FramebufferKey key{pass, color ? t.color : VK_NULL_HANDLE, depth ? t.depth : VK_NULL_HANDLE, t.width, t.height};
    if (const auto it = framebuffers.find(key); it != framebuffers.end())
        return it->second;
    VkImageView views[2];
    uint32_t count = 0;
    if (color)
        views[count++] = t.color;
    if (depth)
        views[count++] = t.depth;
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = count;
    info.pAttachments = views;
    info.width = t.width;
    info.height = t.height;
    info.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    Check(vkCreateFramebuffer(dev.device, &info, nullptr, &fb), "vkCreateFramebuffer");
    framebuffers[key] = fb;
    return fb;
}

// One block of the draw in the slot's ring; exhaustion stops (fail closed: no wrap, no silent drop).
uint64_t Allocate(Slot& s, uint64_t draw, uint64_t bytes, bool uniform, const char* what)
{
    uint64_t offset = 0;
    if (!s.allocator->Allocate(bytes, uniform, offset))
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the slot's upload ring (%llu bytes) cannot hold %llu more bytes for %s after %llu allocations "
                                      "(unit 5 submits within a frame)", (unsigned long long)draw, (unsigned long long)kRingBytes, (unsigned long long)bytes,
             what, (unsigned long long)s.allocator->Allocations());
    return offset;
}

}  // namespace

gpu::SubmissionGate& Submissions() { return gate; }
bool Enabled() { return enabled; }
void SetEdramHooks(const EdramHooks* hooks) { edramHooks = hooks; }
void SetTextureHooks(const TextureHooks* hooks) { textures = hooks; }

bool Initialize(const Options& o)
{
    options = o;
    if (o.mode == Mode::Headless)
        return false;
    if (o.gpuStopPath == nullptr || o.captureDir == nullptr)
        Stop(StopCode::Setup, "renderer: --gpu-stop and --gpu-capture are required with a GPU renderer mode");
    std::string classError;
    if (!ParseShaderClassAllowList(o.shaderClasses, allowedClasses, classError))  // required, no silent default (shader_class.h)
        Stop(StopCode::Setup, "renderer: %s", classError.c_str());
    const gpu::FenceTimeout timeout = gpu::FenceTimeoutFromEnv(o.fenceTimeoutEnv);
    if (timeout.malformed)  // fail closed: a mistyped limit is not silently replaced
        Stop(StopCode::Setup, "renderer: FM4_GPU_FENCE_TIMEOUT_MS=%s is not a decimal number of milliseconds", o.fenceTimeoutEnv);
    guard.emplace(backend, gate, o.gpuStopPath, gpu::FenceTimeoutNs(timeout.ms));
    Require(guard->Begin("renderer initialize"));  // GPU_STOP before any Vulkan call
    captures.emplace(o.captureDir, "frame_", kCaptureBytes);
    SetStopQuiesce(QuiesceForStop, ReportForSignalStop);
    std::fprintf(stderr, "[gpu] renderer %s: fence timeout %u ms, captures %s (at most %zu files, %llu MB), GPU_STOP %s, shader classes allowed: %s\n",
                 o.mode == Mode::SelfCheck ? "self-check" : "present", timeout.ms, o.captureDir, kCaptureRecords,
                 (unsigned long long)(kCaptureBytes >> 20), o.gpuStopPath, allowedClasses == kClassStraight ? "none (straight-line only)" : ShaderClassName(allowedClasses).c_str());
    dev = CreateVulkanDevice(o.validation);
    CreateResources();
    // Unit 8: the shader and pipeline caches. The portability-subset features are not exposed by
    // vulkan_device.cpp yet: constant-alpha colour blend factors stay refused (conservative; FM4's first
    // screens blend with src alpha / inv src alpha only).
    compiler.emplace();
    shaders.emplace(*compiler, o.shaderDumpDir);
    // The features enabled at vkCreateDevice (vulkan_device.cpp), never merely the supported ones.
    PipelineDeviceFeatures features;
    features.depthClamp = dev.depthClamp;
    features.constantAlphaColorBlendFactors = false;
    features.independentBlend = dev.independentBlend;
    KeyResult created;
    pipelines.emplace(dev.device, features, allowedClasses, created);
    if (!created)
        Stop(StopCode::Setup, "renderer: %s", created.reason.c_str());
    SetTextureHooks(InitializeTextureUnit(dev));  // unit 7 (texture_vk.h)
    // The EDRAM unit (edram_vk.h): its transfer shaders through the same compiler, its resolve writes through main.cpp's
    // writable guest base, each recorded resolve reported to the texture unit (guest_writes.h MarkGuestWritten). Present
    // mode only: the self-check draws nothing and keeps its device work unchanged.
    if (o.mode == Mode::Present)
    {
        if (o.guestBase == nullptr)
            Stop(StopCode::Setup, "renderer: present mode needs the writable guest base (Options::guestBase)");
        std::string edramError;
        const EdramHooks* hooks = InitializeEdramVk(dev, *compiler, o.guestBase, MarkGuestWritten, edramError);
        if (hooks == nullptr)
            Stop(StopCode::Setup, "renderer: EDRAM unit: %s", edramError.c_str());
        SetEdramHooks(hooks);
    }
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
    // The frame's recorded work (draws, and the resolves that write guest memory, render_hooks.h) must
    // complete before the front buffer is read: submit and retire the open slot first (FlushSlot; the
    // resolves already flushed their own work). Bounded waits only, in unit 4's one slot.
    FlushSlot(0);
    // A plain CPU read while the title may still write the buffer: the guarded read
    // (write watch, external host memory) is unit 7.
    Transient staging = MakeBuffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    uint32_t* linear = static_cast<uint32_t*>(staging.mapped);
    if (!Detile32(base + guest, TiledSize32(f.pitch, height), linear, width, height, f.pitch))
        Stop(StopCode::Unimplemented, "renderer: swap %llu: front buffer shorter than its tiled size", (unsigned long long)swap);
    if (f.swapRedBlue)
        for (size_t i = 0; i < size_t(width) * height; i++)
            linear[i] = SwapRedBlue1010102(linear[i]);
    ApplyDisplayGamma(base, linear, size_t(width) * height, swap);
    const VkCommandBuffer c = OpenSlot(0);  // a fresh recording: the frame's draws completed above
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
    RetireSlot(0);  // one bounded wait per swap: the one-frame throttle (execution model §2); then the draws' xe_error dwords
    Capture(swap, "front buffer");
    std::fprintf(stderr, "[gpu] swap %llu presented offscreen; since the last swap: %llu draw(s) rendered, %llu no-write draw(s) %s, %llu resolve(s), "
                         "%llu shader load(s); %llu bytes of the slot's ring used; %s\n",
                 (unsigned long long)swap, (unsigned long long)drawsSinceSwap, (unsigned long long)noWriteSinceSwap,
                 kNoWriteDraws == NoWriteDraws::Skip ? "recorded as nothing (draw_record.h kNoWriteDraws)" : "(none allowed)",
                 (unsigned long long)resolvesSinceSwap, (unsigned long long)shadersSinceSwap, (unsigned long long)slots[0].allocator->Used(),
                 TextureUnitSummary().c_str());
    drawsSinceSwap = shadersSinceSwap = noWriteSinceSwap = resolvesSinceSwap = 0;
}

void Draw(const uint8_t* base, uint32_t opcode, uint32_t vizQueryToken)
{
    if (!enabled)
        return;
    const uint64_t draw = drawNumber++;
    // 1. The gate's in-memory state (no file access per draw; GPU_STOP is checked at every Submit, the
    //    single choke point to the queue, execution model §4).
    const gpu::SubmissionGate::View v = gate.Load();
    if (v.stopping || v.stranded)
        Stop(v.stranded ? StopCode::GpuStranded : StopCode::Setup, "renderer: draw %llu after the GPU stopped", (unsigned long long)draw);
    // 2. The draw state from the register file and the current shaders (fail closed).
    const CachedShader* vs = shaders->Current(false);
    const CachedShader* ps = shaders->Current(true);
    if (vs == nullptr || ps == nullptr)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: no %s shader was loaded (IM_LOAD) before it", (unsigned long long)draw, vs ? "pixel" : "vertex");
    DrawInputs in;
    in.read = ReadRegisterHook;
    in.context = base;
    in.opcode = opcode;
    in.vizQueryToken = vizQueryToken;
    in.physicalBytes = runtime::kPhysicalSize;
    in.vs = &vs->t;
    in.ps = &ps->t;
    const DrawState d = DecodeDrawState(in);
    std::string error;
    switch (d.kind)
    {
    case DrawKind::Refused:
        Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, d.error.c_str());
    case DrawKind::Resolve:  // RB_MODECONTROL 6: the EDRAM unit's (render_hooks.h)
        if (edramHooks == nullptr)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: a resolve (RB_MODECONTROL 6) without an EDRAM unit", (unsigned long long)draw);
        if (!edramHooks->resolve(OpenSlot(0), base, d, draw, error))
            Stop(StopCode::Unimplemented, "renderer: draw %llu: resolve: %s", (unsigned long long)draw, error.c_str());
        resolvesSinceSwap++;
        // Every resolve completes before the next draw records: that draw's texture loads read guest memory on
        // the CPU (trace11 samples the video resolve's destination in the same frame). Unconditional, so the
        // order does not depend on the EDRAM unit calling MarkGuestWritten (which only moves the watch's
        // sequences and arms texture_upload.h's pending refusal). Unit 4: one bounded wait per resolve.
        FlushSlot(0);
        return;
    case DrawKind::NoWrite:  // the one policy switch (draw_record.h kNoWriteDraws; decision pending)
        if (kNoWriteDraws == NoWriteDraws::Stop)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: %s of %u indices writes neither colour nor depth nor stencil (kNoWriteDraws = Stop)",
                 (unsigned long long)draw, PrimitiveTypeName(d.primitiveType), d.indices.count);
        noWriteSinceSwap++;
        return;
    case DrawKind::Draw: case DrawKind::DepthOnly:
        break;
    }
    // 3. The host indices, then the loop-work cap (vertex invocations = what the host draws, pixel
    //    invocations = the scissor's area), before anything is recorded.
    HostIndices indices;
    const IndexSource source{base, &d.indices, draw};
    RequireDraw(draw, BuildHostIndices(d, d.indices.indexed ? ReadGuestIndex : nullptr, &source, indices));
    RequireDraw(draw, GpuGuard<Transient, kSlots>::DrawLoopWork(d.vsLoopRegisters, indices.vertexCount, d.psLoopRegisters, d.pixelInvocations));
    // 4. The shader variants, the pipeline key and the descriptor plan (CPU).
    ShaderVariantDefines vsDefines, psDefines;
    RequireDraw(draw, VariantsFromDrawState(d, *vs, *ps, vsDefines, psDefines));
    ShaderVariant* vsVariant = shaders->Variant(*vs, vsDefines, error);
    if (vsVariant == nullptr)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, error.c_str());
    ShaderVariant* psVariant = shaders->Variant(*ps, psDefines, error);
    if (psVariant == nullptr)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, error.c_str());
    PipelineKey key;
    RequireDraw(draw, KeyFromDrawState(d, *vs, *ps, vsVariant->id, psVariant->id, key));
    DescriptorPlan plan;
    RequireDraw(draw, PlanDescriptors(*vs, *ps, plan));
    // 5. The pipeline: the shader-class allow-list is PipelineCache::Get's first step, before any Vulkan call.
    PipelineCache::Entry pipeline;
    RequireDraw(draw, pipelines->Get(key, *vs, *vsVariant, *ps, *psVariant, pipeline));
    // 6. The render target (the EDRAM unit's images) and its framebuffer.
    if (edramHooks == nullptr)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: a draw into a render target without an EDRAM unit", (unsigned long long)draw);
    Slot& slot = slots[0];
    // The vertex windows (their guest bytes are copied into the ring at step 7). One window must sit in one view.
    std::vector<VertexWindow> windows;
    uint32_t vertexDwords = 0;
    PlanVertexWindows(d, indices.highIndex, windows, vertexDwords);
    const auto windowBytes = [&](const VertexWindow& w) -> const uint8_t* {
        uint32_t guest = 0, guestEnd = 0;
        if (!runtime::PhysicalToGuest(w.physical, guest) || !runtime::PhysicalToGuest(w.physical + w.dwords * 4 - 1, guestEnd) || guestEnd - guest != w.dwords * 4 - 1)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex fetch slot %u (%08X, %u dwords) is not in one view", (unsigned long long)draw, w.slot,
                 w.physical, w.dwords);
        return base + guest;
    };
    // FM4 Recomp: the rows the bind may claim (X render_target_cache.cc:757-767, EstimateMaxY).
    // A draw whose extent needs its vertices (clipping off, 8192 scissor: FM4's fast clears) is bounded by its exported
    // positions when its vertex shader's position is a plain vertex fetch (draw_record.h BoundTargetHeight); otherwise
    // the scissor bound stands and a bind refusal names why the vertices were not used.
    RenderTarget target = d.target;
    std::string extentNote;
    if (d.extentNeedsVertices)
    {
        const VertexWindow* positionWindow = nullptr;
        for (const VertexWindow& w : windows)
            if (vs->t.positionFetch.valid && w.slot == vs->t.positionFetch.slot && w.dwords != 0)
                positionWindow = &w;
        std::vector<uint32_t> loads;
        if (positionWindow != nullptr)
        {
            loads.resize(positionWindow->dwords);
            std::memcpy(loads.data(), windowBytes(*positionWindow), loads.size() * 4);  // the GPU's 32-bit loads (little-endian host)
        }
        BoundTargetHeight(d, vs->t, positionWindow ? loads.data() : nullptr, loads.size(), positionWindow ? positionWindow->endian : 0, indices,
                          target.targetHeight, extentNote);
    }
    const VkCommandBuffer c = OpenSlot(0);
    HostTargets targets;
    if (!edramHooks->acquireTargets(c, target, key.colorUsed != 0, key.depthUsed != 0, targets, error))
        Stop(StopCode::Unimplemented, "renderer: draw %llu: render target: %s%s%s", (unsigned long long)draw, error.c_str(), extentNote.empty() ? "" : " (",
             extentNote.empty() ? "" : (extentNote + ")").c_str());
    if (targets.samples != key.samples)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the EDRAM unit returned %u-sample images for a %u-sample target", (unsigned long long)draw,
             targets.samples, key.samples);
    if (key.colorUsed && (targets.color == VK_NULL_HANDLE || targets.format != HostColorFormat(key.color[0].format)))
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the EDRAM unit's colour view is missing or of format %d (expected %d)", (unsigned long long)draw,
             int(targets.format), int(HostColorFormat(key.color[0].format)));
    if (key.depthUsed && (targets.depth == VK_NULL_HANDLE || targets.depthFormat != HostDepthFormat(key.depthFormat)))
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the EDRAM unit's depth view is missing or of format %d (expected %d)", (unsigned long long)draw,
             int(targets.depthFormat), int(HostDepthFormat(key.depthFormat)));
    // The rows the draw can touch: the scissor's bottom within the surface's EDRAM rows (draw_state.h targetHeight), the
    // rows edram::Bind claimed (X render_target_cache.cc:757-767, :819-857). FM4's D3D fast clears have an 8192-row
    // scissor over a 360-row 4x surface whose host image is 1024 rows: the host scissor stops at
    // the claimed rows, never past the image.
    const uint32_t scissorBottom = std::max(target.targetHeight, d.scissor.y);
    if (targets.width < d.scissor.x + d.scissor.width || targets.height < scissorBottom || targets.width == 0 || targets.height == 0)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the %ux%u host target does not cover the scissor (%u, %u, %ux%u; rows to %u)",
             (unsigned long long)draw, targets.width, targets.height, d.scissor.x, d.scissor.y, d.scissor.width, d.scissor.height, scissorBottom);
    const VkFramebuffer framebuffer = Framebuffer(pipeline.renderPass, targets, key.colorUsed != 0, key.depthUsed != 0);
    // 7. The uploads into the slot's ring (pipeline_key.h D9); the windows were planned before the bind.
    const uint64_t systemAt = Allocate(slot, draw, kSystemBytes, true, "XeSystem");
    const uint64_t vsConstantsAt = Allocate(slot, draw, kConstantsBytes, true, "XeVsConstants");
    const uint64_t psConstantsAt = Allocate(slot, draw, kConstantsBytes, true, "XePsConstants");
    const uint64_t vertexBytes = std::max<uint64_t>(uint64_t(vertexDwords) * 4, 4);
    const uint64_t vertexAt = Allocate(slot, draw, vertexBytes, false, "vertex data");
    const uint64_t indexBytes = std::max<uint64_t>(indices.indices.size() * 4, 4);
    const uint64_t indexAt = Allocate(slot, draw, indexBytes, false, "indices");
    const uint64_t errorAt = Allocate(slot, draw, kErrorsBytes, false, "XeErrors");
    uint8_t* ring = static_cast<uint8_t*>(slot.ring.mapped);
    FillXeSystem(windows, FoldViewport(d.viewport, targets.width, targets.height), d.systemFlags, ring + systemAt);
    FillConstants(d, false, ring + vsConstantsAt);
    FillConstants(d, true, ring + psConstantsAt);
    for (const VertexWindow& w : windows)
    {
        if (w.dwords == 0)
            continue;
        // A plain CPU read of guest memory, as the present path (the guarded read is unit 7).
        std::memcpy(ring + vertexAt + uint64_t(w.baseDword) * 4, windowBytes(w), size_t(w.dwords) * 4);
    }
    if (!indices.indices.empty())
        std::memcpy(ring + indexAt, indices.indices.data(), indices.indices.size() * 4);
    std::memset(ring + errorAt, 0, kErrorsBytes);
    slot.errorDwords.emplace_back(errorAt, draw);
    // 8. The descriptor set: the six buffer bindings and the textures the pair samples.
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkDescriptorSetLayout layout = pipelines->SetLayout();
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = slot.descriptors;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &layout;
    if (const VkResult r = vkAllocateDescriptorSets(dev.device, &setInfo, &set); r != VK_SUCCESS)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: vkAllocateDescriptorSets failed (VkResult %d): the slot's pool (%u sets of %u bindings) is exhausted",
             (unsigned long long)draw, int(r), kDrawsPerSlot, 6 + kTextureBindings);
    const VkDescriptorBufferInfo buffers[6] = {
        {slot.ring.buffer, vertexAt, vertexBytes},         // 0 XeVertexData
        {slot.ring.buffer, systemAt, kSystemBytes},        // 1 XeSystem
        {slot.ring.buffer, vsConstantsAt, kConstantsBytes}, // 2 XeVsConstants
        {slot.ring.buffer, psConstantsAt, kConstantsBytes}, // 3 XePsConstants
        {slot.ring.buffer, indexAt, indexBytes},           // 4 XeIndices (read by the XE_RECT_LIST variant only)
        {slot.ring.buffer, errorAt, kErrorsBytes},         // 5 XeErrors
    };
    const VkDescriptorType bufferTypes[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> images;
    images.reserve(kTextureBindings);
    for (uint32_t b = 0; b < 6; b++)
    {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = b;
        w.descriptorCount = 1;
        w.descriptorType = bufferTypes[b];
        w.pBufferInfo = &buffers[b];
        writes.push_back(w);
    }
    for (uint32_t s = 0; s < kTextureBindings; s++)
    {
        if (plan.dimension[s] == 0)
            continue;
        if (textures == nullptr)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: the shaders sample texture slot %u but no texture unit is registered", (unsigned long long)draw, s);
        const TextureSlot* t = nullptr;
        for (const TextureSlot& candidate : d.textureSlots)
            if (candidate.slot == s)
                t = &candidate;
        if (t == nullptr)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: texture slot %u is sampled but was not decoded", (unsigned long long)draw, s);
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        const TextureBindContext context{c, base, slot.ring.buffer, static_cast<uint8_t*>(slot.ring.mapped), &*slot.allocator};
        if (!textures->bind(context, *t, view, sampler, error) || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: texture slot %u: %s", (unsigned long long)draw, s, error.empty() ? "no view or sampler" : error.c_str());
        images.push_back({sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = kBindingTexture0 + s;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images.back();
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(dev.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    // 9. One render pass, one draw. The render area is the whole target (every attachment loads and
    //    stores); the scissor is the guest's. Dynamic state in the pipeline's declared set.
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = pipeline.renderPass;
    pass.framebuffer = framebuffer;
    pass.renderArea = {{0, 0}, {targets.width, targets.height}};
    vkCmdBeginRenderPass(c, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->Layout(), 0, 1, &set, 0, nullptr);
    const HostViewport hv = FoldViewport(d.viewport, targets.width, targets.height);
    const VkViewport viewport{0.0f, 0.0f, float(targets.width), float(targets.height), hv.zMin, hv.zMax};
    vkCmdSetViewport(c, 0, 1, &viewport);
    const VkRect2D scissor{{int32_t(d.scissor.x), int32_t(d.scissor.y)}, {d.scissor.width, scissorBottom - d.scissor.y}};
    vkCmdSetScissor(c, 0, 1, &scissor);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, d.depthStencil.front.ref);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, d.depthStencil.back.ref);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, d.depthStencil.front.compareMask);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, d.depthStencil.back.compareMask);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, d.depthStencil.front.writeMask);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, d.depthStencil.back.writeMask);
    vkCmdSetBlendConstants(c, d.blend.constant);
    vkCmdSetDepthBias(c, 0.0f, 0.0f, 0.0f);  // the poly offset enables are refused by DrawState; the state is declared dynamic
    switch (indices.kind)
    {
    case HostIndices::Kind::Direct: case HostIndices::Kind::RectList:
        vkCmdDraw(c, indices.vertexCount, 1, 0, 0);
        break;
    case HostIndices::Kind::Indexed:
        vkCmdBindIndexBuffer(c, slot.ring.buffer, indexAt, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(c, indices.vertexCount, 1, 0, 0, 0);
        break;
    }
    vkCmdEndRenderPass(c);
    drawsSinceSwap++;
}

void LoadShader(uint32_t type, const uint32_t* words, uint32_t count)
{
    if (!enabled)
        return;
    shadersSinceSwap++;
    const uint64_t before = shaders->Stats().translations;
    std::string reason;
    const CachedShader* shader = shaders->Load(type, words, count, reason);
    if (shader == nullptr)
        Stop(StopCode::Unimplemented, "renderer: %s", reason.c_str());  // the hash is in the reason (shader_cache.cpp)
    if (shaders->Stats().translations != before)  // the first load of this hash: name its class for the allow-list decision
        std::fprintf(stderr, "[shader] %s_%016llx: %u words, class %s, %zu vertex fetch slot(s), %zu texture slot(s), %zu bool(s), %zu loop(s)\n",
                     shader->pixel ? "ps" : "vs", (unsigned long long)shader->hash, shader->words, ShaderClassName(shader->cls).c_str(),
                     shader->t.vertexFetchSlots.size(), shader->t.textureSlots.size(), shader->t.boolConstants.size(), shader->t.loopConstants.size());
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
        // CheckGpuStop names the flag's first line: the writer's provenance line ("GPU_STOP written <when> by
        // fm4recomp (in-process); only the owner clears it."), the reason being its second line (gpu_stop_write.h,
        // the same layout as scripts/gpu_run.sh writes).
        Expect(!v.ok && !v.stranded && v.reason.find("GPU_STOP written ") != std::string::npos &&
                   v.reason.find("by fm4recomp (in-process)") != std::string::npos && counting.calls == 0 && scratch.Load().inFlight == 0,
               "a planted GPU_STOP refuses the submission, naming the flag's first line; the backend is not called");
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
    Stop(StopCode::GpuSelfCheck, "GPU self-check passed: device, clear and upload read back, deferred destruction, bounded fence wait (stranding "
                            "on a scratch flag), GPU_STOP refusal, loop-work cap, capped captures; %llu validation message(s)",
         (unsigned long long)ValidationMessageCount());
}

}  // namespace runtime::video
