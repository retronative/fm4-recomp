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
#include <algorithm>
#include <memory>
#include "texture_cache.h"
#include "texture_info.h"
#include "texture_untile.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <deque>
#include <vector>

namespace runtime::video {

enum class TextureValidity { ReloadEveryBind, WriteWatch };
constexpr TextureValidity kTextureValidity = TextureValidity::ReloadEveryBind;

// The watch's backend in this version: protects nothing (see the header comment).
struct UnprotectedWatchBackend final : WatchBackend
{
    bool Protect(uint32_t, uint32_t, bool) override { return true; }
};

// FM4 Recomp: the content hash of guest texture bytes for FM4_TEXTURE_REUSE (texture_vk.cpp SourceHash): four 64-bit lanes
// (multiply-xor-rotate) over 32-byte blocks, the tail byte-wise, the length and seed mixed in, a final avalanche. Change detection
// of guest data (not adversarial input): any change of a byte changes the hash except with ~2^-64 probability.
inline uint64_t ContentHash(const uint8_t* p, size_t n, uint64_t seed)
{
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t a = seed ^ k, b = seed + k, c = ~seed, d = seed * k + 1;
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        uint64_t w[4];
        std::memcpy(w, p + i, 32);
        a = ((a ^ w[0]) * k), a = (a << 31) | (a >> 33);
        b = ((b ^ w[1]) * k), b = (b << 29) | (b >> 35);
        c = ((c ^ w[2]) * k), c = (c << 27) | (c >> 37);
        d = ((d ^ w[3]) * k), d = (d << 25) | (d >> 39);
    }
    uint64_t h = a ^ (b * 3) ^ (c * 5) ^ (d * 7) ^ n;
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33, h *= 0xFF51AFD7ED558CCDull, h ^= h >> 33;
    return h;
}

// FM4 Recomp: the ring bytes a texture bind reserves before its draw (FM4_TEXTURE_REUSE). A bind whose existing image was loaded
// from guest bytes with the same hash reuses it and uploads nothing, so it reserves nothing; every other bind (reuse off, a new or
// never-hashed image, an unreadable range or changed bytes) reserves its full staging plus alignment padding. The bind then decides
// from this same hash, so it never uploads into space it did not reserve. Measured before: race swaps with ~20 MB uploaded flushed
// the ring 29-33 times because every reused bind reserved its full staging.
inline uint64_t ReservedStagingBytes(bool reuse, bool imageHashed, bool hashRead, uint64_t hash, uint64_t imageHash, uint64_t fullBytes)
{
    return reuse && imageHashed && hashRead && hash == imageHash ? 0 : fullBytes;
}

// Ranges a recorded resolve writes, pending until their submission is confirmed complete.
class GuestWriteLog
{
public:
    // FM4 Recomp (FM4_PARTIAL_RETIRE): `serial` names the submission part that recorded the resolve (0: unknown); a part retired
    // on its own retires only the ranges it recorded (RetiredUpTo), the way ReCarbon's completion thread harvests submissions one
    // by one (runtime/gpu/renderer.cpp:9301) instead of the whole slot.
    void Mark(uint32_t physical, uint32_t bytes, uint64_t serial = 0)
    {
        if (bytes == 0)
            return;
        pending_.push_back({{physical, bytes}, serial});
        marks_++;
    }
    bool Pending() const { return !pending_.empty(); }
    // The first pending range sharing a byte with [physical, physical + bytes) (an empty range shares none).
    bool Overlaps(uint32_t physical, uint32_t bytes, GuestRange& hit) const
    {
        if (bytes == 0)
            return false;
        for (const Entry& e : pending_)
            if (uint64_t(physical) < uint64_t(e.range.physical) + e.range.size && uint64_t(e.range.physical) < uint64_t(physical) + bytes)
            {
                hit = e.range;
                return true;
            }
        return false;
    }
    // FM4_PARTIAL_RETIRE: the newest part whose pending range shares a byte with [physical, physical + bytes) (0: none).
    uint64_t PendingSerial(uint32_t physical, uint32_t bytes) const
    {
        uint64_t serial = 0;
        if (bytes == 0)
            return 0;
        for (const Entry& e : pending_)
            if (uint64_t(physical) < uint64_t(e.range.physical) + e.range.size && uint64_t(e.range.physical) < uint64_t(physical) + bytes)
                serial = std::max(serial, e.serial);
        return serial;
    }
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): as Overlaps, over every pending range but the last marked one.
    bool OverlapsEarlier(uint32_t physical, uint32_t bytes) const
    {
        GuestRange hit;
        GuestWriteLog earlier;
        if (pending_.size() > 1)
            earlier.pending_.assign(pending_.begin(), pending_.end() - 1);
        return earlier.Overlaps(physical, bytes, hit);
    }
    // FM4_PARTIAL_RETIRE: the ranges recorded by parts up to `serial` are complete (their write-backs landed).
    void RetiredUpTo(uint64_t serial)
    {
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Entry& e) { return e.serial <= serial; }), pending_.end());
    }
    void Retired() { pending_.clear(); }
    uint64_t Marks() const { return marks_; }

