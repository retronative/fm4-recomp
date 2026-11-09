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
//
// Window mode (unit 4b): the same present path, then the display image is handed to the main thread's presenter
// (window_present.cpp) after the swap's submission retired and its capture was written; the next swap waits (bounded)
// only while the presenter is blitting from it. The one VkQueue is shared with the presenter: every submission here
// takes the queue lock first (bounded; uncontended in the other modes).
#include "renderer.h"
#include "capture_store.h"
#include "capture_keep.h"
#include "draw_record.h"
#include "draw_state.h"
#include "edram_vk.h"
#include "front_buffer.h"
#include "gpu_guard.h"
#include "guest_writes.h"
#include "pipeline_cache.h"
#include "png.h"
#include "diag_alpha.h"
#include "diag_depth.h"
#include "texture_dump.h"
#include "ps_trace.h"
#include "shader_cache.h"
#include "shader_class.h"
#include "texture_vk.h"
#include "vulkan_device.h"
#include "window_present.h"
#include "kernel/command_processor.h"
#include "kernel/input_script.h"
#include "kernel/thread_cpu.h"
#include "kernel/cp_trace.h"
#include "kernel/graphics.h"
#include "kernel/virtual_memory.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
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
constexpr size_t kKeepRecords = 96;                               // FM4_CAPTURE_KEEP_EVERY (capture_keep.h)
constexpr uint64_t kKeepBytes = 320ull << 20;                      // 96 x ~2.8 MB
constexpr uint32_t kMaxVertexWindowDwords = 1u << 20;               // 4 MiB per vertex slot: a window past its fetch constant (VertexWindowDwords) stops above it
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
    uint32_t sets = 0;  // FM4 Recomp: descriptor sets allocated from `descriptors` since OpenSlot (at most kDrawsPerSlot)
    // FM4 Recomp (FM4_GPU_TIMESTAMPS=1, diagnostic): a timestamp at the start and the end of the slot's submission.
    VkQueryPool timestamps = VK_NULL_HANDLE;
    bool timed = false, stamped = false;
    struct ErrorDword
    {
        uint64_t offset, draw;
        std::string what;  // FM4 Recomp: the draw's vertex shader and windows, named when its shader flags an error
    };
    std::vector<ErrorDword> errorDwords;
} slots[kSlots];
VkImage display = VK_NULL_HANDLE;
VkDeviceMemory displayMemory = VK_NULL_HANDLE;
Transient readback;  // persistent: read on the CPU only after the slot's completion is confirmed
uint64_t transientsDestroyed = 0, drawsSinceSwap = 0, shadersSinceSwap = 0;
uint64_t noWriteSinceSwap = 0, resolvesSinceSwap = 0, ringFlushesSinceSwap = 0, drawNumber = 0;
// FM4 Recomp: per-swap timing (always on, a few clock reads): each FlushSlot (submit, then the bounded wait for completion) by
// cause, and the texture binds (hash, untile, staging) on the CPU.
enum class FlushCause : uint32_t { Resolve, Ring, Swap, Consumer, Sync, NextResolve, Idle, Count };
constexpr const char* kFlushCauseNames[] = {"resolve", "ring", "swap", "resolve consumer", "CPU sync", "next resolve", "ring idle"};
static_assert(std::size(kFlushCauseNames) == size_t(FlushCause::Count));
uint64_t syncFlushesByPacketSinceSwap[128] = {};
// FM4 Recomp: what the draws ask of the slot's ring per swap (the ring check's `need`): texture staging, vertex windows, host
// indices, and the fixed system/constant/error blocks with their alignment slack (the timing line's "ring need").
uint64_t ringNeedSinceSwap[4] = {};  // "CPU sync" flushes per PM4 opcode (SyncGuestWrites)
uint64_t flushCountSinceSwap[size_t(FlushCause::Count)] = {}, flushNsSinceSwap[size_t(FlushCause::Count)] = {}, textureBindNsSinceSwap = 0;
std::chrono::steady_clock::time_point lastSwapEnd = std::chrono::steady_clock::now();
uint64_t NsSince(std::chrono::steady_clock::time_point t)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
}

// FM4 Recomp (opt-in FM4_RESOLVE_DEFER=1): a resolve does not flush the slot. Its write-back to guest
// memory lands when its submission retires: before a later draw reads a range it writes (index buffer, vertex windows, textures:
// one flush, "resolve consumer"), before a command-processor packet the guest CPU can synchronise on (SyncGuestWrites: event
// writes, memory and register write-backs, interrupts, waits: "CPU sync"), and at the swap. So the guest CPU still sees every
// resolve in guest memory by the time it can know the GPU finished it, as on the hardware; draws no longer wait after each
// resolve. Xenia does not read resolves back to the CPU at all by default (d3d12_command_processor.cc:42-47 d3d12_readback_resolve
// false; vulkan_command_processor.cc:2611 "TODO(Triang3l): CPU readback").
bool ResolveDefer()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_DEFER");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        std::fprintf(stderr, "[gpu] resolve deferral %s\n", enabled ? "on (FM4_RESOLVE_DEFER=1): a resolve's write-back waits for its first reader"
                                                                        : "off: every resolve flushes the slot");
        return enabled;
    }();
    return on;
}
}  // namespace

// FM4 Recomp (resolve_resident.h): opt-in, on top of FM4_RESOLVE_DEFER (without it every resolve flushes, so nothing is pending).
bool ResolveResident()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_RESIDENT");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && ResolveDefer();
        std::fprintf(stderr, "[gpu] resident resolves %s\n",
                     enabled ? "on (FM4_RESOLVE_RESIDENT=1): a converting resolve also writes the textures that take its words, on the GPU"
                     : asked ? "off: FM4_RESOLVE_RESIDENT=1 needs FM4_RESOLVE_DEFER=1 (ignored)"
                             : "off");
        return enabled;
    }();
    return on;
}

bool ResolveResidentCheck()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_RESIDENT_CHECK");
        const bool enabled = ResolveResident() && v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] resident resolve check on (FM4_RESOLVE_RESIDENT_CHECK=1): each converted resolve is compared with the CPU's at its retire\n");
        return enabled;
    }();
    return on;
}

