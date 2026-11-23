// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// texture_vk.h. New code. The image, view and sampler parameters follow Xenia xenia-canary 67f16c4e315f
// src/xenia/gpu/vulkan/vulkan_texture_cache.cc (host formats :91-395 kBestHostFormats, through
// texture_info.cpp; VkSamplerCreateInfo :755-820, with texture_info.h's documented difference that min and
// mip filters are their own fields), BSD-3-Clause, read, not copied. The upload is the renderer's present
// upload pattern (renderer.cpp Present: staging -> vkCmdCopyBufferToImage between two layout barriers),
// with the staging block in the slot's ring instead of a held buffer.
#include "texture_vk.h"
#include "guest_writes.h"
#include "pipeline_key.h"
#include "stops.h"
#include "diag_texture_uploads.h"
#include "diag_watch_stale.h"  // FM4_WRITE_WATCH_VERIFY=1 (diagnostic)
#include "texture_dump.h"
#include "texture_upload.h"
#include "render_scale.h"  // FM4 Recomp (M9, FM4_INTERNAL_RES)
#include "mem_ledger.h"    // FM4 Recomp (M9, FM4_DIAG_MEMORY)
#include "resolve_resident.h"
#include "texture_untile_gpu.h"
#include "guest_memory_vk.h"
#include "gpu_read_guard.h"
#include "shader_cache.h"
#include "kernel/virtual_memory.h"
#include "guest_memory.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <set>
#include <tuple>
#include <cstdio>
#include <optional>
#include <unordered_map>

namespace runtime::video {

// texture_info.h's numeric mirrors of Vulkan (it builds without the header; its comment promises these).
static_assert(vk::kFormatUndefined == VK_FORMAT_UNDEFINED && vk::kFormatR8Unorm == VK_FORMAT_R8_UNORM &&
              vk::kFormatR8G8B8A8Unorm == VK_FORMAT_R8G8B8A8_UNORM && vk::kFormatA2B10G10R10UnormPack32 == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
              vk::kFormatBc1RgbaUnormBlock == VK_FORMAT_BC1_RGBA_UNORM_BLOCK && vk::kFormatBc3UnormBlock == VK_FORMAT_BC3_UNORM_BLOCK &&
              vk::kFormatBc2UnormBlock == VK_FORMAT_BC2_UNORM_BLOCK &&
              vk::kFormatBc4UnormBlock == VK_FORMAT_BC4_UNORM_BLOCK && vk::kFormatBc5UnormBlock == VK_FORMAT_BC5_UNORM_BLOCK &&
              vk::kFormatR16G16Unorm == VK_FORMAT_R16G16_UNORM && vk::kFormatR16G16Snorm == VK_FORMAT_R16G16_SNORM &&
              vk::kFormatR8G8Unorm == VK_FORMAT_R8G8_UNORM && vk::kFormatR8G8Snorm == VK_FORMAT_R8G8_SNORM &&
              vk::kFormatR16G16B16A16Sfloat == VK_FORMAT_R16G16B16A16_SFLOAT && vk::kFormatR5G6B5UnormPack16 == VK_FORMAT_R5G6B5_UNORM_PACK16);
static_assert(vk::kFilterNearest == VK_FILTER_NEAREST && vk::kFilterLinear == VK_FILTER_LINEAR &&
              vk::kMipmapModeNearest == VK_SAMPLER_MIPMAP_MODE_NEAREST && vk::kMipmapModeLinear == VK_SAMPLER_MIPMAP_MODE_LINEAR);
static_assert(vk::kAddressRepeat == VK_SAMPLER_ADDRESS_MODE_REPEAT && vk::kAddressMirroredRepeat == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT &&
              vk::kAddressClampToEdge == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && vk::kAddressClampToBorder == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
static_assert(vk::kBorderFloatTransparentBlack == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK && vk::kBorderFloatOpaqueWhite == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
static_assert(vk::kSwizzleZero == VK_COMPONENT_SWIZZLE_ZERO && vk::kSwizzleOne == VK_COMPONENT_SWIZZLE_ONE && vk::kSwizzleR == VK_COMPONENT_SWIZZLE_R &&
              vk::kSwizzleG == VK_COMPONENT_SWIZZLE_G && vk::kSwizzleB == VK_COMPONENT_SWIZZLE_B && vk::kSwizzleA == VK_COMPONENT_SWIZZLE_A);

namespace {

// No eviction yet (freeing an image needs its last submission retired: GPU safety); the caps bound the run instead. FM4's race-prep
// frame goes from 85 images (61 MB, the menus) past 256 in one frame.
constexpr size_t kMaxTextureImages = 4096;
constexpr uint64_t kMaxTextureImageBytes = 2048ull << 20;

struct HostImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t bytes = 0;
    bool initialized = false;  // a copy was recorded (layout SHADER_READ_ONLY_OPTIMAL after it)
    uint64_t sourceHash = 0;   // FM4 Recomp: the hash of the guest bytes the last upload read (FM4_TEXTURE_REUSE)
    bool hashed = false;
    uint64_t lastUseSwap = 0;  // FM4 Recomp: the swap of its last bind (EvictIdleTextures)
    uint64_t reservedHash = 0;  // FM4 Recomp: the hash StagingBytes reserved ring space by; its draw's Bind decides from it
    bool reserved = false;
    // FM4 Recomp (FM4_RESOLVE_RESIDENT, resolve_resident.h): the description it was created for (its guest ranges and shape), and
    // whether it holds the words of resolves recorded into the open submission (adopted; until the next confirmed retire).
    TextureDescription description;
    bool resident = false;
    // FM4 Recomp (FM4_WRITE_WATCH): the watch sequence its content is known good at (armed: its pages were protected before the
    // hash that established the content), the pending arm of the current bind, and StagingBytes' "clean" decision for its draw.
    uint64_t watchSequence = 0, armSequence = 0;
    bool watchArmed = false, armed = false, cleanReserved = false;
    mutable uint64_t cleanEpoch = ~0ull;  // FM4 Recomp (M7 CPU cost): WriteWatch::Epoch at the last WatchClean walk that found it clean
    // FM4 Recomp (M7 CPU cost): at the last GPU-write mark over this image (MarkGuestWritten), whether it was clean (its content
    // known through the watch), and the sequence right after the mark: AdoptResolve's "in step" test without a hash.
    bool cleanAtMark = false;
    uint64_t markSequence = 0;
    // FM4 Recomp (M9, FM4_INTERNAL_RES with the display image at the internal resolution): the scaled twin of a single-level 2D image
    // that resolves write (hiHalves / 2 times its size, the same format; created at the first scaled adoption). hiValid: it holds the
    // same picture as `image` (every write to `image` since was an adoption that also wrote the twin; an upload from guest memory or
    // an adoption without scaled words clears it), so a draw samples it instead (Bind).
    VkImage hiImage = VK_NULL_HANDLE;
    VkDeviceMemory hiMemory = VK_NULL_HANDLE;
    uint64_t hiBytes = 0;
    uint32_t hiHalves = 0;
    uint64_t ledger = 0, hiLedger = 0;  // FM4 Recomp (M9, FM4_DIAG_MEMORY): mem_ledger.h ids
    bool hiInitialized = false, hiValid = false;
    bool blitSource = false;  // FM4 Recomp (M9, live resolution, step 1): created with TRANSFER_SRC (TwinCapable at its creation; Image)
};

// FM4 Recomp (opt-in FM4_WRITE_WATCH=1; ReCarbon
// write_watch.cpp's design, adapted): an image whose guest pages were protected (read-only) before the hash that
// established its content, and on which no write was recorded since (a write faults once, makes the page writable again and
// moves its sequence: texture_cache.h WriteWatch::OnProtectionFault; GPU writes move it explicitly, MarkWritten), is reused at its
// bind WITHOUT hashing its guest bytes. Anything else takes the hash path unchanged. Shared physical memory only (A0000000 /
// C0000000; virtual_memory.h WatchProtect refuses the rest, which keeps hashing). FM4_WRITE_WATCH_VERIFY=1 hashes the clean binds
// anyway and stops naming the texture if the bytes changed without a recorded write.
bool WriteWatchOn()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_WRITE_WATCH");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && runtime::WatchAvailable();
        std::fprintf(stderr, "[texture] write watch %s\n",
                     enabled ? "on (FM4_WRITE_WATCH=1): an unwritten texture is reused without hashing"
                     : asked ? "off: FM4_WRITE_WATCH=1 needs 16 KB host pages and the heaps (ignored)"
                             : "off");
        return enabled;
    }();
    return on;
}
bool WriteWatchVerify()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_WRITE_WATCH_VERIFY");
        const bool enabled = WriteWatchOn() && v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[texture] write watch verify on (FM4_WRITE_WATCH_VERIFY=1): clean binds are hashed and compared\n");
        return enabled;
    }();
    return on;
}

// The watch's host side: protect through the heap (locked, refuses what it cannot watch); unprotect signal-safely.
struct MprotectWatchBackend final : WatchBackend
{
    bool Protect(uint32_t physical, uint32_t size, bool readOnly) override
    {
        return readOnly ? runtime::WatchProtect(physical, size) : runtime::WatchUnprotect(physical, size);
    }
};
MprotectWatchBackend mprotectBackend;
std::atomic<WriteWatch*> activeWatch{nullptr};
std::atomic<uint64_t> watchFaultNs{0};  // FM4 Recomp: time inside the fault hook (clock_gettime is async-signal-safe, POSIX.1-2008)

uint64_t MonotonicNs()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}

// The guest fault hook (guest_memory.h SetGuestWriteFaultHook; signal handler, any thread): a fault in the shared physical views
// on a host page the heap means writable can only be the watch's protection. With no watch (after the texture unit's
// shutdown) the page is just made writable again.
bool WatchFault(uint32_t guest)
{
    if (guest < 0xA0000000u || guest >= 0xE0000000u)
        return false;
    const uint32_t physical = guest & (runtime::kPhysicalSize - 1);
    if (!runtime::PhysicalHostWritable(physical))
        return false;
    if (WriteWatch* w = activeWatch.load(std::memory_order_acquire))
    {
        const uint64_t start = MonotonicNs();
        const bool handled = w->OnProtectionFault(physical);
        watchFaultNs.fetch_add(MonotonicNs() - start, std::memory_order_relaxed);
        return handled;
    }
    return runtime::WatchUnprotect(physical & ~(runtime::kWatchHostPage - 1), runtime::kWatchHostPage);
}
void WatchAccessChanged(uint32_t physical, uint32_t size)
{
    if (WriteWatch* w = activeWatch.load(std::memory_order_acquire))
        w->HostAccessChanged(physical, size);
}
void WatchHostWrite(uint32_t physical, uint32_t size)
{
    if (WriteWatch* w = activeWatch.load(std::memory_order_acquire))
        w->BeforeHostWrite(physical, size);
}