private:
    struct Entry
    {
        GuestRange range;
        uint64_t serial = 0;
    };
    std::vector<Entry> pending_;
    uint64_t marks_ = 0;
};

// The formats the first screens sample: k_8 (2),
// k_8_8_8_8_AS_16_16_16_16 (50), k_DXT1_AS_16_16_16_16 (51). texture_info.cpp also maps 6, 18 and 54 (the
// same host formats as 50 / 51, and A2B10G10R10); each joins this list when a run first needs it.
// 54 (k_2_10_10_10_AS_16_16_16_16): the HDR resolve's destination at 1BB71000.
// 36 (k_32_FLOAT): a 32x32 float texture the HDR luminance pass samples.
// 6 (k_8_8_8_8): a 16x16x16 3D lookup texture.
// 23 (k_24_8_FLOAT): a 1280x720 depth texture at 1C421000 the intro race's first a0-relative PS samples.
// 53 (k_DXT4_5_AS_16_16_16_16): a 256x128 gamma DXT5 texture at 11946000.
// 18 (k_DXT1): a 128x256 DXT1 with mips at 0DE6F000 (BC1 as its AS_16_16_16_16 twin 51).
// 25 (k_16_16): a 512x512 signed texture at 1C1E3000, the k_16_16 resolve's destination.
// 10 (k_8_8): the k_8_8 resolve's destination (an 8_8_8_8 source); admitted with that
// resolve, not yet observed as a fetch.
// 20 (k_DXT4_5): a 512x512 gamma DXT5 with 10 levels at 0A98E000, sampled by the dynamic-loop PS.
constexpr bool ObservedFormat(uint32_t format)
{
    return format == 2 || format == 6 || format == 10 || format == 18 || format == 20 || format == 23 || format == 25 || format == 36 || format == 49 ||
           format == 50 || format == 51 || format == 53 || format == 54 || format == 59 || format == 19 || format == 52 ||
           format == 29 || format == 32 ||  // FM4 Recomp (alpha, Car Select): k_16_16_16_16_EXPAND / _FLOAT, 8-byte blocks, no conversion
           format == 4;  // FM4 Recomp: k_5_6_5, 2-byte blocks, red and blue exchanged at the load (HostFormat::swapRedBlue565)
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
    bool samplerAnisotropy = false;  // the device feature, enabled at vkCreateDevice when offered
    float maxAnisotropy = 1.0f;      // VkPhysicalDeviceLimits::maxSamplerAnisotropy
};

class TextureUnitModel
{
public:
    // FM4 Recomp: `backend` (FM4_WRITE_WATCH: texture_vk.cpp's mprotect backend) drives a second WriteWatch, the protecting one
    // (ProtectingWatch: arming, the fault hook, the heaps' observers); the cache keeps its bookkeeping watch on the unprotected
    // backend (with ReloadEveryBind its Loaded must not protect anything).
    explicit TextureUnitModel(TextureValidity validity, WatchBackend* backend = nullptr) : watch_(backend_), cache_(watch_), validity_(validity)
    {
        if (backend != nullptr)
            protecting_ = std::make_unique<WriteWatch>(*backend);
    }
    TextureUnitModel(const TextureUnitModel&) = delete;
    TextureUnitModel& operator=(const TextureUnitModel&) = delete;

    // `fetch`: the draw's six fetch-constant words (TextureSlot::words); `shader`: the resolved filters and
    // the shader's dimension; `drawStateGamma`: TextureSlot::gamma (draw_record.h emits the shader define
    // from it; the binding must agree).
    // `allowPending` (FM4_RESOLVE_RESIDENT): no refusal of a range a recorded resolve has not written back; the caller binds
    // a host image that already holds the resolve's words (texture_vk.cpp) or refuses itself.
    // Returns a reference the caller uses within the bind (the next Prepare may replace it).
    const PreparedTexture& Prepare(const uint32_t fetch[6], const ShaderFetch& shader, bool drawStateGamma, const TextureDeviceFeatures& device,
                            bool allowPending = false);
    PreparedTexture PrepareUncached(const uint32_t fetch[6], const ShaderFetch& shader, bool drawStateGamma, const TextureDeviceFeatures& device, bool allowPending);
    // After the copies are recorded (texture_cache.h rule 2).
    bool Loaded(TextureEntry& e, const LoadPlan& plan) { return cache_.Loaded(e, plan); }