namespace {
gpu::SubmissionGate gate;
// FM4 Recomp (FM4_GPU_TIMESTAMPS=1, diagnostic): GPU execution time of each submission from two timestamp queries, summed per
// swap, to split the flush waits into GPU work and submission round trips.
// FM4 Recomp (FM4_RING_MB=<16-1024>, opt-in; default 16): the slot's upload ring size. A race frame needs 100-240 MB of vertex
// data, so a 16 MB ring flushes (submit + wait) 7-14 times a frame; ShiftGlue grows its
// upload space instead of waiting (src/ui/graphics_upload_buffer_pool.cpp:93-145). The draws of one submission
// stay bounded by the descriptor pool (kDrawsPerSlot sets: a flush before it is exhausted).
uint64_t RingBytes()
{
    static const uint64_t bytes = [] {
        const char* v = std::getenv("FM4_RING_MB");
        if (v == nullptr)
            return uint64_t(16) << 20;
        char* end = nullptr;
        const unsigned long mb = std::strtoul(v, &end, 10);
        if (end == v || *end != '\0' || mb < 16 || mb > 1024)
            Stop(StopCode::Setup, "renderer: FM4_RING_MB=%s is not a size in MB from 16 to 1024", v);
        std::fprintf(stderr, "[gpu] upload ring %lu MB a slot (FM4_RING_MB)\n", mb);
        return uint64_t(mb) << 20;
    }();
    return bytes;
}

bool GpuTimestamps()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_GPU_TIMESTAMPS");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}
double timestampPeriodNs = 0;

// FM4 Recomp (FM4_PASS_MERGE=1, opt-in): consecutive draws on the same render pass and framebuffer share one render pass instance
// instead of one each. On a tile-based GPU (Apple, through MoltenVK) every instance loads and stores its whole attachments; one
// instance per draw (about 2,100 a race frame) moved them through memory each time. The pass stays open after a draw and is ended
// by the next draw on another framebuffer, by any recording outside a pass (render_hooks.h OutsideRenderPass) and before a
// submission. The technique, independently implemented: ShiftGlue (Xenia-derived, BSD) keeps its render pass while the next
// draw's pass and framebuffer match (src/graphics/vulkan/command_processor.cpp:5292-5311 at 72b7dd6f).
bool PassMerge()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_PASS_MERGE");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] render pass merge on (FM4_PASS_MERGE=1): consecutive draws on one framebuffer share a render pass\n");
        return enabled;
    }();
    return on;
}
struct OpenPass
{
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
} openPass;
uint64_t passesSinceSwap = 0;
uint64_t cpIdleAt = 0, selfWaitsAt = 0, immediateWaitsAt = 0, memWaitsAt = 0, syncedWaitsAt = 0, memWaitNsAt = 0, immediateInterruptsAt = 0, graceSyncsAt = 0;
uint64_t currentSwap = 0;  // the last swap Present saw (diagnostics)

// FM4 Recomp (FM4_DIAG_CONSUMER=1, diagnostic): what a "resolve consumer" flush waited for; each distinct one is logged once (at
// most 64): a vertex window or index buffer over a pending range, or a texture (its six fetch-constant words) that is not resident.
void DiagConsumer(const char* what, uint32_t physical, uint32_t bytes, const uint32_t* words)
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_CONSUMER");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    if (!on)
        return;
    static std::set<std::string> seen;
    char key[160];
    if (words != nullptr)
        std::snprintf(key, sizeof key, "%s %08X %08X %08X %08X %08X %08X", what, words[0], words[1], words[2], words[3], words[4], words[5]);
    else
        std::snprintf(key, sizeof key, "%s at physical %08X, %u bytes", what, physical, bytes);
    if (seen.size() < 64 && seen.insert(key).second)
        std::fprintf(stderr, "[diag consumer] swap %llu: %s waits for a resolve write-back\n", (unsigned long long)currentSwap, key);
}  // FM4_DIAG_THREAD_CPU: runtime::gpu::CpIdleNs at the previous swap
bool ThreadCpuDiag()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_THREAD_CPU");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] diagnostic FM4_DIAG_THREAD_CPU=1: CPU time per swap by thread role (kernel/thread_cpu.h) and command-processor idle time\n");
        return enabled;
    }();
    return on;
}
void EndOpenPass(VkCommandBuffer c)
{
    if (openPass.commands == VK_NULL_HANDLE)
        return;
    if (c != openPass.commands)
        Stop(StopCode::Setup, "renderer: a render pass is open on another command buffer than the one recorded into");
    vkCmdEndRenderPass(c);
    openPass = OpenPass{};
}
uint64_t gpuBusyNsSinceSwap = 0, gpuTimedSinceSwap = 0;
std::timed_mutex queueLock;  // vkQueueSubmit / vkQueuePresentKHR on dev.queue (window mode: two threads)

// Unit 8.
std::optional<GlslangCompiler> compiler;
std::optional<ShaderCache> shaders;
std::optional<PipelineCache> pipelines;
ShaderClass allowedClasses = kClassStraight;
SamplerLimits samplerLimits;  // the device's (CreateResources), for PlanDescriptors and the descriptor pools
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
std::optional<CaptureStore<kKeepRecords>> kept;  // FM4_CAPTURE_KEEP_EVERY
uint64_t keepEvery = 0;

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

// The slot's recorded commands to the queue, under the queue lock (window_present.h LockQueue: bounded).
void SubmitSlot(uint32_t s)
{
    std::unique_lock<std::timed_mutex> queue = LockQueue(queueLock, PresenterWaitBoundNs(guard->FenceTimeoutNs()), "submit");
    Require(guard->Submit(s));
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
    EndOpenPass(c);  // FM4_PASS_MERGE
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
    if (slot.timestamps != VK_NULL_HANDLE)  // FM4_GPU_TIMESTAMPS (diagnostic)
    {
        vkCmdResetQueryPool(c, slot.timestamps, 0, 2);
        vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.timestamps, 0);
        slot.timed = true, slot.stamped = false;
    }
    slot.allocator->Reset();
    Check(vkResetDescriptorPool(dev.device, slot.descriptors, 0), "vkResetDescriptorPool");
    slot.sets = 0;
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
    runtime::gpu::ApplyDeferredStores();  // FM4_DEFER_FENCES: the stream's stores, after the write-backs they follow (none queued otherwise)
    GuestWritesRetired();  // the resolves recorded into it wrote guest memory (guest_writes.h; unit 4's one slot)
    Slot& slot = slots[s];
    slot.open = false;
    if (slot.timed && slot.stamped)  // FM4_GPU_TIMESTAMPS (diagnostic): the submission is complete, so both queries are available
    {
        uint64_t t[2] = {};
        if (vkGetQueryPoolResults(dev.device, slot.timestamps, 0, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0])
            gpuBusyNsSinceSwap += uint64_t(double(t[1] - t[0]) * timestampPeriodNs), gpuTimedSinceSwap++;
    }
    slot.timed = slot.stamped = false;
    for (const Slot::ErrorDword& e : slot.errorDwords)
    {
        uint32_t error = 0;
        std::memcpy(&error, static_cast<const uint8_t*>(slot.ring.mapped) + e.offset, 4);
        if (error != 0)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: the vertex shader flagged xe_error %u (bit 0: a vertex fetch outside its slot's data; "
                                          "bit 1: a 16-bit float with exponent 31); %s", (unsigned long long)e.draw, error, e.what.c_str());
    }
    slot.errorDwords.clear();
}