// FM4 Recomp (the default; FM4_TEXTURE_REUSE=0 turns it off for comparisons): a bind whose image already holds the guest bytes it would
// load (the same image key, so the same description, and the same 64-bit hash of every level's guest range) skips the reload.
// The validity is the content itself, so every writer counts (guest CPU stores, resolve read-backs); a range a recorded resolve
// has not yet written back is refused before this (texture_upload.h GuestWriteLog). Measured: ~88% of the race's upload bytes were
// unchanged re-uploads within a swap (FM4_DIAG_TEXTURE_UPLOADS); the A/B: race uploads 4.2-5.2 GB -> 26-68 MB a swap, ~6x faster
// swaps, the static screen pixel-identical but for its animated spinner.
bool TextureReuse()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_TEXTURE_REUSE");
        const bool enabled = !(v != nullptr && v[0] == '0' && v[1] == '\0');
        std::fprintf(stderr, "[texture] reuse %s: %s\n", enabled ? "on" : "off (FM4_TEXTURE_REUSE=0)",
                     enabled ? "an unchanged texture (same guest bytes) is not reloaded at its bind" : "every bind reloads");
        return enabled;
    }();
    return on;
}

const uint8_t* GuestBytes(void* context, uint32_t physical, uint32_t bytes, std::string& error);  // below

// FM4 Recomp: the swap's hashing cost (TextureUnitSummary "hashing"): calls, guest bytes read, wall time. Two clock reads a call.
uint64_t hashCallsSince = 0, hashBytesSince = 0, hashNsSince = 0;

// The hash of every level's guest range (the bytes FillStaging reads); false (with `error`) when a range is not readable.
bool SourceHash(const TextureDescription& d, const uint8_t* base, uint64_t& hash, std::string& error)
{
    const auto start = std::chrono::steady_clock::now();
    struct Account
    {
        std::chrono::steady_clock::time_point start;
        ~Account() { hashNsSince += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); }
    } account{start};
    hashCallsSince++;
    uint64_t h = 0x6A09E667F3BCC908ull;
    for (const MipLevel& l : d.levels)
    {
        const uint8_t* bytes = GuestBytes(const_cast<uint8_t*>(base), l.address, uint32_t(l.extentBytes), error);
        if (bytes == nullptr)
            return false;
        h = ContentHash(bytes, l.extentBytes, h ^ l.address);
        hashBytesSince += l.extentBytes;
    }
    hash = h;
    return true;
}

struct Unit
{
    VulkanDevice dev;  // handles only (not owned)
    TextureDeviceFeatures features;
    uint64_t copyAlignment = 1;
    TextureUnitModel model{kTextureValidity, WriteWatchOn() ? &mprotectBackend : nullptr};
    uint64_t watchSkipsSince = 0, watchSkippedBytesSince = 0, watchFaultsAt = 0, watchProtectsAt = 0, watchProtectNsSince = 0, watchFaultNsAt = 0;  // FM4 Recomp: FM4_WRITE_WATCH
    std::unordered_map<TextureKey, HostImage, TextureKeyHash> images;
    std::unordered_map<ViewKey, VkImageView, ViewKeyHash> views;
    std::unordered_map<ViewKey, VkImageView, ViewKeyHash> hiViews;  // FM4 Recomp (M9): of the scaled twins (HostImage::hiImage)
    uint64_t hiAdoptedSince = 0, hiBindsSince = 0, hiOffsetBindsSince = 0, hiDroppedSince = 0, hiImages = 0;
    std::unordered_map<SamplerDescription, VkSampler, SamplerDescriptionHash> samplers;
    std::unordered_map<uint32_t, VkFormatFeatureFlags> formatFeatures;
    // FM4 Recomp: Xenia's null images (vulkan_texture_cache.cc:485-512): 1x1 R8G8B8A8, cleared to 0, per fetch dimension (1 2D,
    // 2 3D, 3 cube with six layers), created at the first null binding.
    HostImage nullImages[4];
    VkImageView nullViews[4] = {};
    uint64_t imageBytes = 0, uploads = 0, uploadedBytes = 0, uploadsSince = 0, uploadedBytesSince = 0;
    uint64_t reusedSince = 0, reusedBytesSince = 0;  // FM4 Recomp: binds that skipped an unchanged reload (FM4_TEXTURE_REUSE)
    uint64_t swap = 0, evicted = 0, evictedSince = 0;  // FM4 Recomp: EvictIdleTextures
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): guest memory (from the binds), the resident images, and the swap's counts.
    const uint8_t* guestBase = nullptr;
    uint32_t residentImages = 0;
    uint64_t adoptedSince = 0, residentBindsSince = 0, residentDroppedSince = 0;
    // FM4 Recomp (M7): the swap's CPU texture staging (FillStaging calls, staged bytes, wall time), always counted.
    uint64_t fillsSince = 0, fillBytesSince = 0, fillNsSince = 0;
    // FM4 Recomp (M7, FM4_TEXTURE_GPU_UNTILE): the untile pipeline (created at Initialize, destroyed at Shutdown) and the swap's counts.
    struct GpuUntile
    {
        bool on = false;
        const GuestMemoryGpu* guest = nullptr;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        uint64_t storageAlign = 0, maxRange = 0;
        uint32_t maxGroupsX = 0;
    } untile;
    uint64_t gpuLoadsSince = 0, gpuLoadBytesSince = 0, gpuFallbacksSince = 0;
};
std::optional<Unit> unit;

std::string Hex(uint32_t v)
{
    char text[16];
    std::snprintf(text, sizeof text, "%08X", v);
    return text;
}

bool Failed(VkResult r, const char* what, std::string& error)
{
    if (r == VK_SUCCESS)
        return false;
    error = std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")";
    return true;
}

// guest_memory through the view that holds the frames; both ends in one view, every page committed (a read
// of an uncommitted page would be an unnamed guest fault stop).
const uint8_t* GuestBytes(void* context, uint32_t physical, uint32_t bytes, std::string& error)
{
    const uint8_t* base = static_cast<const uint8_t*>(context);
    uint32_t guest = 0, guestEnd = 0;
    if (bytes == 0 || !runtime::PhysicalToGuest(physical, guest) || !runtime::PhysicalToGuest(physical + bytes - 1, guestEnd) || guestEnd - guest != bytes - 1)
    {
        error = "guest range " + Hex(physical) + " + " + std::to_string(bytes) + " bytes is not in one view";
        return nullptr;
    }
    const runtime::Heap* heap = runtime::LookupHeap(guest);
    if (heap == nullptr || !heap->IsCommitted(guest, bytes))
    {
        error = "guest range " + Hex(guest) + " + " + std::to_string(bytes) + " bytes is not committed";
        return nullptr;
    }
    return base + guest;
}

bool FormatSupported(VkFormat format, bool linear, std::string& error)
{
    auto it = unit->formatFeatures.find(uint32_t(format));
    if (it == unit->formatFeatures.end())
    {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(unit->dev.gpu, format, &p);
        it = unit->formatFeatures.emplace(uint32_t(format), p.optimalTilingFeatures).first;
        std::fprintf(stderr, "[gpu] texture format %d: optimal tiling features 0x%X\n", int(format), unsigned(p.optimalTilingFeatures));
    }
    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (linear)
        needed |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((it->second & needed) != needed)
    {
        error = "host format " + std::to_string(int(format)) + " lacks features 0x" + Hex(needed & ~it->second) + " (sampled, transfer dst" +
                (linear ? ", linear filter)" : ")");
        return false;
    }
    return true;
}

// FM4 Recomp (M9, FM4_INTERNAL_RES): whether an image of this description may get a scaled twin (EnsureHi): the display image is at
// the internal resolution, one level, one layer, 2D, a raw 32 bpp colour format (resolve_resident.h ScaledTwinFormat). Such an image is
// created as a blit source too; no other image is ever put in TRANSFER_SRC.
void NameTwinFallback(const TextureDescription& d, const char* why);  // below
bool TwinCapable(const TextureDescription& d)
{
    return runtime::video::scale::OutputScaled() && d.levelCount == 1 && d.layers == 1 && d.dimension != 2 && d.depth == 1 && ScaledTwinFormat(d.fetch.format);
}

