// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The CPU side of the texture unit's binding path (M4 unit 7 on the renderer, texture_vk.cpp): what a draw's
// texture slot needs before anything is recorded, decided without a Vulkan header so the test
// (tests/runtime/test_texture_vk.cpp) builds without one. New code; it composes texture_info.h (storage and
// binding descriptions), texture_untile.h (the copies) and texture_cache.h (TextureCache, WriteWatch).
//
//  - Validity (when a host image is reloaded from guest memory). kTextureValidity is the ONE switch:
//      ReloadEveryBind  (this version) every bind of a texture reloads it from guest memory, as of the moment
//                       the command processor records the draw. Correct for every writer, including the
//                       title's own stores (the intro video's Y/U/V planes are written by translated title
//                       code: no runtime import decodes video, so no host write path could bump a counter
//                       for them). Its cost is the bytes bound per draw (the first screens: one UI or
//                       composite draw and one video draw per frame, ~5.5 MB a frame), bounded by
//                       the slot's upload ring (exhaustion stops, named). NOT per-draw hashing (D-a): no
//                       byte is compared, the copy is the work.
//      WriteWatch       texture_cache.h's model with a real WatchBackend (mprotect) and the guest fault hook
//                       (guest_memory.h SetGuestWriteFaultHook): reload only when a watched page was written.
//                       Drafted, not wired: it needs the Heap::SetHostAccess mask, pre-write notifies at the
//                       host paths that write guest memory through syscalls (file_system.cpp ReadDiscFile's
//                       pread, stfs.cpp), and the E0000000 view's 0x1000 shift.
//    In both, the cache's WriteWatch runs on UnprotectedWatchBackend (nothing is protected, so
//    WriteWatch::OnWriteFault is never called in this version); GPU writes move its sequences explicitly
//    (MarkWritten), so the switch to WriteWatch is the backend and the hook, nothing here.
//  - GPU writes (guest_writes.h): a range marked by a resolve is pending until the renderer retires the
//    submission that writes it; a texture over a pending range is refused (the renderer flushes after each
//    marking resolve, so a refusal means a write the renderer did not flush).
//  - Fail closed: more than one level, a BC format without textureCompressionBC, a non-identity component
//    mapping without imageViewFormatSwizzle, anisotropy, a gamma decision that disagrees with DrawState's,
//    a guest range that is not wholly in one committed view.
#pragma once
#include "texture_cache.h"
#include "texture_info.h"
#include "texture_untile.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace runtime::video {

enum class TextureValidity { ReloadEveryBind, WriteWatch };
constexpr TextureValidity kTextureValidity = TextureValidity::ReloadEveryBind;

// The watch's backend in this version: protects nothing (see the header comment).
struct UnprotectedWatchBackend final : WatchBackend
{
    bool Protect(uint32_t, uint32_t, bool) override { return true; }
};

// Ranges a recorded resolve writes, pending until their submission is confirmed complete.
class GuestWriteLog
{
public:
    void Mark(uint32_t physical, uint32_t bytes)
    {
        if (bytes == 0)
            return;
        pending_.push_back({physical, bytes});
        marks_++;
    }
    bool Pending() const { return !pending_.empty(); }
    // The first pending range sharing a byte with [physical, physical + bytes) (an empty range shares none).
    bool Overlaps(uint32_t physical, uint32_t bytes, GuestRange& hit) const
    {
        if (bytes == 0)
            return false;
        for (const GuestRange& r : pending_)
            if (uint64_t(physical) < uint64_t(r.physical) + r.size && uint64_t(r.physical) < uint64_t(physical) + bytes)
            {
                hit = r;
                return true;
            }
        return false;
    }
    void Retired() { pending_.clear(); }
    uint64_t Marks() const { return marks_; }

private:
    std::vector<GuestRange> pending_;
    uint64_t marks_ = 0;
};