// FM4_GPU_TIMESTAMPS (diagnostic): the end timestamp of the open slot, recorded last (outside any render pass).
void StampEnd(uint32_t s)
{
    EndOpenPass(slots[s].commands);  // FM4_PASS_MERGE: nothing is submitted inside a render pass
    if (slots[s].timed)
        vkCmdWriteTimestamp(slots[s].commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slots[s].timestamps, 1), slots[s].stamped = true;
}

// The open slot's recorded work, submitted and confirmed complete (a bounded wait), so that guest memory
// holds what it wrote (resolves) before the CPU reads it. Nothing to do when the slot is not open.
void FlushSlot(uint32_t s, FlushCause cause)
{
    if (!slots[s].open)
        return;
    const auto start = std::chrono::steady_clock::now();
    StampEnd(s);
    Check(vkEndCommandBuffer(slots[s].commands), "vkEndCommandBuffer");
    SubmitSlot(s);
    RetireSlot(s);  // confirmed completion, then the draws' xe_error dwords
    flushCountSinceSwap[size_t(cause)]++, flushNsSinceSwap[size_t(cause)] += NsSince(start);
}

// The display image (already in TRANSFER_DST_OPTIMAL after `written`) -> the readback buffer, made
// visible to the host. Leaves the display image in TRANSFER_SRC_OPTIMAL (the window's presenter blits from it).
void RecordReadback(VkCommandBuffer c)
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
}

// RecordReadback, then ends the recording.
void EndWithReadback(VkCommandBuffer c)
{
    RecordReadback(c);
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
    if (kept && id % keepEvery == 0)
    {
        if (!kept->Add(id, png.size(), [&](const std::string& path) {
                FILE* f = std::fopen(path.c_str(), "wb");
                if (f == nullptr)
                    return false;
                const bool written = std::fwrite(png.data(), 1, png.size(), f) == png.size();
                return std::fclose(f) == 0 && written;
            }, error))
            Stop(StopCode::Setup, "renderer: kept capture of %s: %s", what, error.c_str());
        std::fprintf(stderr, "[gpu] %s kept: %s\n", what, kept->PathOf(id).c_str());
    }
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
    // The sampler budgets of the per-pair layouts (pipeline_key.h SamplerLimits), queried, never assumed.
    samplerLimits.perStageSamplers = properties.limits.maxPerStageDescriptorSamplers;
    samplerLimits.perStageSampledImages = properties.limits.maxPerStageDescriptorSampledImages;
    samplerLimits.perStageResources = properties.limits.maxPerStageResources;
    samplerLimits.setSamplers = properties.limits.maxDescriptorSetSamplers;
    samplerLimits.setSampledImages = properties.limits.maxDescriptorSetSampledImages;
    std::fprintf(stderr, "[gpu] sampler limits: per stage %u samplers, %u sampled images, %u resources; per set %u samplers, %u sampled images\n",
                 samplerLimits.perStageSamplers, samplerLimits.perStageSampledImages, samplerLimits.perStageResources, samplerLimits.setSamplers,
                 samplerLimits.setSampledImages);
    if (uniformAlign == 0 || (uniformAlign & (uniformAlign - 1)) || storageAlign == 0 || (storageAlign & (storageAlign - 1)))
        Stop(StopCode::Setup, "renderer: buffer offset alignments %llu / %llu are not powers of two", (unsigned long long)uniformAlign,
             (unsigned long long)storageAlign);
    endOpenRenderPass = EndOpenPass;  // FM4_PASS_MERGE: the units end an open pass before recording outside one (render_hooks.h)
    if (GpuTimestamps())  // FM4 Recomp (diagnostic): only with timestamp support on the queue family
    {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, nullptr);
        std::vector<VkQueueFamilyProperties> family(families);
        vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, family.data());
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(dev.gpu, &properties);
        if (dev.queueFamily < families && family[dev.queueFamily].timestampValidBits > 0 && properties.limits.timestampPeriod > 0)
        {
            timestampPeriodNs = properties.limits.timestampPeriod;
            std::fprintf(stderr, "[gpu] diagnostic FM4_GPU_TIMESTAMPS=1: GPU time per submission (period %.3f ns, %u valid bits)\n", timestampPeriodNs,
                         family[dev.queueFamily].timestampValidBits);
        }
        else
            std::fprintf(stderr, "[gpu] diagnostic FM4_GPU_TIMESTAMPS=1 refused: the queue family has no timestamps\n");
    }
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
        if (timestampPeriodNs > 0)  // FM4_GPU_TIMESTAMPS (diagnostic)
        {
            VkQueryPoolCreateInfo query{};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 2;
            Check(vkCreateQueryPool(dev.device, &query, nullptr, &s.timestamps), "vkCreateQueryPool");
        }
        // Unit 8: the ring (uniform, storage and index data) and the descriptor pool, persistent per slot.
        // Unit 7: also the staging source of texture uploads (vkCmdCopyBufferToImage, texture_vk.cpp).
        s.ring = MakeBuffer(RingBytes(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        s.allocator.emplace(RingBytes(), uniformAlign, storageAlign);
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * kDrawsPerSlot},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kDrawsPerSlot},
            // A set holds only its pair's samplers (per-pair layouts), at most min(32, the set budget) of them: the worst case per set.
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::min<uint32_t>(kTextureBindings, std::min(samplerLimits.setSamplers, samplerLimits.setSampledImages)) * kDrawsPerSlot},
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
    endOpenRenderPass = nullptr, openPass = OpenPass{};
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
        vkDestroyQueryPool(dev.device, s.timestamps, nullptr);  // FM4_GPU_TIMESTAMPS: every submission confirmed complete (above)
        s.timestamps = VK_NULL_HANDLE, s.timed = s.stamped = false;
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