HostImage* Image(const TextureEntry& e, std::string& error)
{
    if (auto it = unit->images.find(e.key); it != unit->images.end())
        return &it->second;
    const TextureDescription& d = e.description;
    if (unit->images.size() >= kMaxTextureImages)
    {
        error = "the texture unit holds " + std::to_string(kMaxTextureImages) + " images already (no eviction yet)";
        return nullptr;
    }
    HostImage h;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = d.dimension == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = VkFormat(d.host.vkFormat);
    info.extent = {d.width, d.height, d.depth};
    info.mipLevels = d.levelCount;
    info.arrayLayers = d.layers;  // FM4 Recomp: a cube's six faces
    if (d.layers == 6)
        info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // FM4 Recomp (M9, FM4_INTERNAL_RES): an image a resolve may write verbatim (resolve_resident.h: single level, 2D, 32 bpp 6/7/50/54)
    // is also the blit source that fills its scaled twin (AdoptResolve).
    // FM4 Recomp (M9, live resolution, step 1): remembered per image (blitSource), because TwinCapable follows the current scale: an
    // image created while the display image is 1280x720 must never become a blit source later. One scale a run: the two always agree.
    if (TwinCapable(d))
        info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT, h.blitSource = true;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The device's limits for this format and type (a 3D BC or float image is a per-format property on Metal).
    VkImageFormatProperties limits{};
    if (Failed(vkGetPhysicalDeviceImageFormatProperties(unit->dev.gpu, info.format, info.imageType, info.tiling, info.usage, info.flags, &limits),
               "vkGetPhysicalDeviceImageFormatProperties (texture)", error))
    {
        error += ": host format " + std::to_string(int(info.format)) + (d.dimension == 2 ? " as a 3D image" : " as a 2D image");
        return nullptr;
    }
    if (info.extent.width > limits.maxExtent.width || info.extent.height > limits.maxExtent.height || info.extent.depth > limits.maxExtent.depth ||
        info.mipLevels > limits.maxMipLevels || info.arrayLayers > limits.maxArrayLayers)
    {
        error = "a " + std::to_string(d.width) + "x" + std::to_string(d.height) + "x" + std::to_string(d.depth) + " image of host format " +
                std::to_string(int(info.format)) + " exceeds the device's " + std::to_string(limits.maxExtent.width) + "x" +
                std::to_string(limits.maxExtent.height) + "x" + std::to_string(limits.maxExtent.depth);
        return nullptr;
    }
    if (Failed(vkCreateImage(unit->dev.device, &info, nullptr, &h.image), "vkCreateImage (texture)", error))
        return nullptr;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(unit->dev.device, h.image, &requirements);
    if (unit->imageBytes + requirements.size > kMaxTextureImageBytes)
    {
        vkDestroyImage(unit->dev.device, h.image, nullptr);  // never bound, never used
        error = "texture images would exceed " + std::to_string(kMaxTextureImageBytes >> 20) + " MB (no eviction yet)";
        return nullptr;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(unit->dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (Failed(vkAllocateMemory(unit->dev.device, &allocate, nullptr, &h.memory), "vkAllocateMemory (texture)", error) ||
        Failed(vkBindImageMemory(unit->dev.device, h.image, h.memory, 0), "vkBindImageMemory (texture)", error))
    {
        vkDestroyImage(unit->dev.device, h.image, nullptr);  // never used by a command
        vkFreeMemory(unit->dev.device, h.memory, nullptr);   // null-safe
        return nullptr;
    }
    h.bytes = requirements.size;
    h.description = d;
    unit->imageBytes += h.bytes;
    {
        char label[112];
        std::snprintf(label, sizeof label, "%ux%ux%u format %u at %08X, %u level(s), %u layer(s)", d.width, d.height, d.depth, d.fetch.format, d.base.physical, d.levelCount, d.layers);
        h.ledger = runtime::video::mem::Global().Add(runtime::video::mem::Kind::Texture, h.bytes, label);
    }
    return &unit->images.emplace(e.key, h).first->second;
}

void Barrier(VkCommandBuffer c, VkImage image, uint32_t levels, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
             VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, VK_REMAINING_ARRAY_LAYERS};  // every face of a cube
    OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
    vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// FM4 Recomp (M7, opt-in FM4_TEXTURE_GPU_UNTILE=1; ReCarbon renderer.cpp:3255-3257, :4335-4352):
// a load texture_untile_gpu.h can plan is untiled by a compute dispatch from the imported guest
// memory (guest_memory_vk.h) into the same ring bytes FillStaging would write, then copied as before. The GPU reads guest memory when
// the part executes, so every range it reads is first given to the guest-memory guard (GuardGuestRead below): a later store is held
// until the part completes. Everything else (and the whole unit when off) stages on the CPU as before.
bool GpuUntileAsked()
{
    static const bool asked = [] { const char* v = std::getenv("FM4_TEXTURE_GPU_UNTILE"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    return asked;
}
bool GpuUntileOn() { return unit && unit->untile.on; }

// The adapter: the texture unit's one call into the guest-memory draft's guard (gpu_read_guard.h); its final API is wired here.
// FM4 Recomp (M7, shared guest memory; wired at merge): the vertex path's order (renderer.cpp, gpu_read_guard.h G1): the read is noted
// with the part first, then the range is protected (WatchGuestRange). A range outside physical memory, unaligned, or with a pending
// resolve write-back over its host pages is refused (the CPU path). A refusal after the note leaves it noted: a later store to those
// pages waits for this part for nothing, which is safe.
bool GuardGuestRead(void*, uint32_t physical, uint32_t bytes, uint64_t serial)
{
    GpuReadGuard* const guard = GuestMemoryGpuGuard();
    if (guard == nullptr)
        return false;
    GuardFacts f{true, physical, bytes, runtime::kPhysicalSize, true, false};
    if (ChooseGuardPath(f) != GuardPath::Direct)
        return false;
    const uint64_t hostFirst = physical & ~uint64_t(runtime::kWatchHostPage - 1);
    const uint64_t hostEnd = (uint64_t(physical) + bytes + runtime::kWatchHostPage - 1) & ~uint64_t(runtime::kWatchHostPage - 1);
    if (GuestWritesOverlap(uint32_t(hostFirst), uint32_t(hostEnd - hostFirst)))
        return false;
    if (guard->Hot(physical, bytes))  // FM4 Recomp (M7, FM4_GUARD_HOT): a page stores waited for: the CPU path, nothing noted
        return false;
    (void)guard->NoteReadThen(physical, bytes, serial, [] { return false; });
    uint64_t sequence = 0;
    return WatchGuestRange(physical, bytes, sequence);
}

// Records the planned levels' dispatches into `c`, then the compute-write -> transfer-read barrier over the staging. The bindings are
// computed first: a level that cannot be bound records nothing (false: the caller stages on the CPU into the same placement).
bool RecordGpuUntile(const TextureBindContext& c, std::vector<GpuUntileLevel>& levels, uint64_t offset, uint64_t stagingBytes)
{
    const Unit::GpuUntile& u = unit->untile;
    std::vector<GpuUntileBinding> bindings(levels.size());
    for (size_t i = 0; i < levels.size(); i++)
        if (!GpuUntileBind(levels[i], offset + levels[i].levelOffset, u.guest->bytes, c.allocator->Capacity(), u.storageAlign, u.maxRange, bindings[i]))
            return false;
    OutsideRenderPass(c.commands);  // FM4 Recomp (FM4_PASS_MERGE): a dispatch is recorded outside a render pass
    vkCmdBindPipeline(c.commands, VK_PIPELINE_BIND_POINT_COMPUTE, u.pipeline);
    for (size_t i = 0; i < levels.size(); i++)
    {
        const VkDescriptorBufferInfo buffers[2] = {{u.guest->buffer, bindings[i].guestOffset, bindings[i].guestRange},
                                                   {c.ring, bindings[i].ringOffset, bindings[i].ringRange}};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t b = 0; b < 2; b++)
        {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &buffers[b];
        }
        unit->dev.cmdPushDescriptorSet(c.commands, VK_PIPELINE_BIND_POINT_COMPUTE, u.layout, 0, 2, writes);
        vkCmdPushConstants(c.commands, u.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuUntilePush), &levels[i].push);
        vkCmdDispatch(c.commands, levels[i].groups, 1, 1);
    }
    VkBufferMemoryBarrier written{};
    written.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    written.srcQueueFamilyIndex = written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    written.buffer = c.ring;
    written.offset = offset;
    written.size = GpuUntileStagingBytes(stagingBytes);
    vkCmdPipelineBarrier(c.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &written, 0, nullptr);
    return true;
}

// Guest memory -> the slot's ring -> the image. An image an earlier draw of this (or an earlier) submission
// sampled is overwritten only behind a barrier from the shader stages (write after read, same queue).
bool Upload(const TextureBindContext& c, const uint32_t words[6], const TextureDescription& d, const TextureBinding& binding, const LoadPlan& plan,
            HostImage& image, std::string& error)
{
    const uint64_t alignment = StagingAlignment(1u << d.log2BytesPerBlock, unit->copyAlignment);
    if (alignment == 0)
    {
        error = "no staging alignment for " + std::to_string(1u << d.log2BytesPerBlock) + "-byte blocks and optimalBufferCopyOffsetAlignment " +
                std::to_string(unit->copyAlignment);
        return false;
    }
    // FM4 Recomp (M7, FM4_TEXTURE_GPU_UNTILE): the path is chosen before the placement (a GPU load writes whole dwords).
    std::vector<GpuUntileLevel> levels;
    GpuLoadChoice choice;
    if (GpuUntileOn())
    {
        std::string why;
        const bool planned = PlanGpuUntile(d, plan, unit->untile.maxGroupsX, levels, why);
        choice = ChooseTextureLoad(true, unit->model.ReadsPending(words), planned, d, currentPartSerial, GuardGuestRead, nullptr);
    }
    uint64_t offset = 0;
    if (!PlaceInRing(*c.allocator, choice.gpu ? GpuUntileStagingBytes(plan.stagingBytes) : plan.stagingBytes, alignment, offset))
    {
        error = "the slot's upload ring (" + std::to_string(c.allocator->Used()) + " of " + std::to_string(c.allocator->Capacity()) +
                " bytes used) cannot hold the " + std::to_string(plan.stagingBytes) + " staging bytes of the texture at " + Hex(d.base.physical) +
                " (every bind reloads, texture_upload.h kTextureValidity)";
        return false;
    }
    if (choice.gpu && RecordGpuUntile(c, levels, offset, plan.stagingBytes))
        unit->gpuLoadsSince++, unit->gpuLoadBytesSince += plan.stagingBytes;
    else
    {
        unit->gpuFallbacksSince += GpuUntileOn() ? 1 : 0;
        const auto fillStart = std::chrono::steady_clock::now();
        const bool filled = FillStaging(d, plan, GuestBytes, const_cast<uint8_t*>(c.base), c.ringMapped + offset, error);
        unit->fillsSince++, unit->fillBytesSince += plan.stagingBytes;
        unit->fillNsSince += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - fillStart).count());
        if (!filled)
            return false;
        MaybeDumpTexture(words, d, binding, plan, GuestBytes, const_cast<uint8_t*>(c.base), c.ringMapped + offset);  // FM4_DUMP_TEXTURE (texture_dump.h)
        DiagTextureUpload(d, c.ringMapped + offset, plan.stagingBytes);  // FM4_DIAG_TEXTURE_UPLOADS (diagnostic, diag_texture_uploads.h): nothing unless set
    }
    const VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (image.hiValid)  // FM4 Recomp (M9): the image is reloaded from guest memory (1x bytes): its scaled twin no longer matches
        image.hiValid = false, unit->hiDroppedSince++, NameTwinFallback(d, "reloaded from guest memory, its scaled twin dropped");
    Barrier(c.commands, image.image, d.levelCount, image.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.initialized ? shaders : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    std::vector<VkBufferImageCopy> copies;
    for (const CopyRegion& r : CopyRegions(d, plan, offset))
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = r.bufferOffset;  // rows tightly packed: bufferRowLength = bufferImageHeight = 0
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r.level, 0, r.layers};  // a cube's faces follow each other in the buffer
        copy.imageExtent = {r.width, r.height, r.depth};
        copies.push_back(copy);
    }
    vkCmdCopyBufferToImage(c.commands, c.ring, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    Barrier(c.commands, image.image, d.levelCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, shaders, VK_ACCESS_SHADER_READ_BIT);
    image.initialized = true;
    unit->uploads++, unit->uploadsSince++;
    unit->uploadedBytes += plan.stagingBytes, unit->uploadedBytesSince += plan.stagingBytes;
    return true;
}

VkImageView View(const TextureEntry& e, const HostImage& image, const uint32_t components[4], std::string& error)
{
    ViewKey key{e.key, {components[0], components[1], components[2], components[3]}};
    if (auto it = unit->views.find(key); it != unit->views.end())
        return it->second;
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image.image;
    info.viewType = e.description.dimension == 2   ? VK_IMAGE_VIEW_TYPE_3D
                    : e.description.dimension == 3 ? VK_IMAGE_VIEW_TYPE_CUBE
                                                   : VK_IMAGE_VIEW_TYPE_2D;  // the shader's sampler3D / samplerCube / sampler2D
    info.format = VkFormat(e.description.host.vkFormat);
    info.components = {VkComponentSwizzle(components[0]), VkComponentSwizzle(components[1]), VkComponentSwizzle(components[2]), VkComponentSwizzle(components[3])};
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e.description.levelCount, 0, e.description.layers};
    VkImageView view = VK_NULL_HANDLE;
    if (Failed(vkCreateImageView(unit->dev.device, &info, nullptr, &view), "vkCreateImageView (texture)", error))
        return VK_NULL_HANDLE;
    unit->views.emplace(key, view);
    return view;
}