    void MarkWritten(uint32_t physical, uint32_t bytes, uint64_t serial = 0)
    {
        watch_.MarkWritten(physical, bytes);
        if (protecting_)
            protecting_->MarkWritten(physical, bytes);
        log_.Mark(physical, bytes, serial);
        prepared_.clear();  // the pending log changed: Prepare's answers may change
    }
    bool Pending() const { return log_.Pending(); }
    void Retired() { log_.Retired(), prepared_.clear(); }
    void RetiredUpTo(uint64_t serial) { log_.RetiredUpTo(serial), prepared_.clear(); }  // FM4_PARTIAL_RETIRE
    uint64_t PendingSerial(uint32_t physical, uint32_t bytes) const { return log_.PendingSerial(physical, bytes); }
    // FM4_PARTIAL_RETIRE: the newest part whose pending range a texture's guest ranges share a byte with (0: none).
    uint64_t ReadsPendingSerial(const uint32_t fetch[6])
    {
        const TextureCache::Lookup lookup = cache_.Find(fetch, false);  // FM4 Recomp (M7 CPU cost): a range question, no validity walk
        if (lookup.entry == nullptr)
            return 0;
        const TextureDescription& d = lookup.entry->description;
        return std::max(log_.PendingSerial(d.base.physical, d.base.size), log_.PendingSerial(d.mips.physical, d.mips.size));
    }
    // FM4 Recomp (FM4_RESOLVE_DEFER): whether a texture's guest ranges share a byte with a range a recorded resolve has not yet
    // written back (the renderer flushes before the draw that binds it); false for a constant the cache cannot describe.
    bool ReadsPending(const uint32_t fetch[6])
    {
        const TextureCache::Lookup lookup = cache_.Find(fetch, false);  // FM4 Recomp (M7 CPU cost): a range question, no validity walk
        if (lookup.entry == nullptr)
            return false;
        const TextureDescription& d = lookup.entry->description;
        GuestRange hit;
        return log_.Overlaps(d.base.physical, d.base.size, hit) || log_.Overlaps(d.mips.physical, d.mips.size, hit);
    }
    bool OverlapsPending(uint32_t physical, uint32_t bytes) const
    {
        GuestRange hit;
        return log_.Overlaps(physical, bytes, hit);
    }
    bool OverlapsEarlierPending(uint32_t physical, uint32_t bytes) const { return log_.OverlapsEarlier(physical, bytes); }

    const TextureCache& Cache() const { return cache_; }
    const WriteWatch& Watch() const { return watch_; }
    WriteWatch* ProtectingWatch() { return protecting_.get(); }  // FM4 Recomp (FM4_WRITE_WATCH); null without a backend
    const GuestWriteLog& Log() const { return log_; }

private:
    UnprotectedWatchBackend backend_;
    WriteWatch watch_;
    std::unique_ptr<WriteWatch> protecting_;  // FM4 Recomp (FM4_WRITE_WATCH)
    TextureCache cache_;
    GuestWriteLog log_;
    const TextureValidity validity_;
    // FM4 Recomp (M7 CPU cost): Prepare runs two or three times per texture slot per draw (StagingBytes, ReadsPendingWrite, Bind)
    // with the same inputs; its answer depends only on them, the cache's entries (created on first sight, never removed between
    // Prepares) and the pending log, so the last few answers are kept until the log changes. The plan's sequence is the one of
    // the first call: a lower bound, which only makes a later validity check stricter.
    struct Prepared
    {
        uint32_t fetch[6];
        ShaderFetch shader;
        TextureDeviceFeatures device;
        bool gamma, allowPending;
        PreparedTexture p;
    };
    std::deque<Prepared> prepared_;  // references into it stay valid across push_back/pop_front (the callers use the answer within one bind)
    PreparedTexture scratch_;         // the answer when the cache does not apply
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
    uint32_t layers = 1;                        // FM4 Recomp: cube 6 (the faces, consecutive in the buffer); else 1
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