// The formats the first screens sample: k_8 (2),
// k_8_8_8_8_AS_16_16_16_16 (50), k_DXT1_AS_16_16_16_16 (51). texture_info.cpp also maps 6, 18 and 54 (the
// same host formats as 50 / 51, and A2B10G10R10); each joins this list when a run first needs it.
// 54 (k_2_10_10_10_AS_16_16_16_16): the HDR resolve's destination at 1BB71000, sampled from draw 27039 (the local capture draw_c2d1 (not distributed)).
// 36 (k_32_FLOAT): a 32x32 float texture the HDR luminance pass samples (the local capture draw_loop1 (not distributed) draw 22725 tex 1).
// 6 (k_8_8_8_8): a 16x16x16 3D lookup texture (the local capture draw_tex3d1 (not distributed) draw 33248 slot 7).
// 23 (k_24_8_FLOAT): a 1280x720 depth texture at 1C421000 the intro race's first a0-relative PS samples (the local capture scenario6 (not distributed)
// draw 452279 slot 1).
// 53 (k_DXT4_5_AS_16_16_16_16): a 256x128 gamma DXT5 texture at 11946000 (the local capture scenario9 (not distributed) draw 459668 slot 0).
// 20 (k_DXT4_5): a 512x512 gamma DXT5 with 10 levels at 0A98E000, sampled by the dynamic-loop PS.
constexpr bool ObservedFormat(uint32_t format)
{
    return format == 2 || format == 6 || format == 20 || format == 23 || format == 36 || format == 50 || format == 51 || format == 53 ||
           format == 54;
}

// VkComponentMapping {R, G, B, A}: what a view may use without imageViewFormatSwizzle (the portability
// subset's VkImageViewCreateInfo rule: with the feature VK_FALSE every component must be the identity
// swizzle; an explicit R, G, B, A is the identity).
constexpr bool IdentityComponents(const uint32_t c[4])
{
    return c[0] == vk::kSwizzleR && c[1] == vk::kSwizzleG && c[2] == vk::kSwizzleB && c[3] == vk::kSwizzleA;
}

// One texture slot of one draw, decided on the CPU.
struct PreparedTexture
{
    TextureEntry* entry = nullptr;  // null with `error`: refused
    TextureBinding binding;         // components, sampler (valid when entry is set)
    bool load = false;              // reload the host image from guest memory before the draw
    LoadPlan plan;                  // valid when load
    std::string error;
};

struct TextureDeviceFeatures
{
    bool textureCompressionBC = false, imageViewFormatSwizzle = false;
};

class TextureUnitModel
{
public:
    explicit TextureUnitModel(TextureValidity validity) : watch_(backend_), cache_(watch_), validity_(validity) {}
    TextureUnitModel(const TextureUnitModel&) = delete;
    TextureUnitModel& operator=(const TextureUnitModel&) = delete;

    // `fetch`: the draw's six fetch-constant words (TextureSlot::words); `shader`: the resolved filters and
    // the shader's dimension; `drawStateGamma`: TextureSlot::gamma (draw_record.h emits the shader define
    // from it; the binding must agree).
    PreparedTexture Prepare(const uint32_t fetch[6], const ShaderFetch& shader, bool drawStateGamma, const TextureDeviceFeatures& device);
    // After the copies are recorded (texture_cache.h rule 2).
    bool Loaded(TextureEntry& e, const LoadPlan& plan) { return cache_.Loaded(e, plan); }

    void MarkWritten(uint32_t physical, uint32_t bytes)
    {
        watch_.MarkWritten(physical, bytes);
        log_.Mark(physical, bytes);
    }
    bool Pending() const { return log_.Pending(); }
    void Retired() { log_.Retired(); }

    const TextureCache& Cache() const { return cache_; }
    const WriteWatch& Watch() const { return watch_; }
    const GuestWriteLog& Log() const { return log_; }

private:
    UnprotectedWatchBackend backend_;
    WriteWatch watch_;
    TextureCache cache_;
    GuestWriteLog log_;
    const TextureValidity validity_;
};