// FM4 Recomp (M9, FM4_INTERNAL_RES): the same view of the image's scaled twin (one level, 2D).
VkImageView HiView(const TextureEntry& e, const HostImage& image, const uint32_t components[4], std::string& error)
{
    ViewKey key{e.key, {components[0], components[1], components[2], components[3]}};
    if (auto it = unit->hiViews.find(key); it != unit->hiViews.end())
        return it->second;
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image.hiImage;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VkFormat(e.description.host.vkFormat);
    info.components = {VkComponentSwizzle(components[0]), VkComponentSwizzle(components[1]), VkComponentSwizzle(components[2]), VkComponentSwizzle(components[3])};
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (Failed(vkCreateImageView(unit->dev.device, &info, nullptr, &view), "vkCreateImageView (scaled texture)", error))
        return VK_NULL_HANDLE;
    unit->hiViews.emplace(key, view);
    return view;
}

// The scaled twin of `image` (created once; false, with the reason named once per run, when it cannot exist: a size with no whole
// host size, a format the device cannot blit and filter, or the image budget).
bool EnsureHi(HostImage& image, uint32_t halves)
{
    if (image.hiImage != VK_NULL_HANDLE)
        return image.hiHalves == halves;
    const TextureDescription& d = image.description;
    const VkFormat format = VkFormat(d.host.vkFormat);
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(unit->dev.gpu, format, &props);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {runtime::video::scale::HostEdge(d.width, halves), runtime::video::scale::HostEdge(d.height, halves), 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageFormatProperties limits{};
    const char* why = !TwinCapable(d) ? "not a single-level 2D texture of a raw 32 bpp colour format"
                      : !runtime::video::scale::HostExact(d.width, halves) || !runtime::video::scale::HostExact(d.height, halves) ? "its size has no whole host size"
                      : (props.optimalTilingFeatures & needed) != needed ? "its host format cannot be blitted and filtered"
                      : vkGetPhysicalDeviceImageFormatProperties(unit->dev.gpu, format, info.imageType, info.tiling, info.usage, 0, &limits) != VK_SUCCESS ||
                              info.extent.width > limits.maxExtent.width || info.extent.height > limits.maxExtent.height
                          ? "the device has no image of that size"
                          : nullptr;
    VkImage twin = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkMemoryRequirements requirements{};
    if (why == nullptr && vkCreateImage(unit->dev.device, &info, nullptr, &twin) != VK_SUCCESS)
        why = "vkCreateImage failed";
    if (why == nullptr)
    {
        vkGetImageMemoryRequirements(unit->dev.device, twin, &requirements);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = MemoryType(unit->dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (unit->imageBytes + requirements.size > kMaxTextureImageBytes)
            why = "the texture image budget is full";
        else if (vkAllocateMemory(unit->dev.device, &allocate, nullptr, &memory) != VK_SUCCESS || vkBindImageMemory(unit->dev.device, twin, memory, 0) != VK_SUCCESS)
            why = "its memory could not be allocated";
    }
    if (why != nullptr)
    {
        vkDestroyImage(unit->dev.device, twin, nullptr);  // null-safe; never used by a command
        vkFreeMemory(unit->dev.device, memory, nullptr);
        static std::set<std::string> named;
        if (named.insert(why).second)
            std::fprintf(stderr, "[texture] internal resolution: no scaled twin for the %ux%u format %u texture at %08X: %s (first time for this reason)\n",
                         d.width, d.height, d.fetch.format, d.base.physical, why);
        return false;
    }
    image.hiImage = twin, image.hiMemory = memory, image.hiBytes = requirements.size, image.hiHalves = halves;
    {
        char label[112];
        std::snprintf(label, sizeof label, "%ux%u twin of the %ux%u format %u texture at %08X", info.extent.width, info.extent.height, d.width, d.height, d.fetch.format, d.base.physical);
        image.hiLedger = runtime::video::mem::Global().Add(runtime::video::mem::Kind::TextureTwin, image.hiBytes, label);
    }
    unit->imageBytes += image.hiBytes, unit->hiImages++;
    static std::set<std::pair<uint32_t, uint32_t>> named;
    if (named.insert({d.base.physical, d.width << 16 | d.height}).second)
        std::fprintf(stderr, "[texture] internal resolution: scaled twin %ux%u for the %ux%u format %u texture at %08X\n", info.extent.width, info.extent.height,
                     d.width, d.height, d.fetch.format, d.base.physical);
    return true;
}

// The twin and its views go with the image (every submission that used them is confirmed complete: the callers' rule).
void DestroyHi(const TextureKey& key, HostImage& image)
{
    for (auto it = unit->hiViews.begin(); it != unit->hiViews.end();)
        if (it->first.key == key)
        {
            vkDestroyImageView(unit->dev.device, it->second, nullptr);
            it = unit->hiViews.erase(it);
        }
        else
            ++it;
    if (image.hiImage == VK_NULL_HANDLE)
        return;
    vkDestroyImage(unit->dev.device, image.hiImage, nullptr);
    vkFreeMemory(unit->dev.device, image.hiMemory, nullptr);
    runtime::video::mem::Global().Release(image.hiLedger), image.hiLedger = 0;
    unit->imageBytes -= image.hiBytes, unit->hiImages--;
    image.hiImage = VK_NULL_HANDLE, image.hiMemory = VK_NULL_HANDLE, image.hiBytes = 0, image.hiInitialized = image.hiValid = false;
}

// FM4 Recomp (M9, FM4_INTERNAL_RES): why a texture that has a scaled twin is sampled or kept at 1x, once per texture and reason (the
// swap line counts every occurrence): the evidence of which parts of the picture are not at the internal resolution.
void NameTwinFallback(const TextureDescription& d, const char* why)
{
    static std::set<std::tuple<uint32_t, uint32_t, uint32_t, std::string>> named;
    if (named.size() < 256 && named.insert({d.base.physical, d.width, d.height, why}).second)
        std::fprintf(stderr, "[texture] internal resolution: the %ux%u format %u texture at %08X: %s (first time for this texture)\n", d.width, d.height,
                     d.fetch.format, d.base.physical, why);
}

VkSampler Sampler(const SamplerDescription& s, std::string& error)
{
    if (auto it = unit->samplers.find(s); it != unit->samplers.end())
        return it->second;
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VkFilter(s.magFilter);
    info.minFilter = VkFilter(s.minFilter);
    info.mipmapMode = VkSamplerMipmapMode(s.mipmapMode);
    info.addressModeU = VkSamplerAddressMode(s.addressModeU);
    info.addressModeV = VkSamplerAddressMode(s.addressModeV);
    info.addressModeW = VkSamplerAddressMode(s.addressModeW);
    info.mipLodBias = 0.0f;  // LOD bias is the shader's (XE_TEXTURE_<slot>_LOD_BIAS)
    info.anisotropyEnable = s.anisotropyEnable ? VK_TRUE : VK_FALSE;  // X vulkan_texture_cache.cc:798-802 (clamped in texture_upload.cpp)
    info.maxAnisotropy = s.anisotropyEnable ? s.maxAnisotropy : 1.0f;
    info.compareEnable = VK_FALSE;
    info.minLod = s.minLod;
    info.maxLod = s.maxLod;
    info.borderColor = VkBorderColor(s.borderColor);
    info.unnormalizedCoordinates = VK_FALSE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (Failed(vkCreateSampler(unit->dev.device, &info, nullptr, &sampler), "vkCreateSampler", error))
        return VK_NULL_HANDLE;
    unit->samplers.emplace(s, sampler);
    return sampler;
}

// The zero image of a fetch dimension for a null slot (DrawState TextureSlot::null): created and cleared outside the render pass
// on first use, then SHADER_READ_ONLY_OPTIMAL for the run; the sampler the default (nearest, clamp: every texel is 0).
bool NullBinding(const TextureBindContext& c, const TextureSlot& t, VkImageView& view, VkSampler& sampler, std::string& error)
{
    const uint32_t dim = t.fetchDimension == 0 ? 1 : t.fetchDimension;  // a 1D fetch samples the 2D null image at (s, 0)
    if (dim < 1 || dim > 3)
    {
        error = "a null texture for fetch dimension " + std::to_string(dim);
        return false;
    }
    HostImage& h = unit->nullImages[dim];
    if (h.image == VK_NULL_HANDLE)
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = dim == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {1, 1, 1};
        info.mipLevels = 1;
        info.arrayLayers = dim == 3 ? 6 : 1;
        info.flags = dim == 3 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (Failed(vkCreateImage(unit->dev.device, &info, nullptr, &h.image), "vkCreateImage (null texture)", error))
            return false;
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(unit->dev.device, h.image, &requirements);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = MemoryType(unit->dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (Failed(vkAllocateMemory(unit->dev.device, &allocate, nullptr, &h.memory), "vkAllocateMemory (null texture)", error) ||
            Failed(vkBindImageMemory(unit->dev.device, h.image, h.memory, 0), "vkBindImageMemory (null texture)", error))
            return false;  // the handles are destroyed at shutdown (never used by a command)
        h.bytes = requirements.size;
        VkImageViewCreateInfo v{};
        v.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        v.image = h.image;
        v.viewType = dim == 2 ? VK_IMAGE_VIEW_TYPE_3D : dim == 3 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        v.format = info.format;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, info.arrayLayers};
        if (Failed(vkCreateImageView(unit->dev.device, &v, nullptr, &unit->nullViews[dim]), "vkCreateImageView (null texture)", error))
            return false;
    }
    if (!h.initialized)
    {
        const VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        Barrier(c.commands, h.image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(c.commands, h.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        Barrier(c.commands, h.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, shaders, VK_ACCESS_SHADER_READ_BIT);
        h.initialized = true;
        std::fprintf(stderr, "[texture] null texture (fetch dimension %u) created and cleared to 0 for fetch constants that are not textures\n", dim);
    }
    view = unit->nullViews[dim];
    sampler = Sampler(SamplerDescription{}, error);
    return sampler != VK_NULL_HANDLE;
}

// FM4 Recomp (FM4_WRITE_WATCH): protect first, then take the sequence, then hash: a write before the protection is in the hash, a
// write after it faults and moves a sequence above the one recorded. The arm is committed once the hash decided the content.
void ArmWatch(HostImage& image)
{
    if (!WriteWatchOn())
        return;
    WriteWatch& w = *unit->model.ProtectingWatch();
    const TextureDescription& d = image.description;
    const uint64_t start = MonotonicNs();
    // The sequence is read before the protection (a store that faults between the two bumps above it and is seen as a write).
    image.armSequence = w.Current();
    image.armed = w.Watch(d.base.physical, d.base.size) && w.Watch(d.mips.physical, d.mips.size);
    unit->watchProtectNsSince += MonotonicNs() - start;
}
void CommitWatch(HostImage& image)
{
    image.watchSequence = image.armSequence, image.watchArmed = image.armed;
    image.armed = false;
    image.cleanEpoch = ~0ull, image.cleanAtMark = false;
}
// FM4 Recomp (M7 CPU cost): the image's content may be established by a hash or by the write-back of its resident resolves
// (GuestWritesRetired arms and commits the watch instead of hashing), so `hashed` is not required here.
bool WatchClean(const HostImage& image)
{
    if (!WriteWatchOn() || !image.watchArmed || !image.initialized)
        return false;
    const WriteWatch& w = *unit->model.ProtectingWatch();
    const uint64_t epoch = w.Epoch();  // every bump moves it: an unchanged epoch means no page sequence moved since the last clean walk
    if (image.cleanEpoch == epoch)
        return true;
    const TextureDescription& d = image.description;
    const bool clean = w.WriteSequence(d.base.physical, d.base.size) <= image.watchSequence && w.WriteSequence(d.mips.physical, d.mips.size) <= image.watchSequence;
    if (clean)
        image.cleanEpoch = epoch;
    return clean;
}

bool Bind(const TextureBindContext& c, const TextureSlot& t, VkImageView& view, VkSampler& sampler, std::string& error)
{
    if (!unit || c.allocator == nullptr || c.ringMapped == nullptr || c.base == nullptr)
    {
        error = "the texture unit is not initialized or the bind context is incomplete";
        return false;
    }
    if (t.null)
    {
        static std::set<std::pair<uint32_t, uint32_t>> named;  // once per (slot, type)
        if (named.insert({t.slot, t.type}).second)
            std::fprintf(stderr, "[texture] slot %u: fetch constant type %u (%08X %08X ...) is not a texture: the zero null texture is bound, as Xenia\n",
                         t.slot, t.type, t.words[0], t.words[1]);
        return NullBinding(c, t, view, sampler, error);
    }
    // The resolved filters (the instruction's unless 3; anisotropy the instruction's unless 7, DrawState).
    const ShaderFetch shader{t.magFilter, t.minFilter, t.mipFilter, t.anisoFilter, t.fetchDimension};
    unit->guestBase = c.base;
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): a range a recorded resolve has not written back is refused below unless the image holds it.
    const PreparedTexture& p = unit->model.Prepare(t.words, shader, t.gamma, unit->features, ResolveResident());
    if (p.entry == nullptr)
    {
        error = p.error;
        return false;
    }
    if (p.binding.biasedMask != t.biasedMask)
    {
        error = "the binding's biased components (" + std::to_string(p.binding.biasedMask) + ") disagree with DrawState's (" +
                std::to_string(t.biasedMask) + "), which chose the shader define";
        return false;
    }
    const TextureDescription& d = p.entry->description;
    const SamplerDescription& s = p.binding.sampler;
    const bool linear = s.magFilter == vk::kFilterLinear || s.minFilter == vk::kFilterLinear || s.mipmapMode == vk::kMipmapModeLinear;
    if (!FormatSupported(VkFormat(d.host.vkFormat), linear, error))
        return false;
    HostImage* image = Image(*p.entry, error);
    if (image == nullptr)
        return false;
    image->lastUseSwap = unit->swap;
    if (ResolveResident() && !image->resident && unit->model.ReadsPending(t.words))
    {
        error = "the texture at " + Hex(d.base.physical) + ": its guest range was written by a resolve recorded into the open submission that is not "
                "confirmed complete, and its host image does not hold the resolve's words; the renderer must retire it first";
        return false;
    }
    uint64_t sourceHash = 0;
    const bool reuse = TextureReuse();
    if (image->resident)  // FM4 Recomp (FM4_RESOLVE_RESIDENT): the image holds the resolves' words; guest memory is not read
    {
        image->reserved = false;
        unit->residentBindsSince++;
        if (!unit->model.Loaded(*p.entry, p.plan))
        {
            error = "the write watch refused the texture at " + Hex(d.base.physical);
            return false;
        }
    }
    // FM4 Recomp (FM4_WRITE_WATCH): unwritten since the hash that established its content: reused without hashing (StagingBytes'
    // decision for this draw when it made one, so the ring holds what it reserved).
    const bool clean = !image->resident && reuse && p.load && image->initialized && (image->cleanReserved || WatchClean(*image));
    image->cleanReserved = false;
    if (clean)
    {
        image->reserved = false;
        // A write recorded after this draw's reservation decided "clean" (cleanReserved; a guest thread racing the draw): the next
        // bind sees it and reloads, so only a change with no recorded write is a miss.
        if (WriteWatchVerify() && image->hashed && !WatchClean(*image))
            diag::ReportReservationRace(d.base.physical);
        else if (WriteWatchVerify() && image->hashed)
        {
            uint64_t check = 0;
            if (SourceHash(d, c.base, check, error) && check != image->sourceHash)
            {
                diag::ReportStalePages(*unit->model.ProtectingWatch(), c.base, d.base.physical, d.base.size, image->watchSequence,
                                       runtime::PhysicalHostWritable);
                diag::ReportStalePages(*unit->model.ProtectingWatch(), c.base, d.mips.physical, d.mips.size, image->watchSequence,
                                       runtime::PhysicalHostWritable);
                Stop(StopCode::Unimplemented, "write watch verify: the texture at %08X (%s %ux%u, %u bytes) changed without a recorded write (hash %016llX, "
                                              "image %016llX, armed at sequence %llu, now %llu)",
                     d.base.physical, d.info.name, d.width, d.height, d.base.size, (unsigned long long)check, (unsigned long long)image->sourceHash,
                     (unsigned long long)image->watchSequence, (unsigned long long)unit->model.ProtectingWatch()->Current());
            }
        }
        unit->watchSkipsSince++, unit->watchSkippedBytesSince += d.base.size + d.mips.size;
    }
    else if (image->resident)
        ;
    else if (reuse && p.load && image->reserved)  // the hash this draw's ring reservation used (StagingBytes armed it first)
        sourceHash = image->reservedHash;
    else if (reuse && p.load)
    {
        ArmWatch(*image);
        if (!SourceHash(d, c.base, sourceHash, error))
            return false;
    }
    image->reserved = false;
    if (clean)
    {
        unit->reusedSince++, unit->reusedBytesSince += p.plan.stagingBytes;  // unchanged: the image already holds these bytes
        if (!unit->model.Loaded(*p.entry, p.plan))
        {
            error = "the write watch refused the texture at " + Hex(d.base.physical);
            return false;
        }
    }
    else if (image->resident)
        ;
    else if (reuse && p.load && image->initialized && image->hashed && image->sourceHash == sourceHash)
    {
        CommitWatch(*image);
        unit->reusedSince++, unit->reusedBytesSince += p.plan.stagingBytes;  // unchanged: the image already holds these bytes
        if (!unit->model.Loaded(*p.entry, p.plan))
        {
            error = "the write watch refused the texture at " + Hex(d.base.physical);
            return false;
        }
    }
    else if (p.load)
    {
        if (!Upload(c, t.words, d, p.binding, p.plan, *image, error))
            return false;
        image->sourceHash = sourceHash, image->hashed = reuse;
        if (reuse)
            CommitWatch(*image);
        if (!unit->model.Loaded(*p.entry, p.plan))
        {
            error = "the write watch refused the texture at " + Hex(d.base.physical);
            return false;
        }
    }
    else if (!image->initialized)
    {
        error = "the texture at " + Hex(d.base.physical) + " is valid in the cache but its host image was never loaded";
        return false;
    }
    view = View(*p.entry, *image, p.binding.components, error);
    if (view == VK_NULL_HANDLE)
        return false;
    // FM4 Recomp (M9, FM4_INTERNAL_RES): the scaled twin while it holds the same picture, unless the shader moves this slot's
    // coordinates by texel offsets (they are divided by textureSize, which must be the guest's size: the 1x image, counted).
    if (image->hiValid && t.fetchOffsets)
        unit->hiOffsetBindsSince++, NameTwinFallback(d, "sampled at 1x by a fetch with texel offsets");
    else if (image->hiValid)
    {
        view = HiView(*p.entry, *image, p.binding.components, error);
        if (view == VK_NULL_HANDLE)
            return false;
        unit->hiBindsSince++;
    }
    sampler = Sampler(s, error);
    return sampler != VK_NULL_HANDLE;
}

// FM4 Recomp (M7, opt-in FM4_TEXTURE_PREPARE_ONCE=1): StagingBytes and
// ReadsPendingWrite ask Prepare with the slot's own anisotropy, as Bind does, so the three share one memo entry instead of preparing
// each texture twice a draw. Anisotropy only reaches the sampler (texture_info.cpp:391), never the entry or the plan
// (test_texture_vk.cpp TestPrepareAnisotropy).
uint32_t PrepareAniso(const TextureSlot& t)
{
    static const bool once = [] {
        const char* v = std::getenv("FM4_TEXTURE_PREPARE_ONCE");
        const bool on = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (on)
            std::fprintf(stderr, "[texture] prepare once on (FM4_TEXTURE_PREPARE_ONCE=1): staging and pending checks share Bind's prepared texture\n");
        return on;
    }();
    return once ? t.anisoFilter : 7u;
}

uint64_t StagingBytes(const TextureSlot& t, const uint8_t* base)
{
    if (!unit || t.null)
        return 0;
    const ShaderFetch shader{t.magFilter, t.minFilter, t.mipFilter, PrepareAniso(t), t.fetchDimension};
    const PreparedTexture& p = unit->model.Prepare(t.words, shader, t.gamma, unit->features, ResolveResident());
    if (p.entry == nullptr)
        return 0;
    const auto it = unit->images.find(p.entry->key);  // FM4 Recomp (M7 CPU cost): one lookup (it was two of the same key per texture per draw)
    if (it != unit->images.end() && it->second.resident)
        return 0;  // FM4 Recomp (FM4_RESOLVE_RESIDENT): bound without an upload
    const uint64_t alignment = StagingAlignment(1u << p.entry->description.log2BytesPerBlock, unit->copyAlignment);
    // The worst-case padding PlaceInRing can add; FM4_TEXTURE_GPU_UNTILE: a GPU load's staging rounded up to a dword.
    const uint64_t full = (GpuUntileOn() ? GpuUntileStagingBytes(p.plan.stagingBytes) : p.plan.stagingBytes) + alignment;
    if (!TextureReuse() || !p.load || base == nullptr || it == unit->images.end() || !it->second.initialized)
        return full;
    HostImage& image = it->second;
    if (WatchClean(image))  // FM4 Recomp (FM4_WRITE_WATCH): unwritten: no hash, no ring bytes; its Bind reuses it
    {
        image.cleanReserved = true, image.reserved = false;
        return 0;
    }
    if (!image.hashed)  // content known only through the watch, and written since: Bind hashes once and uploads
        return full;
    std::string ignored;  // an unreadable range reserves the full size; Bind names the error
    uint64_t hash = 0;
    ArmWatch(image);
    const bool read = SourceHash(p.entry->description, base, hash, ignored);
    image.reservedHash = hash, image.reserved = read;
    return ReservedStagingBytes(true, image.hashed, read, hash, image.sourceHash, full);
}

bool ReadsPendingWrite(const TextureSlot& t)
{
    if (!unit || t.null || !unit->model.ReadsPending(t.words))
        return false;
    if (!ResolveResident())
        return true;
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): an image that holds the resolves' words reads nothing pending.
    const ShaderFetch shader{t.magFilter, t.minFilter, t.mipFilter, PrepareAniso(t), t.fetchDimension};
    const PreparedTexture& p = unit->model.Prepare(t.words, shader, t.gamma, unit->features, true);
    const auto it = p.entry == nullptr ? unit->images.end() : unit->images.find(p.entry->key);
    return it == unit->images.end() || !it->second.resident;
}

bool RangesOverlap(uint32_t a, uint64_t aBytes, uint32_t b, uint64_t bBytes)
{
    return aBytes != 0 && bBytes != 0 && uint64_t(a) < uint64_t(b) + bBytes && uint64_t(b) < uint64_t(a) + aBytes;
}

// FM4 Recomp (FM4_RESOLVE_RESIDENT, resolve_resident.h; render_hooks.h TextureHooks::adoptResolve). Only images over the
// resolve's range change: one that takes its words verbatim (ResidentTextureMatches) and is in step with guest memory (resident
// already, or its reuse hash equals the guest bytes with no earlier resolve of this submission pending over it) gets them by a
// GPU copy; every other one stops being resident, so a draw that samples it waits for the write-back as before.
// FM4 Recomp (opt-in FM4_RESOLVE_RESIDENT_MIPS=1): a mipped image takes a resolve into one of its unpacked levels.
bool ResidentMips()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_RESIDENT_MIPS");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[texture] resident mip levels on (FM4_RESOLVE_RESIDENT_MIPS=1): a resolve into an unpacked level is copied there\n");
        return enabled;
    }();
    return on;
}