// FM4_SELFCHECK_EDRAM: unset, or "depth4x2x" (RunSelfCheck step 6, the 4x -> 2x depth transfer); anything else stops.
bool EdramSelfCheckRequested()
{
    const char* v = std::getenv("FM4_SELFCHECK_EDRAM");
    if (v == nullptr || *v == 0)
        return false;
    if (std::strcmp(v, "depth4x2x") != 0)
        Stop(StopCode::Setup, "renderer: FM4_SELFCHECK_EDRAM=%s: only depth4x2x is defined", v);
    return true;
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
    const uint8_t* direct = nullptr;  // FM4 Recomp (M7): the whole buffer in one view (PhysicalRangeToGuest), element 0 here
};
uint32_t ReadGuestIndex(const void* context, uint32_t i)
{
    const IndexSource& s = *static_cast<const IndexSource*>(context);
    const uint32_t size = s.indices->is32Bit ? 4 : 2, physical = s.indices->physical + i * size;
    if (s.direct != nullptr && uint64_t(i + 1) * size <= s.indices->bytes)  // one lookup per draw instead of a locked one per element
    {
        uint32_t raw = 0;
        std::memcpy(&raw, s.direct + uint64_t(i) * size, size);
        return raw;
    }
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
                                      "(unit 5 submits within a frame)", (unsigned long long)draw, (unsigned long long)RingBytes(), (unsigned long long)bytes,
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
    const KeepEvery keep = KeepEveryFromEnv(std::getenv("FM4_CAPTURE_KEEP_EVERY"));
    if (keep.malformed)
        Stop(StopCode::Setup, "renderer: FM4_CAPTURE_KEEP_EVERY is not a decimal number from 1 to 1000000");
    if (keep.every)
    {
        keepEvery = keep.every;
        kept.emplace(o.captureDir, "keep_", kKeepBytes);
        std::fprintf(stderr, "[gpu] diagnostic FM4_CAPTURE_KEEP_EVERY=%llu: every %llu-th swap also kept (at most %zu files, %llu MB)\n",
                     (unsigned long long)keepEvery, (unsigned long long)keepEvery, kKeepRecords, (unsigned long long)(kKeepBytes >> 20));
    }
    SetStopQuiesce(QuiesceForStop, ReportForSignalStop);
    if (o.mode == Mode::Window && o.surface == nullptr)
        Stop(StopCode::Setup, "renderer: window mode needs the window's surface request (window_sdl.h OpenWindow)");
    std::fprintf(stderr, "[gpu] renderer %s: fence timeout %u ms, captures %s (at most %zu files, %llu MB), GPU_STOP %s, shader classes allowed: %s\n",
                 o.mode == Mode::SelfCheck ? "self-check" : o.mode == Mode::Window ? "window" : "present", timeout.ms, o.captureDir, kCaptureRecords,
                 (unsigned long long)(kCaptureBytes >> 20), o.gpuStopPath, allowedClasses == kClassStraight ? "none (straight-line only)" : ShaderClassName(allowedClasses).c_str());
    dev = CreateVulkanDevice(o.validation, o.mode == Mode::Window ? o.surface : nullptr);  // surface and swapchain: window mode only
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
    SetTextureDumpDir(o.captureDir);              // FM4_DUMP_TEXTURE (diagnostic, texture_dump.h): nothing unless set
    SetTextureHooks(InitializeTextureUnit(dev));  // unit 7 (texture_vk.h)
    // The EDRAM unit (edram_vk.h): its transfer shaders through the same compiler, its resolve writes through main.cpp's
    // writable guest base, each recorded resolve reported to the texture unit (guest_writes.h MarkGuestWritten). Present
    // mode only: the self-check draws nothing and keeps its device work unchanged, unless FM4_SELFCHECK_EDRAM asks for the
    // isolated EDRAM check (RunSelfCheck step 6).
    if (o.mode == Mode::Present || o.mode == Mode::Window || (o.mode == Mode::SelfCheck && EdramSelfCheckRequested()))
    {
        if (o.guestBase == nullptr)
            Stop(StopCode::Setup, "renderer: present mode needs the writable guest base (Options::guestBase)");
        std::string edramError;
        const EdramHooks* hooks = InitializeEdramVk(dev, *compiler, o.guestBase, MarkGuestWritten, edramError);
        if (hooks == nullptr)
            Stop(StopCode::Setup, "renderer: EDRAM unit: %s", edramError.c_str());
        SetEdramHooks(hooks);
    }
    if (o.mode == Mode::Window)  // on the main thread (main.cpp): the swapchain, the presenter's slot and guard
        InitializePresenter({&dev, display, {kDisplayWidth, kDisplayHeight}, &gate, o.gpuStopPath, guard->FenceTimeoutNs(), &queueLock});
    enabled = true;
    return true;
}