// ---- Staging in the slot's upload ring -------------------------------------------------------------
// vkCmdCopyBufferToImage's bufferOffset must be a multiple of the format's texel block size (for a colour
// format) and, in Vulkan 1.2's VkBufferImageCopy rules, of 4 (to be confirmed by the validation layer on the
// first run); the device's optimalBufferCopyOffsetAlignment is honoured too, and LoadPlan's level
// offsets are 16-byte aligned. 0 when `optimalCopyAlignment` is not a power of two (the caller refuses).
constexpr uint64_t StagingAlignment(uint32_t bytesPerBlock, uint64_t optimalCopyAlignment)
{
    if (optimalCopyAlignment == 0 || (optimalCopyAlignment & (optimalCopyAlignment - 1)) || bytesPerBlock == 0 ||
        (bytesPerBlock & (bytesPerBlock - 1)))
        return 0;
    uint64_t a = 16;
    if (bytesPerBlock > a)
        a = bytesPerBlock;
    if (optimalCopyAlignment > a)
        a = optimalCopyAlignment;
    return a;
}

// A block of `bytes` at a multiple of `alignment` (a power of two) in `ring` (pipeline_key.h UploadRing, or a
// fake with its Allocate): the ring's storage alignment is kept by asking for `alignment - 1` more bytes.
// False when the ring cannot hold it (nothing allocated: UploadRing::Allocate's rule).
template <typename Ring>
bool PlaceInRing(Ring& ring, uint64_t bytes, uint64_t alignment, uint64_t& offset)
{
    if (alignment == 0 || (alignment & (alignment - 1)))
        return false;
    uint64_t raw = 0;
    if (!ring.Allocate(bytes + alignment - 1, false, raw))
        return false;
    offset = (raw + alignment - 1) & ~(alignment - 1);
    return true;
}

// The host bytes of guest physical range [physical, physical + bytes), or null with `error` (the range is not
// wholly inside one committed view). The renderer's resolves through PhysicalToGuest; the test fakes it.
using GuestBytesFn = const uint8_t* (*)(void* context, uint32_t physical, uint32_t bytes, std::string& error);

// Every level of `d` from guest memory into `out` (plan.stagingBytes bytes) at plan.levelOffsets: tiled levels
// through DetileBlocksAt, linear through CopyLinearBlocksAt, with the fetch constant's endian. False with
// `error` (nothing usable in `out`).
bool FillStaging(const TextureDescription& d, const LoadPlan& plan, GuestBytesFn guest, void* context, uint8_t* out, std::string& error);

// One VkBufferImageCopy per level (tightly packed rows: bufferRowLength = bufferImageHeight = 0).
struct CopyRegion
{
    uint64_t bufferOffset = 0;
    uint32_t level = 0, width = 0, height = 0;  // texels (the host image level's extent)
    uint32_t depth = 1;                         // 3D: slices (imageExtent.depth); 1 for 2D
};
std::vector<CopyRegion> CopyRegions(const TextureDescription& d, const LoadPlan& plan, uint64_t ringOffset);

// Samplers are cached by their VkSamplerCreateInfo fields (SamplerDescription; floats by their bits).
struct SamplerDescriptionHash
{
    size_t operator()(const SamplerDescription& s) const
    {
        uint32_t f[3];
        std::memcpy(&f[0], &s.maxAnisotropy, 4);
        std::memcpy(&f[1], &s.minLod, 4);
        std::memcpy(&f[2], &s.maxLod, 4);
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint64_t v : {uint64_t(s.magFilter), uint64_t(s.minFilter), uint64_t(s.mipmapMode), uint64_t(s.addressModeU), uint64_t(s.addressModeV),
                           uint64_t(s.addressModeW), uint64_t(s.anisotropyEnable), uint64_t(f[0]), uint64_t(f[1]), uint64_t(f[2]), uint64_t(s.borderColor)})
            h = (h ^ v) * 0x100000001b3ull;
        return size_t(h);
    }
};

// A host view: one image (its storage key) seen through one component mapping.
struct ViewKey
{
    TextureKey key;
    uint32_t components[4] = {};
    bool operator==(const ViewKey& o) const { return key == o.key && std::memcmp(components, o.components, sizeof components) == 0; }
};
struct ViewKeyHash
{
    size_t operator()(const ViewKey& v) const
    {
        uint64_t h = TextureKeyHash{}(v.key);
        for (uint32_t c : v.components)
            h = (h ^ c) * 0x100000001b3ull;
        return size_t(h);
    }
};

}  // namespace runtime::video