// FM4 Recomp (M7, resident resolves; opt-in FM4_RESOLVE_RESIDENT_COVER=1, needs FM4_RESOLVE_RESIDENT=1):
// a resolve is taken at its pitch aligned to 32 texels, clipped to a texture (level) it reaches past inside the padded storage, and a
// resolve that replaces every texel of a single-level texture makes it in step even over an earlier pending resolve
// (resolve_resident.h ResidentTextureMatches `cover`, ResidentCoversInStep). The GPU work is unchanged: the same copy and barriers.
bool ResidentCover()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_RESIDENT_COVER");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[texture] resident cover on (FM4_RESOLVE_RESIDENT_COVER=1): aligned resolve pitches, rectangles clipped to padded levels, "
                                 "covering resolves in step over earlier pending ones\n");
        return enabled;
    }();
    return on;
}

uint32_t AdoptResolve(VkCommandBuffer c, const ResidentResolve& r, VkImage words, VkImage hiWords)
{
    if (!unit || r.extentLength == 0)
        return 0;
    const bool cover = ResidentCover();  // FM4 Recomp (M7, resident resolves)
    uint32_t adopted = 0;
    const VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    for (auto& [key, image] : unit->images)
    {
        const TextureDescription& d = image.description;
        if (!RangesOverlap(d.base.physical, d.base.size, r.extentStart, r.extentLength) && !RangesOverlap(d.mips.physical, d.mips.size, r.extentStart, r.extentLength))
            continue;
        const ResidentTextureShape shape{d.base.physical, d.width, d.height, d.fetch.pitchTexels, d.fetch.format, d.fetch.endian, d.levelCount,
                                         d.dimension, d.depth, d.fetch.tiled, d.fetch.stacked, d.fetch.packedMips};
        std::string why = !image.initialized ? "its host image was never loaded" : d.layers != 1 ? "a cube" : "";
        uint32_t x = 0, y = 0;
        // FM4 Recomp: a mipped image takes a resolve into one of its unpacked levels (resolve_resident.h ResidentLevelFor).
        std::vector<ResidentLevel> levels;
        for (const MipLevel& l : ResidentMips() ? d.levels : std::vector<MipLevel>{})
            levels.push_back({l.address, l.widthTexels, l.heightTexels, l.pitchBlocks * (d.info.blockWidth != 0 ? d.info.blockWidth : 1), l.packed});
        // FM4 Recomp (M7, resident resolves): `extent` is what lands inside the level (r.width x r.height unless `cover` clipped it).
        ResidentCopy extent{r.width, r.height};
        const int level = image.initialized && d.layers == 1 ? ResidentLevelFor(shape, levels, r, &why, &x, &y, cover, &extent) : -1;
        bool take = level >= 0 && words != VK_NULL_HANDLE;
        if (take && !image.resident)
        {
            uint64_t hash = 0;
            std::string ignored;
            // In step: its reuse hash equals the guest bytes, or (M7 CPU cost: an image whose content the write watch established, e.g.
            // a resident image re-armed at the retire) it was clean when this resolve's range was marked and nothing wrote it since.
            const bool noEarlierPending = !unit->model.OverlapsEarlierPending(d.base.physical, uint32_t(d.base.size)) &&
                                          !unit->model.OverlapsEarlierPending(d.mips.physical, uint32_t(d.mips.size));
            // FM4 Recomp (M7 waits): a resolve that covers the whole single-level texture replaces every texel, so the image is in
            // step by construction whatever it held (FM4's bloom chain resolves 160x96, 64x32 and 32x32 textures at one base in turn:
            // each lands over the previous one's bytes, so no hash or watch could call the image clean).
            // FM4 Recomp (M7, resident resolves): with `cover` also over an earlier pending resolve (resolve_resident.h ResidentCoversInStep).
            bool inStep = ResidentCoversInStep(level, d.levelCount, x, y, extent, d.width, d.height, noEarlierPending, cover);
            if (!inStep && TextureReuse() && unit->guestBase != nullptr && noEarlierPending)
            {
                if (image.hashed)
                    inStep = SourceHash(d, unit->guestBase, hash, ignored) && hash == image.sourceHash;
                else if (image.cleanAtMark && WriteWatchOn())
                {
                    const WriteWatch& w = *unit->model.ProtectingWatch();
                    inStep = w.WriteSequence(d.base.physical, d.base.size) <= image.markSequence && w.WriteSequence(d.mips.physical, d.mips.size) <= image.markSequence;
                }
            }
            take = inStep;
            why = "its host image is not in step with guest memory";
        }
        static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool>> named;  // once per (image, resolve base, outcome)
        if (named.insert({d.base.physical, d.fetch.format, d.width, d.height, r.textureBase, take}).second)
            std::fprintf(stderr, "[texture] resident resolve %s the %ux%u format %u texture at %08X (resolve format %u at %08X, %ux%u at texel %u,%u of level %d)%s%s\n",
                         take ? "writes" : "does not write", d.width, d.height, d.fetch.format, d.base.physical, r.destFormat, r.textureBase, r.width, r.height, x, y,
                         std::max(level, 0), take ? "" : ": ", take ? "" : why.c_str());
        if (!take)
        {
            if (image.resident)
                image.resident = false, unit->residentImages--, unit->residentDroppedSince++;
            if (image.hiValid)  // FM4 Recomp (M9): the image will be reloaded from guest memory
                image.hiValid = false, unit->hiDroppedSince++, NameTwinFallback(d, ("a resolve over it was not taken (" + why + "), its scaled twin dropped").c_str());
            continue;
        }
        // FM4 Recomp (M7, resident resolves): a rectangle wholly in the level's padding copies nothing (a zero extent is not a valid
        // VkImageCopy, VUID-VkImageCopy-extent-06668): the image keeps its texels, which the resolve does not change. Otherwise the
        // clipped extent: src (0, 0) is the rectangle's corner in `words` and clipping only cuts its right and bottom.
        if (extent.width != 0 && extent.height != 0)
        {
            Barrier(c, image.image, d.levelCount, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, shaders, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level), 0, 1};
            copy.dstOffset = {int32_t(x), int32_t(y), 0};
            copy.extent = {extent.width, extent.height, 1};
            vkCmdCopyImage(c, words, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(c, image.image, d.levelCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, shaders, VK_ACCESS_SHADER_READ_BIT);
        }
        // FM4 Recomp (M9, FM4_INTERNAL_RES): the resolve's words per host pixel into the image's scaled twin, at the scaled corner. The
        // whole rectangle of level 0 of a single-level image only, with whole host edges; a twin that did not hold the image's picture
        // is first filled from it (a LINEAR blit of the 1x image, which the copy above just completed), so texels no scaled resolve
        // has written yet are the 1x picture enlarged, never undefined. Anything else leaves the twin out of use.
        const bool hi = hiWords != VK_NULL_HANDLE && r.hiHalves > runtime::video::scale::kHalves1x && level == 0 && d.levelCount == 1 &&
                        extent.width == r.width && extent.height == r.height && extent.width != 0 && extent.height != 0 &&
                        runtime::video::scale::HostExact(x, r.hiHalves) && runtime::video::scale::HostExact(y, r.hiHalves) &&
                        runtime::video::scale::HostExact(r.width, r.hiHalves) && runtime::video::scale::HostExact(r.height, r.hiHalves) &&
                        TwinCapable(d) && image.blitSource && EnsureHi(image, r.hiHalves);  // blitSource: FM4 Recomp (M9, live resolution, step 1)
        if (hi)
        {
            using runtime::video::scale::HostEdge;
            const uint32_t h = r.hiHalves;
            Barrier(c, image.hiImage, 1, image.hiInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.hiInitialized ? shaders : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            if (!image.hiValid)
            {
                Barrier(c, image.image, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shaders, 0,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                VkImageBlit blit{};
                blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {int32_t(d.width), int32_t(d.height), 1};
                blit.dstOffsets[1] = {int32_t(HostEdge(d.width, h)), int32_t(HostEdge(d.height, h)), 1};
                vkCmdBlitImage(c, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.hiImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                Barrier(c, image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, shaders, VK_ACCESS_SHADER_READ_BIT);
            }
            VkImageCopy copy{};
            copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstOffset = {int32_t(HostEdge(x, h)), int32_t(HostEdge(y, h)), 0};
            copy.extent = {HostEdge(r.width, h), HostEdge(r.height, h), 1};
            vkCmdCopyImage(c, hiWords, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.hiImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(c, image.hiImage, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, shaders, VK_ACCESS_SHADER_READ_BIT);
            image.hiInitialized = image.hiValid = true;
            unit->hiAdoptedSince++;
        }
        else if (image.hiValid && extent.width != 0 && extent.height != 0)  // 1x words only: the twin no longer matches
            image.hiValid = false, unit->hiDroppedSince++, NameTwinFallback(d, "a resolve wrote it with 1x words only, its scaled twin dropped");
        if (!image.resident)
            image.resident = true, unit->residentImages++;
        adopted++;
    }
    unit->adoptedSince += adopted;
    return adopted;
}

uint64_t PendingWriteSerial(const TextureSlot& t)  // FM4_PARTIAL_RETIRE
{
    if (!ReadsPendingWrite(t))
        return 0;
    return unit->model.ReadsPendingSerial(t.words);
}
const TextureHooks hooks{Bind, StagingBytes, ReadsPendingWrite, PendingWriteSerial, AdoptResolve};

// FM4 Recomp (M7, FM4_TEXTURE_GPU_UNTILE): the untile pipeline, once, or one line naming why every load stays on the CPU. Objects that
// fail half-way were never used by a command and are destroyed here.
void InitializeGpuUntile(GlslangCompiler* compiler, const VkPhysicalDeviceLimits& limits)
{
    if (!GpuUntileAsked())
        return;
    Unit::GpuUntile& u = unit->untile;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(unit->dev.gpu, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families);
    vkGetPhysicalDeviceQueueFamilyProperties(unit->dev.gpu, &families, family.data());
    const GuestMemoryGpu* guest = GuestMemoryGpuView();
    const char* why = guest == nullptr || guest->buffer == VK_NULL_HANDLE ? "no guest memory view (FM4_GUEST_MEMORY_GPU=1 and its import)"
                      : !unit->dev.pushDescriptors || unit->dev.cmdPushDescriptorSet == nullptr || unit->dev.maxPushDescriptors < 2
                          ? "no push descriptors (FM4_PUSH_DESCRIPTORS=1 and VK_KHR_push_descriptor)"
                      : unit->dev.queueFamily >= families || !(family[unit->dev.queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "the queue has no compute"
                      : std::getenv("FM4_DUMP_TEXTURE") != nullptr || DiagTextureUploadsEnabled()
                          ? "FM4_DUMP_TEXTURE / FM4_DIAG_TEXTURE_UPLOADS read the staged bytes on the CPU"
                      : compiler == nullptr ? "no shader compiler"
                                            : nullptr;
    std::string error, warnings;
    std::vector<uint32_t> spirv;
    if (why == nullptr && !compiler->CompileCompute(UntileGpuGlsl(), spirv, warnings, error))
        why = "the untile shader did not compile";
    VkShaderModule module = VK_NULL_HANDLE;
    if (why == nullptr)
    {
        VkDescriptorSetLayoutBinding bindings[2] = {};
        for (uint32_t b = 0; b < 2; b++)
            bindings[b] = {b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        set.bindingCount = 2;
        set.pBindings = bindings;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuUntilePush)};
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &u.setLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        VkShaderModuleCreateInfo shader{};
        shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader.codeSize = spirv.size() * 4;
        shader.pCode = spirv.data();
        VkComputePipelineCreateInfo pipeline{};
        pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.pName = "main";
        if (Failed(vkCreateDescriptorSetLayout(unit->dev.device, &set, nullptr, &u.setLayout), "vkCreateDescriptorSetLayout (untile)", error) ||
            Failed(vkCreatePipelineLayout(unit->dev.device, &layout, nullptr, &u.layout), "vkCreatePipelineLayout (untile)", error) ||
            Failed(vkCreateShaderModule(unit->dev.device, &shader, nullptr, &module), "vkCreateShaderModule (untile)", error))
            why = "a Vulkan object could not be created";
        else
        {
            pipeline.stage.module = module;
            pipeline.layout = u.layout;
            if (Failed(vkCreateComputePipelines(unit->dev.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &u.pipeline), "vkCreateComputePipelines (untile)", error))
                why = "the compute pipeline could not be created";
        }
        vkDestroyShaderModule(unit->dev.device, module, nullptr);  // null-safe; not needed once the pipeline exists
    }
    if (why != nullptr)
    {
        vkDestroyPipeline(unit->dev.device, u.pipeline, nullptr);  // null-safe; never used by a command
        vkDestroyPipelineLayout(unit->dev.device, u.layout, nullptr);
        vkDestroyDescriptorSetLayout(unit->dev.device, u.setLayout, nullptr);
        u = Unit::GpuUntile{};
        std::fprintf(stderr, "[texture] FM4_TEXTURE_GPU_UNTILE=1 ignored: %s%s%s; every texture is staged on the CPU\n", why, error.empty() ? "" : ": ",
                     error.c_str());
        return;
    }
    u.guest = guest;
    u.storageAlign = limits.minStorageBufferOffsetAlignment;
    u.maxRange = limits.maxStorageBufferRange;
    u.maxGroupsX = limits.maxComputeWorkGroupCount[0];
    u.on = true;
    std::fprintf(stderr, "[texture] GPU untile on (FM4_TEXTURE_GPU_UNTILE=1): 2D tiled and linear loads without conversion are untiled by a compute "
                         "dispatch from guest memory (%llu MB view) when the guard takes their ranges; the rest are staged on the CPU\n",
                 (unsigned long long)(guest->bytes >> 20));
}

}  // namespace

const TextureHooks* InitializeTextureUnit(const VulkanDevice& device, GlslangCompiler* compiler)
{
    unit.emplace();
    unit->dev = device;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(device.gpu, &properties);
    unit->features = {device.textureCompressionBC, device.imageViewFormatSwizzle, device.samplerAnisotropy,
                      std::min(16.0f, std::max(1.0f, properties.limits.maxSamplerAnisotropy))};  // X vulkan_texture_cache.cc:2591-2596
    unit->copyAlignment = properties.limits.optimalBufferCopyOffsetAlignment;
    std::fprintf(stderr, "[gpu] texture unit: textureCompressionBC %d, imageViewFormatSwizzle %d, samplerAnisotropy %d (max %g), optimalBufferCopyOffsetAlignment %llu, validity %s\n",
                 int(device.textureCompressionBC), int(device.imageViewFormatSwizzle), int(device.samplerAnisotropy), double(unit->features.maxAnisotropy),
                 (unsigned long long)unit->copyAlignment,
                 kTextureValidity == TextureValidity::ReloadEveryBind ? "reload at every bind" : "write watch");
    if (WriteWatchOn())  // FM4 Recomp (FM4_WRITE_WATCH): the hook and the heaps' observers
    {
        activeWatch.store(unit->model.ProtectingWatch(), std::memory_order_release);
        runtime::SetWatchObservers(WatchAccessChanged, WatchHostWrite);
        runtime::SetGuestWriteFaultHook(WatchFault);
    }
    InitializeGpuUntile(compiler, properties.limits);  // FM4 Recomp (M7, FM4_TEXTURE_GPU_UNTILE)
    return &hooks;
}

void ShutdownTextureUnit()
{
    if (!unit)
        return;
    // FM4 Recomp (FM4_WRITE_WATCH): the observers go and the hook stays: a guest store on a page still protected for the watch is
    // made writable without bookkeeping (WatchFault with no watch), so a late write never stops the run.
    runtime::SetWatchObservers(nullptr, nullptr);
    activeWatch.store(nullptr, std::memory_order_release);
    for (auto& [key, view] : unit->views)
        vkDestroyImageView(unit->dev.device, view, nullptr);
    for (auto& [key, view] : unit->hiViews)  // FM4 Recomp (M9): the scaled twins' views and images
        vkDestroyImageView(unit->dev.device, view, nullptr);
    unit->hiViews.clear();
    for (auto& [key, image] : unit->images)
    {
        vkDestroyImage(unit->dev.device, image.hiImage, nullptr);  // null-safe
        vkFreeMemory(unit->dev.device, image.hiMemory, nullptr);
    }
    for (auto& [key, sampler] : unit->samplers)
        vkDestroySampler(unit->dev.device, sampler, nullptr);
    for (auto& [key, image] : unit->images)
    {
        vkDestroyImage(unit->dev.device, image.image, nullptr);
        vkFreeMemory(unit->dev.device, image.memory, nullptr);
    }
    for (uint32_t i = 0; i < 4; i++)  // null-safe
    {
        vkDestroyImageView(unit->dev.device, unit->nullViews[i], nullptr);
        vkDestroyImage(unit->dev.device, unit->nullImages[i].image, nullptr);
        vkFreeMemory(unit->dev.device, unit->nullImages[i].memory, nullptr);
    }
    // FM4 Recomp (M7, FM4_TEXTURE_GPU_UNTILE): null-safe; every submission that dispatched it is confirmed complete (above).
    vkDestroyPipeline(unit->dev.device, unit->untile.pipeline, nullptr);
    vkDestroyPipelineLayout(unit->dev.device, unit->untile.layout, nullptr);
    vkDestroyDescriptorSetLayout(unit->dev.device, unit->untile.setLayout, nullptr);
    unit.reset();
}

// FM4 Recomp (observed: 4,095 images, 543 MB, during a drive): above kEvictAbove images or
// kEvictAboveBytes, the images not bound in the current or the previous swap are destroyed (least recently bound first) down
// to kEvictTo, with their views. Called by the renderer only after the swap's RetireSlot (every submission confirmed complete,
// so no command buffer can use them: GPU safety); a later bind recreates and reloads the image.
constexpr size_t kEvictAbove = 3072, kEvictTo = 2048;
constexpr uint64_t kEvictAboveBytes = 1536ull << 20;
size_t MarkTexturesStale()
{
    if (!unit)
        return 0;
    size_t n = 0;
    for (auto& [key, image] : unit->images)
        if (!image.resident)
        {
            image.hashed = false, image.sourceHash = 0, image.watchArmed = false, image.cleanEpoch = ~0ull, image.cleanAtMark = false;
            n++;
        }
    return n;
}

void EvictIdleTextures()
{
    if (!unit)
        return;
    unit->swap++;
    if (unit->images.size() <= kEvictAbove && unit->imageBytes <= kEvictAboveBytes)
        return;
    std::vector<std::pair<uint64_t, TextureKey>> idle;
    for (const auto& [key, image] : unit->images)
        if (image.lastUseSwap + 2 <= unit->swap && !image.resident)  // resident: never (cleared at every retire before this)
            idle.push_back({image.lastUseSwap, key});
    std::sort(idle.begin(), idle.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [use, key] : idle)
    {
        if (unit->images.size() <= kEvictTo && unit->imageBytes <= kEvictAboveBytes / 2)
            break;
        for (auto it = unit->views.begin(); it != unit->views.end();)
            if (it->first.key == key)
            {
                vkDestroyImageView(unit->dev.device, it->second, nullptr);
                it = unit->views.erase(it);
            }
            else
                ++it;
        HostImage& image = unit->images.at(key);
        DestroyHi(key, image);  // FM4 Recomp (M9): its scaled twin, if any
        runtime::video::mem::Global().Release(image.ledger);
        vkDestroyImage(unit->dev.device, image.image, nullptr);
        vkFreeMemory(unit->dev.device, image.memory, nullptr);
        unit->imageBytes -= image.bytes;
        unit->images.erase(key);
        unit->evicted++, unit->evictedSince++;
    }
}

// FM4 Recomp (M9, live resolution, step 2): every scaled twin and its views destroyed, at a change of the internal scale (the
// renderer's switch: every submission confirmed complete, no command buffer open, so nothing recorded or in flight samples one).
// A twin is at one scale (EnsureHi refuses another) and Bind samples it while hiValid whatever the scale, so none may outlive the
// scale it was made at. The 1x images are untouched: they hold guest memory's words and are sampled until the next resolve over
// them gives scaled words again. The unit keeps no bound view between draws (Bind looks the view up at each bind; the prepare
// memo holds entries and plans, never views), so nothing else needs forgetting.
// Known limit: an image created while the display image was 1280x720 has no TRANSFER_SRC usage (HostImage::blitSource false) and
// never gets a twin, so after a start at 720p a resolved texture stays 1x until it is evicted and made again.
size_t DestroyTextureTwins()
{
    if (!unit)
        return 0;
    size_t destroyed = 0;
    for (auto& [key, image] : unit->images)
        if (image.hiImage != VK_NULL_HANDLE)
            DestroyHi(key, image), destroyed++;
    for (auto& [key, view] : unit->hiViews)  // none is left unless a view outlived its image (never expected)
        vkDestroyImageView(unit->dev.device, view, nullptr);
    unit->hiViews.clear();
    return destroyed;
}

std::string TextureUnitSummary()
{
    if (!unit)
        return "textures: no unit";
    char text[512];
    std::snprintf(text, sizeof text, "textures: %llu upload(s), %llu bytes since the last swap; %llu unchanged reuse(s), %llu bytes not reloaded; %zu image(s) (%llu MB, %llu evicted since the last swap), %zu view(s), %zu sampler(s); %llu GPU-written range(s) marked",
                  (unsigned long long)unit->uploadsSince, (unsigned long long)unit->uploadedBytesSince, (unsigned long long)unit->reusedSince,
                  (unsigned long long)unit->reusedBytesSince, unit->images.size(),
                  (unsigned long long)(unit->imageBytes >> 20), (unsigned long long)unit->evictedSince, unit->views.size(), unit->samplers.size(), (unsigned long long)unit->model.Log().Marks());
    std::string summary = text;
    std::snprintf(text, sizeof text, "; hashing: %llu call(s), %llu MB, %.1f ms", (unsigned long long)hashCallsSince, (unsigned long long)(hashBytesSince >> 20),
                  hashNsSince / 1e6);
    summary += text;
    hashCallsSince = hashBytesSince = hashNsSince = 0;
    // FM4 Recomp (M7): the CPU texture staging (FillStaging) of the swap, and with FM4_TEXTURE_GPU_UNTILE the GPU-untiled loads.
    std::snprintf(text, sizeof text, "; CPU staging: %llu fill(s), %.2f MB, %.2f ms", (unsigned long long)unit->fillsSince, unit->fillBytesSince / 1048576.0,
                  unit->fillNsSince / 1e6);
    summary += text;
    if (GpuUntileOn())
    {
        std::snprintf(text, sizeof text, "; GPU untile: %llu load(s), %.2f MB, %llu staged on the CPU", (unsigned long long)unit->gpuLoadsSince,
                      unit->gpuLoadBytesSince / 1048576.0, (unsigned long long)unit->gpuFallbacksSince);
        summary += text;
    }
    unit->fillsSince = unit->fillBytesSince = unit->fillNsSince = unit->gpuLoadsSince = unit->gpuLoadBytesSince = unit->gpuFallbacksSince = 0;
    if (runtime::video::scale::OutputScaled())  // FM4 Recomp (M9): the scaled twins of the swap
    {
        std::snprintf(text, sizeof text, "; scaled twins: %llu image(s), %llu write(s), %llu bind(s), %llu bind(s) of the 1x image for texel offsets, %llu dropped",
                      (unsigned long long)unit->hiImages, (unsigned long long)unit->hiAdoptedSince, (unsigned long long)unit->hiBindsSince,
                      (unsigned long long)unit->hiOffsetBindsSince, (unsigned long long)unit->hiDroppedSince);
        summary += text;
        unit->hiAdoptedSince = unit->hiBindsSince = unit->hiOffsetBindsSince = unit->hiDroppedSince = 0;
    }
    if (ResolveResident())  // FM4 Recomp: the resident resolves of the swap (resolve_resident.h)
    {
        std::snprintf(text, sizeof text, "; resident: %llu image write(s), %llu bind(s), %llu dropped", (unsigned long long)unit->adoptedSince,
                      (unsigned long long)unit->residentBindsSince, (unsigned long long)unit->residentDroppedSince);
        summary += text;
    }
    unit->adoptedSince = unit->residentBindsSince = unit->residentDroppedSince = 0;
    if (WriteWatchOn())  // FM4 Recomp: the swap's write watch
    {
        const WriteWatch::Stats& w = unit->model.ProtectingWatch()->GetStats();
        const uint64_t faults = w.faults.load(), protects = w.protects.load();
        const uint64_t faultNs = watchFaultNs.load(std::memory_order_relaxed);
        std::snprintf(text, sizeof text, "; watch: %llu bind(s) not hashed (%llu MB), %llu fault(s), %llu page(s) protected; fault hook %.2f ms, protect %.2f ms",
                      (unsigned long long)unit->watchSkipsSince, (unsigned long long)(unit->watchSkippedBytesSince >> 20),
                      (unsigned long long)(faults - unit->watchFaultsAt), (unsigned long long)(protects - unit->watchProtectsAt),
                      (faultNs - unit->watchFaultNsAt) / 1e6, unit->watchProtectNsSince / 1e6);
        summary += text;
        unit->watchFaultsAt = faults, unit->watchProtectsAt = protects, unit->watchFaultNsAt = faultNs;
    }
    unit->watchSkipsSince = unit->watchSkippedBytesSince = unit->watchProtectNsSince = 0;
    unit->uploadsSince = unit->uploadedBytesSince = unit->reusedSince = unit->reusedBytesSince = unit->evictedSince = 0;
    if (const std::string diag = DiagTextureSwapSummary(); !diag.empty())  // FM4_DIAG_TEXTURE_UPLOADS (diagnostic)
        std::fprintf(stderr, "%s\n", diag.c_str());
    return summary;
}

void MarkGuestWritten(uint32_t physical, uint32_t bytes)
{
    if (!unit)
        Stop(StopCode::Unimplemented, "renderer: guest range %08X + %u bytes written by the GPU before the texture unit exists", physical, bytes);
    // FM4 Recomp (M7 CPU cost): before the mark moves the page sequences, remember which overlapping images were clean (AdoptResolve).
    std::vector<HostImage*> over;
    if (WriteWatchOn() && unit->model.ProtectingWatch() != nullptr)
        for (auto& [key, image] : unit->images)
            if (!image.resident && !image.hashed && image.initialized &&
                (RangesOverlap(image.description.base.physical, image.description.base.size, physical, bytes) ||
                 RangesOverlap(image.description.mips.physical, image.description.mips.size, physical, bytes)))
                image.cleanAtMark = WatchClean(image), over.push_back(&image);
    unit->model.MarkWritten(physical, bytes, currentPartSerial);  // FM4_PARTIAL_RETIRE: tagged with the recording part
    for (HostImage* image : over)
        image->markSequence = unit->model.ProtectingWatch()->Current();
    DiagTextureGpuWrite(physical, bytes);  // FM4_DIAG_TEXTURE_UPLOADS (diagnostic)
}

uint64_t GuestWritesPendingSerial(uint32_t physical, uint32_t bytes) { return unit ? unit->model.PendingSerial(physical, bytes) : 0; }
void GuestWritesRetiredUpTo(uint64_t serial)
{
    if (unit)
        unit->model.RetiredUpTo(serial);
}

void GuestRangeHostWrite(uint32_t physical, uint32_t bytes)
{
    if (unit && WriteWatchOn() && unit->model.ProtectingWatch() != nullptr && bytes != 0)
        unit->model.ProtectingWatch()->BeforeHostWrite(physical, bytes);
}

bool GuestWritesPending() { return unit && unit->model.Pending(); }

bool GuestWritesOverlap(uint32_t physical, uint32_t bytes) { return unit && unit->model.OverlapsPending(physical, bytes); }

bool WatchGuestRange(uint32_t physical, uint32_t bytes, uint64_t& sequence)
{
    if (!unit || !WriteWatchOn() || unit->model.ProtectingWatch() == nullptr || bytes == 0)
        return false;
    WriteWatch& w = *unit->model.ProtectingWatch();
    const uint64_t start = MonotonicNs();
    sequence = w.Current();  // before the protection: a store that faults in between bumps above it (guest_writes.h)
    const bool watched = w.Watch(physical, bytes);
    unit->watchProtectNsSince += MonotonicNs() - start;
    return watched;
}

bool GuestRangeUnwritten(uint32_t physical, uint32_t bytes, uint64_t sequence)
{
    return unit && WriteWatchOn() && unit->model.ProtectingWatch() != nullptr && unit->model.ProtectingWatch()->WriteSequence(physical, bytes) <= sequence;
}

void GuestWritesRetired()
{
    if (!unit)
        return;
    unit->model.Retired();
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): the write-back just landed; a resident image's content is taken as those guest bytes
    // (resolve_resident.h HYPOTHESIS; FM4_RESOLVE_RESIDENT_CHECK compares the words). M7 CPU cost: instead of hashing the guest
    // bytes the image already holds (3.6 MiB per 1280x720 target, every frame, on the command-processor thread), the write watch is
    // armed and committed now: the image is clean until a write moves a page sequence (WatchClean), and `hashed` is cleared so a
    // later dirty bind never matches a stale hash (it hashes once and uploads).
    for (auto it = unit->images.begin(); unit->residentImages != 0 && it != unit->images.end(); ++it)
        if (HostImage& image = it->second; image.resident)
        {
            image.resident = false, unit->residentImages--;
            image.hashed = false, image.sourceHash = 0;
            if (WriteWatchOn())
                ArmWatch(image), CommitWatch(image);
        }
}

}  // namespace runtime::video