void Present(const uint8_t* base, const uint32_t fetch[6], uint32_t physical, uint32_t width, uint32_t height, uint64_t swap)
{
    currentSwap = swap;
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
    FlushSlot(0, FlushCause::Swap);
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
    const bool windowed = options.mode == Mode::Window;
    if (windowed)  // the display image is written below: never while the presenter blits from it (bounded wait)
        if (HandOver().BeginWrite(PresenterWaitBoundNs(guard->FenceTimeoutNs())) == window::FrameHandOver::Write::TimedOut)
            Stop(StopCode::Setup, "renderer: swap %llu: the window's presenter still reads the display image after %llu ms", (unsigned long long)swap,
                 (unsigned long long)(PresenterWaitBoundNs(guard->FenceTimeoutNs()) / 1000000));
    const VkCommandBuffer c = OpenSlot(0);  // a fresh recording: the frame's draws completed above
    if (!guard->Hold(0, staging))
        Stop(StopCode::Setup, "renderer: the staging buffer cannot be held (slot not idle)");
    ImageBarrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(c, staging.buffer, display, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    RecordReadback(c);
    StampEnd(0);
    Check(vkEndCommandBuffer(c), "vkEndCommandBuffer");
    const auto presentStart = std::chrono::steady_clock::now();
    SubmitSlot(0);
    RetireSlot(0);  // one bounded wait per swap: the one-frame throttle (execution model §2); then the draws' xe_error dwords
    flushNsSinceSwap[size_t(FlushCause::Swap)] += NsSince(presentStart);
    EvictIdleTextures();  // FM4 Recomp: every submission confirmed complete just above (GPU safety: no in-flight use)
    Capture(swap, "front buffer");
    if (windowed && !HandOver().Offer(swap))  // the main thread blits it to the swapchain and presents (window_present.cpp)
        Stop(StopCode::Setup, "renderer: swap %llu: the window hand-over was not idle after BeginWrite (contract breach)", (unsigned long long)swap);
    std::fprintf(stderr, "[gpu] swap %llu presented %s; since the last swap: %llu draw(s) rendered, %llu no-write draw(s) %s, %llu resolve(s), "
                         "%llu shader load(s); %llu bytes of the slot's ring used, %llu ring flush(es); %s\n",
                 (unsigned long long)swap, windowed ? "offscreen and handed to the window" : "offscreen", (unsigned long long)drawsSinceSwap, (unsigned long long)noWriteSinceSwap,
                 kNoWriteDraws == NoWriteDraws::Skip ? "recorded as nothing (draw_record.h kNoWriteDraws)" : "(none allowed)",
                 (unsigned long long)resolvesSinceSwap, (unsigned long long)shadersSinceSwap, (unsigned long long)slots[0].allocator->Used(),
                 (unsigned long long)ringFlushesSinceSwap, TextureUnitSummary().c_str());
    {
        std::string timing;
        uint64_t waits = 0;
        for (size_t k = 0; k < size_t(FlushCause::Count); k++)
        {
            waits += flushNsSinceSwap[k];
            if (flushCountSinceSwap[k] || flushNsSinceSwap[k])
            {
                char part[96];
                std::snprintf(part, sizeof part, "; %s %llu x %.1f ms", kFlushCauseNames[k], (unsigned long long)flushCountSinceSwap[k],
                              flushNsSinceSwap[k] / 1e6);
                timing += part;
            }
            flushCountSinceSwap[k] = flushNsSinceSwap[k] = 0;
        }
        const char* separator = " (CPU sync by packet:";
        for (uint32_t op = 0; op < 128; op++)
            if (syncFlushesByPacketSinceSwap[op])
            {
                char part[48];
                std::snprintf(part, sizeof part, "%s %02X x %llu", separator, op, (unsigned long long)syncFlushesByPacketSinceSwap[op]);
                timing += part, separator = ",";
                syncFlushesByPacketSinceSwap[op] = 0;
            }
        if (*separator == ',')
            timing += ")";
        char gpuBusy[448] = "";
        if (timestampPeriodNs > 0)  // FM4_GPU_TIMESTAMPS (diagnostic)
            std::snprintf(gpuBusy, sizeof gpuBusy, "; GPU busy %.1f ms in %llu submission(s)", gpuBusyNsSinceSwap / 1e6, (unsigned long long)gpuTimedSinceSwap);
        if (ThreadCpuDiag())  // FM4_DIAG_THREAD_CPU (diagnostic): CPU time by thread role and the command processor's idle time
        {
            const runtime::ThreadCpu cpu = runtime::ThreadCpuSince();
            const uint64_t idle = runtime::gpu::CpIdleNs();
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; CPU: guest %.1f ms (%u threads), cp %.1f ms, other %.1f ms; cp idle %.1f ms",
                          cpu.guestMs, cpu.guestThreads, cpu.cpMs, cpu.otherMs, (idle - cpIdleAt) / 1e6);
            cpIdleAt = idle;
        }
        if (DeferFences())  // FM4_DEFER_FENCES: waits a queued store satisfied (no flush)
        {
            const uint64_t self = runtime::gpu::SelfWaits();
            const uint64_t immediate = runtime::gpu::ImmediateWaits();
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; self-waits %llu, immediate waits %llu",
                          (unsigned long long)(self - selfWaitsAt), (unsigned long long)(immediate - immediateWaitsAt));
            const uint64_t mem = runtime::gpu::MemWaits(), synced = runtime::gpu::SyncedWaits(), memNs = runtime::gpu::MemWaitNs();
            const uint64_t interrupts = runtime::gpu::ImmediateInterrupts(), grace = runtime::gpu::GraceSyncs();
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy),
                          ", memory waits %llu (%llu synced, %llu after the grace), %.1f ms; immediate interrupts %llu",
                          (unsigned long long)(mem - memWaitsAt), (unsigned long long)(synced - syncedWaitsAt), (unsigned long long)(grace - graceSyncsAt),
                          (memNs - memWaitNsAt) / 1e6, (unsigned long long)(interrupts - immediateInterruptsAt));
            selfWaitsAt = self, immediateWaitsAt = immediate, memWaitsAt = mem, syncedWaitsAt = synced, memWaitNsAt = memNs;
            immediateInterruptsAt = interrupts, graceSyncsAt = grace;
        }
        if (PassMerge())  // FM4_PASS_MERGE: render pass instances begun by draws
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu draw render pass(es)", (unsigned long long)passesSinceSwap);
        std::fprintf(stderr, "[gpu] swap %llu timing: %.1f ms since the last swap; submit + wait %.1f ms%s; texture binds %.1f ms; ring need: textures %.1f MB, "
                             "vertices %.1f MB, indices %.1f MB, fixed blocks %.1f MB; vblank %llu%s\n",
                     (unsigned long long)swap, NsSince(lastSwapEnd) / 1e6, waits / 1e6, timing.c_str(), textureBindNsSinceSwap / 1e6,
                     ringNeedSinceSwap[0] / 1048576.0, ringNeedSinceSwap[1] / 1048576.0, ringNeedSinceSwap[2] / 1048576.0, ringNeedSinceSwap[3] / 1048576.0,
                     (unsigned long long)runtime::gpu::VblankCount(), gpuBusy);  // FM4 Recomp: guest time (the input script's clock) per swap
        gpuBusyNsSinceSwap = gpuTimedSinceSwap = passesSinceSwap = 0;
        for (uint64_t& n : ringNeedSinceSwap)
            n = 0;
        textureBindNsSinceSwap = 0;
        lastSwapEnd = std::chrono::steady_clock::now();
    }
    // FM4 Recomp (M7 A/B harness, input_script.h @race): the first swap of 1,000 or more draws is the race start (the front end and
    // the loading screens draw about 100 to 400; a race frame 1,400 to 2,700). Nothing without a @race script.
    if (drawsSinceSwap >= 1000)
        runtime::input::NotifyRaceStart(runtime::gpu::VblankCount());
    drawsSinceSwap = shadersSinceSwap = noWriteSinceSwap = resolvesSinceSwap = ringFlushesSinceSwap = 0;
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
    // FM4_CP_TRACE: the fetch constants of every texture slot the bound pair samples (the CP logs slots 0-2 of DRAW_INDX only).
    if (runtime::gpu::TraceEnabled())
    {
        uint32_t mask = 0;
        for (const CachedShader* stage : {vs, ps})
            for (const uint32_t s : stage->t.textureSlots)
                mask |= s < 32 ? 1u << s : 0u;
        runtime::gpu::TraceTextures(base, opcode, mask);
    }
    TracePsDraw(base, draw, vs->hash, ps->hash, vs->t);  // FM4_TRACE_PS (diagnostic, ps_trace.h): nothing unless set
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
    {
        if (edramHooks == nullptr)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: a resolve (RB_MODECONTROL 6) without an EDRAM unit", (unsigned long long)draw);
        // FM4 Recomp: the EDRAM unit holds one pending resolve per submission (edram_vk.cpp Resolve: one staging buffer), so a
        // deferred one is written back before the next is recorded.
        // FM4_RESOLVE_RESIDENT: it holds several while its staging has room (EdramHooks::resolveRoom).
        if (ResolveDefer() && slots[0].open && GuestWritesPending() && !(ResolveResident() && edramHooks->resolveRoom()))
            FlushSlot(0, FlushCause::NextResolve);
        const VkCommandBuffer commands = OpenSlot(0);
        if (!edramHooks->resolve(commands, base, d, draw, error))
            Stop(StopCode::Unimplemented, "renderer: draw %llu: resolve: %s", (unsigned long long)draw, error.c_str());
        resolvesSinceSwap++;
        // FM4 Recomp (resolve_resident.h): the textures that take the resolve's words get them now; the others over its range wait.
        if (ResolveResident() && textures != nullptr)
        {
            ResidentResolve last;
            VkImage words = VK_NULL_HANDLE;
            if (edramHooks->lastResolve(last, words))
                textures->adoptResolve(commands, last, words);
        }
        // Every resolve completes before the next draw records: that draw's texture loads read guest memory on
        // the CPU (trace11 samples the video resolve's destination in the same frame). Unconditional, so the
        // order does not depend on the EDRAM unit calling MarkGuestWritten (which only moves the watch's
        // sequences and arms texture_upload.h's pending refusal). Unit 4: one bounded wait per resolve.
        // FM4 Recomp: with FM4_RESOLVE_DEFER the write-back waits for its first reader instead (ResolveDefer).
        if (!ResolveDefer())
            FlushSlot(0, FlushCause::Resolve);
        return;
    }
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
    // FM4 Recomp (FM4_RESOLVE_DEFER): an index buffer a pending resolve writes is read after its write-back.
    if (ResolveDefer() && GuestWritesPending() && d.indices.indexed && GuestWritesOverlap(d.indices.physical, d.indices.bytes))
    {
        DiagConsumer("index buffer", d.indices.physical, d.indices.bytes, nullptr);
        FlushSlot(0, FlushCause::Consumer);
    }
    HostIndices indices;
    IndexSource source{base, &d.indices, draw};
    if (uint32_t guest = 0; d.indices.indexed && runtime::PhysicalRangeToGuest(d.indices.physical, d.indices.bytes, guest))
        source.direct = base + guest;
    RequireDraw(draw, BuildHostIndices(d, d.indices.indexed ? ReadGuestIndex : nullptr, &source, indices));
    RequireDraw(draw, GpuGuard<Transient, kSlots>::DrawLoopWork(d.vsLoopRegisters, indices.vertexCount, d.psLoopRegisters, d.pixelInvocations));
    // 4. The shader variants, the pipeline key and the descriptor plan (CPU).
    ShaderVariantDefines vsDefines, psDefines;
    RequireDraw(draw, VariantsFromDrawState(d, *vs, *ps, vsDefines, psDefines));
    DiagAlphaQuantize(ps->hash, psDefines);  // FM4_DIAG_ALPHA_QUANT (diagnostic, diag_alpha.h): nothing unless set
    ShaderVariant* vsVariant = shaders->Variant(*vs, vsDefines, error);
    if (vsVariant == nullptr)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, error.c_str());
    ShaderVariant* psVariant = shaders->Variant(*ps, psDefines, error);
    if (psVariant == nullptr)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, error.c_str());
    PipelineKey key;
    RequireDraw(draw, KeyFromDrawState(d, *vs, *ps, vsVariant->id, psVariant->id, key));
    const bool noDepthDiagnostic = DiagNoDepth(ps->hash, key);  // FM4_DIAG_PS_NO_DEPTH (diagnostic, diag_depth.h): nothing unless set
    DescriptorPlan plan;
    RequireDraw(draw, PlanDescriptors(*vs, *ps, samplerLimits, plan));
    key.samplerLayout = plan.layoutKey;  // the pipeline layout is the pair's (PipelineCache::Get refuses a key without it)
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
    // FM4 Recomp (FM4_RESOLVE_DEFER): vertex windows and textures a pending resolve writes are read after its write-back (one
    // flush, before anything of this draw reads guest memory or is recorded).
    if (ResolveDefer() && GuestWritesPending())
    {
        bool needs = false;
        for (const VertexWindow& w : windows)
            if (w.dwords != 0 && GuestWritesOverlap(w.physical, w.dwords * 4))
                needs = true, DiagConsumer("vertex window", w.physical, w.dwords * 4, nullptr);
        if (textures != nullptr)
            for (const TextureSlot& t : d.textureSlots)
                if (textures->readsPendingWrite(t))
                    needs = true, DiagConsumer("texture", 0, 0, t.words);
        if (needs)
            FlushSlot(0, FlushCause::Consumer);
    }
    const auto windowBytes = [&](const VertexWindow& w) -> const uint8_t* {
        uint32_t guest = 0, guestEnd = 0;
        if (!runtime::PhysicalToGuest(w.physical, guest) || !runtime::PhysicalToGuest(w.physical + w.dwords * 4 - 1, guestEnd) || guestEnd - guest != w.dwords * 4 - 1)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex fetch slot %u (%08X, %u dwords) is not in one view", (unsigned long long)draw, w.slot,
                 w.physical, w.dwords);
        if (const runtime::Heap* heap = runtime::LookupHeap(guest); heap == nullptr || !heap->IsCommitted(guest, w.dwords * 4))
            Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex fetch slot %u (%08X, %u dwords) reaches uncommitted guest memory", (unsigned long long)draw,
                 w.slot, w.physical, w.dwords);
        return base + guest;
    };
    // A window past its fetch constant's size (VertexWindowDwords: read from guest memory, as Xenia): capped, and named once per
    // (vertex shader, slot) with the draw's index state and, when it reaches past twice the size, the first words past the constant.
    for (size_t k = 0; k < windows.size(); k++)
    {
        const VertexWindow& w = windows[k];
        const VertexSlot& v = d.vertexSlots[k];
        if (w.dwords <= v.sizeWords)
            continue;
        if (w.dwords > kMaxVertexWindowDwords)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex fetch slot %u (%08X, %u dwords) needs a %u-dword window, more than the cap %u",
                 (unsigned long long)draw, w.slot, w.physical, v.sizeWords, w.dwords, kMaxVertexWindowDwords);
        static std::set<std::pair<uint64_t, uint32_t>> named;
        if (!named.insert({vs->hash, w.slot}).second)
            continue;
        std::string past;
        if (w.dwords > 2 * v.sizeWords)
        {
            const uint8_t* bytes = windowBytes(w);
            for (uint32_t i = v.sizeWords; i < std::min(w.dwords, v.sizeWords + 24); i++)
            {
                char word[16];
                std::snprintf(word, sizeof word, " %02X%02X%02X%02X", bytes[4 * i], bytes[4 * i + 1], bytes[4 * i + 2], bytes[4 * i + 3]);
                past += word;
            }
        }
        std::fprintf(stderr, "[draw] draw %llu: vs_%016llx vertex slot %u at %08X reaches past its fetch constant: indices up to %u x stride %u + end %u = %u dwords, "
                             "the constant holds %u (primitive %u, %s, count %u, offset %u, min %u, max %u)%s%s\n",
                     (unsigned long long)draw, (unsigned long long)vs->hash, w.slot, w.physical, indices.highIndex, v.stride, v.end, w.dwords, v.sizeWords,
                     d.primitiveType, d.indices.indexed ? "indexed" : "auto", d.indices.count, d.indexOffset, d.minIndex, d.maxIndex,
                     past.empty() ? "" : "; guest bytes past it:", past.c_str());
    }
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
    // The slot's ring must hold this whole draw (every texture reloads at every bind: texture_upload.h kTextureValidity).
    // Draws are separate render passes, so a full ring is flushed between draws (submit, confirmed retire, reset) rather
    // than refusing the draw (one observed draw had 14,971,540 of 16,777,216 bytes used, 1,869,120 more needed).
    {
        const uint64_t slack = 256;  // per block: the device's uniform/storage alignment is at most 256 (Vulkan limits)
        uint64_t need = kSystemBytes + 2 * uint64_t(kConstantsBytes) + std::max<uint64_t>(uint64_t(vertexDwords) * 4, 4) +
                        std::max<uint64_t>(indices.indices.size() * 4, 4) + kErrorsBytes + 6 * slack;
        const uint64_t fixed = need - std::max<uint64_t>(uint64_t(vertexDwords) * 4, 4) - std::max<uint64_t>(indices.indices.size() * 4, 4);
        ringNeedSinceSwap[1] += std::max<uint64_t>(uint64_t(vertexDwords) * 4, 4), ringNeedSinceSwap[2] += std::max<uint64_t>(indices.indices.size() * 4, 4);
        ringNeedSinceSwap[3] += fixed;
        for (uint32_t s = 0; s < kTextureBindings; s++)
            if (plan.dimension[s] != 0 && textures != nullptr)
                for (const TextureSlot& t : d.textureSlots)
                    if (t.slot == s)
                    {
                        const uint64_t bytes = textures->stagingBytes(t, base);
                        need += bytes, ringNeedSinceSwap[0] += bytes;
                    }
        if (need > slot.allocator->Capacity())
            Stop(StopCode::Unimplemented, "renderer: draw %llu: needs %llu ring bytes, more than the slot's %llu", (unsigned long long)draw,
                 (unsigned long long)need, (unsigned long long)slot.allocator->Capacity());
        if (slot.open && (slot.allocator->Used() + need > slot.allocator->Capacity() || slot.sets >= kDrawsPerSlot))
        {
            FlushSlot(0, FlushCause::Ring);
            ringFlushesSinceSwap++;
        }
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
    {
        char hash[24];
        std::snprintf(hash, sizeof hash, "vs_%016llx", (unsigned long long)vs->hash);
        std::string what = std::string(hash) + ", highest index " + std::to_string(indices.highIndex);
        for (size_t k = 0; k < windows.size(); k++)
            what += "; slot " + std::to_string(windows[k].slot) + ": window " + std::to_string(windows[k].dwords) + " dwords, constant " +
                    std::to_string(d.vertexSlots[k].sizeWords) + ", stride " + std::to_string(d.vertexSlots[k].stride) + ", end " +
                    std::to_string(d.vertexSlots[k].end) + (d.vertexSlots[k].computedIndex ? ", computed index" : "");
        slot.errorDwords.push_back({errorAt, draw, what});
    }
    // 8. The descriptor set: the six buffer bindings and the textures the pair samples.
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkDescriptorSetLayout layout = pipeline.layouts.set;
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = slot.descriptors;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &layout;
    if (const VkResult r = vkAllocateDescriptorSets(dev.device, &setInfo, &set); r != VK_SUCCESS)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: vkAllocateDescriptorSets failed (VkResult %d): the slot's pool (%u sets) is exhausted "
             "(this set: 6 buffers, %d samplers)", (unsigned long long)draw, int(r), kDrawsPerSlot, __builtin_popcount(plan.vsSlots | plan.psSlots));
    slot.sets++;
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
        const auto bindStart = std::chrono::steady_clock::now();
        const bool bound = textures->bind(context, *t, view, sampler, error);
        textureBindNsSinceSwap += NsSince(bindStart);
        if (!bound || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: texture slot %u (%08X %08X %08X %08X %08X %08X): %s", (unsigned long long)draw, s,
                 t->words[0], t->words[1], t->words[2], t->words[3], t->words[4], t->words[5], error.empty() ? "no view or sampler" : error.c_str());
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
    if (openPass.commands != c || openPass.pass != pipeline.renderPass || openPass.framebuffer != framebuffer)
    {
        EndOpenPass(openPass.commands);
        vkCmdBeginRenderPass(c, &pass, VK_SUBPASS_CONTENTS_INLINE);
        passesSinceSwap++;
        if (PassMerge())
            openPass = {c, pipeline.renderPass, framebuffer};
    }
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layouts.pipeline, 0, 1, &set, 0, nullptr);
    const HostViewport hv = FoldViewport(d.viewport, targets.width, targets.height);
    const VkViewport viewport{0.0f, 0.0f, float(targets.width), float(targets.height), hv.zMin, hv.zMax};
    vkCmdSetViewport(c, 0, 1, &viewport);
    const VkRect2D scissor{{int32_t(d.scissor.x), int32_t(d.scissor.y)}, {d.scissor.width, scissorBottom - d.scissor.y}};
    vkCmdSetScissor(c, 0, 1, &scissor);
    {
        // FM4_TRACE_PS (diagnostic, ps_trace.h): the bind and dynamic state of a traced draw.
        PsBindTrace t;
        t.depthControl = runtime::gpu::ReadRegister(base, 0x2200), t.suScModeCntl = runtime::gpu::ReadRegister(base, 0x2205);
        t.pitch = target.pitch, t.msaa = target.msaa, t.colorBase = target.colorBase, t.colorFormat = target.colorFormat;
        t.depthBase = target.depthBase, t.depthFormat = target.depthFormat, t.pitchTiles = target.pitchTiles, t.edramRows = target.edramRows;
        t.targetHeight = target.targetHeight, t.colorWritten = target.colorWritten, t.depthWritten = target.depthWritten;
        t.keyDepthUsed = key.depthUsed, t.keyDepthWrite = key.depthWrite, t.keyDepthFunc = key.depthFunc, t.keyStencil = key.stencilEnable;
        t.keySamples = key.samples, t.hostWidth = targets.width, t.hostHeight = targets.height, t.hostSamples = targets.samples;
        t.scissorX = scissor.offset.x, t.scissorY = scissor.offset.y, t.scissorWidth = scissor.extent.width, t.scissorHeight = scissor.extent.height;
        t.viewportWidth = viewport.width, t.viewportHeight = viewport.height, t.minDepth = viewport.minDepth, t.maxDepth = viewport.maxDepth;
        t.noDepthDiagnostic = noDepthDiagnostic;
        t.colorControl = runtime::gpu::ReadRegister(base, 0x2202);
        for (uint32_t i = 0; i < 6; i++)
            t.vport[i] = runtime::gpu::ReadRegister(base, 0x210F + i);
        t.clipCntl = runtime::gpu::ReadRegister(base, 0x2204), t.vteCntl = runtime::gpu::ReadRegister(base, 0x2206);
        t.vtxCntl = runtime::gpu::ReadRegister(base, 0x2302), t.systemFlags = d.systemFlags;
        t.foldZScale = hv.scale[2], t.foldZOffset = hv.offset[2];
        const std::string psDefineText = psDefines.Joined();
        t.psDefines = psDefineText.c_str();
        TracePsBind(draw, t);
    }
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, d.depthStencil.front.ref);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, d.depthStencil.back.ref);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, d.depthStencil.front.compareMask);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, d.depthStencil.back.compareMask);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, d.depthStencil.front.writeMask);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, d.depthStencil.back.writeMask);
    vkCmdSetBlendConstants(c, d.blend.constant);
    const HostDepthBias bias = DepthBiasFor(d);  // the pipeline enables depth bias only with a depth attachment and an offset
    vkCmdSetDepthBias(c, bias.constant, 0.0f, bias.slope);
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
    if (!PassMerge())
        vkCmdEndRenderPass(c);
    drawsSinceSwap++;
}

bool DeferFences()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DEFER_FENCES");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && ResolveDefer();
        if (asked)
            std::fprintf(stderr, enabled ? "[gpu] deferred stream stores on (FM4_DEFER_FENCES=1): fences and memory writes land at GPU completion\n"
                                         : "[gpu] FM4_DEFER_FENCES=1 ignored: it needs FM4_RESOLVE_DEFER=1\n");
        return enabled;
    }();
    return on;
}

bool DeferStore(uint32_t physical)
{
    if (!DeferFences() || !slots[0].open)
        return false;
    MarkGuestWritten(physical, 4);  // pending: a draw reading these bytes, or a CPU sync point, waits for the retire
    return true;
}

bool WorkOpen() { return DeferFences() && slots[0].open; }

bool PendingAt(uint32_t physical, uint32_t bytes)
{
    return ResolveDefer() && slots[0].open && GuestWritesPending() && GuestWritesOverlap(physical, bytes);
}

void RingIdle()
{
    if (slots[0].open)
        FlushSlot(0, FlushCause::Idle);
    else
        runtime::gpu::ApplyDeferredStores();  // nothing open: every submission they followed is already complete
}

void SyncGuestWrites(uint32_t opcode)
{
    if (ResolveDefer() && slots[0].open && GuestWritesPending())
    {
        FlushSlot(0, FlushCause::Sync);
        syncFlushesByPacketSinceSwap[opcode & 0x7F]++;
    }
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
        std::fprintf(stderr, "[shader] %s_%016llx: %u words, class %s, %zu vertex fetch slot(s), %zu texture slot(s), %zu bool(s), %zu loop(s)%s\n",
                     shader->pixel ? "ps" : "vs", (unsigned long long)shader->hash, shader->words, ShaderClassName(shader->cls).c_str(),
                     shader->t.vertexFetchSlots.size(), shader->t.textureSlots.size(), shader->t.boolConstants.size(), shader->t.loopConstants.size(),
                     !shader->t.pathZeroReads.empty() ? (", " + std::to_string(shader->t.pathZeroReads.size()) +
                                                " path-dependent read(s) of an unwritten register (zero, as Xenia)").c_str() : "");
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

    // 6. (FM4_SELFCHECK_EDRAM=depth4x2x only) The EDRAM depth transfer 4x -> 2x on the device, isolated: known depth and
    //    stencil uploaded into the 1x key, transferred 1x -> 4x and 4x -> 2x by the unit's own transfer passes, every guest
    //    sample of both dumped and compared with the EDRAM sample layout's expectation (edram_vk.h RecordDepthSelfCheck).
    const bool edramCheck = EdramSelfCheckRequested();
    if (edramCheck)
    {
        Require(guard->Begin("self-check EDRAM depth 4x -> 2x"));
        const VkCommandBuffer e = BeginSlot(0);
        std::string error;
        const bool recorded = RecordDepthSelfCheck(e, error);
        if (!recorded)
            std::fprintf(stderr, "[gpu self-check] EDRAM depth 4x -> 2x: %s\n", error.c_str());
        // Ended even after a refusal, so the command buffer is well formed (nothing below submits it then). No display
        // readback: step 1 left the display image in TRANSFER_SRC, and this step reads its results from the EDRAM staging buffer.
        Check(vkEndCommandBuffer(e), "vkEndCommandBuffer");
        Expect(recorded, "EDRAM depth 4x -> 2x recorded (1x upload, transfers 1x -> 4x and 4x -> 2x, depth dumps)");
        Require(guard->Submit(0));
        Require(guard->Retire(0));
        const DepthSelfCheckResult r = VerifyDepthSelfCheck();
        std::fprintf(stderr, "[gpu self-check] EDRAM depth: 4x %llu sample(s), %llu mismatch(es); 2x %llu sample(s), %llu mismatch(es)%s%s\n",
                     (unsigned long long)r.checked4x, (unsigned long long)r.mismatches4x, (unsigned long long)r.checked2x,
                     (unsigned long long)r.mismatches2x, r.first.empty() ? "" : "; first: ", r.first.c_str());
        Expect(r.checked4x == 640ull * 16 * 4 && r.mismatches4x == 0, "EDRAM 1x -> 4x depth: every guest sample's depth and stencil word as the layout places it");
        Expect(r.checked2x == 1280ull * 16 * 2 && r.mismatches2x == 0, "EDRAM 4x -> 2x depth: every guest sample's depth and stencil word as the layout places it");
        std::fprintf(stderr, "[gpu self-check] EDRAM depth formats: kD24FS8 -> kD24S8 %llu pixel(s), %llu mismatch(es); back %llu pixel(s), %llu mismatch(es)\n",
                     (unsigned long long)r.checkedS8, (unsigned long long)r.mismatchesS8, (unsigned long long)r.checkedRoundTrip,
                     (unsigned long long)r.mismatchesRoundTrip);
        Expect(r.checkedS8 == 1280ull * 32 && r.mismatchesS8 == 0, "EDRAM depth kD24FS8 -> kD24S8: every pixel's word as ConvertDepthFormat gives it");
        Expect(r.checkedRoundTrip == 1280ull * 32 && r.mismatchesRoundTrip == 0, "EDRAM depth kD24S8 -> kD24FS8 (back): every pixel's word as ConvertDepthFormat gives it");
    }

    Shutdown();
    Stop(StopCode::GpuSelfCheck, "GPU self-check passed: device, clear and upload read back, deferred destruction, bounded fence wait (stranding "
                            "on a scratch flag), GPU_STOP refusal, loop-work cap, capped captures%s; %llu validation message(s)",
         edramCheck ? ", EDRAM depth transfers 4x -> 2x and between kD24FS8 and kD24S8" : "", (unsigned long long)ValidationMessageCount());
}

}  // namespace runtime::video
