// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The CPU side of the texture cache (M4 unit 7, execution model D-a "eager guest writes + GPU guard",
// "write-watch textures"): which guest pages a loaded texture watches, how a guest write invalidates
// it, when it is loaded again, and what each draw costs. No Vulkan type: the renderer implements
// WatchBackend with mprotect on the views that map the range, the test (tests/runtime/
// test_texture_info.cpp) with a fake. Design after ReCarbon runtime/kernel/write_watch.h:1-27, :36-55
// and runtime/gpu/renderer.cpp:3562-3600, :4355-4363, :4425-4455 (read, not copied: per
// guest page write sequences, a global epoch, a per-texture "loaded at" sequence and "checked at"
// epoch) and Xenia's shared-memory watches (texture_cache.h:150-180 TextureKey at 67f16c4e315f).
// No hashing of texture memory per draw (measured in an earlier port: 22-40 us per fetch).
//
// The contract, in order:
//  1. A draw binds fetch constant F in slot s: TextureCache::Find(F) describes it (texture_info.h)
//     the first time its key is seen, then returns the entry and whether it must be (re)loaded.
//     Cost per draw per slot: one hash lookup and one epoch compare; a walk over the texture's pages
//     only when some watched page anywhere was written since this entry was last checked.
//  2. A load: TextureCache::Plan gives the staging bytes per level and the sequence to record
//     (WriteWatch::Current, taken BEFORE the untile reads guest memory, so a write during the copy
//     makes the entry stale again). The renderer: BeginSlot(s) (only an idle slot), Hold the staging
//     buffer and the previous host image (if any) by that slot's lifetime (gpu_guard.h: destroyed
//     after a confirmed completion, never while in flight), untile each level into the staging buffer
//     (texture_untile.h), record the buffer-to-image copies into the slot being recorded, then
//     TextureCache::Loaded, which watches the entry's two guest ranges (level 0, the mips).
//  3. A watched page: its host pages (16 KB on Apple silicon, 4 guest pages each) are read-only in
//     every view that maps the physical memory. The first CPU store faults; the guest fault handler
//     (runtime/guest_memory.cpp OnFault) first asks WriteWatch::OnWriteFault: a watched page moves the
//     write sequence of its 4 guest pages, is made writable again (the heap's own access), and the
//     store re-runs. An unwatched page falls through to the existing stop (a real guest fault, a
//     data-import guard page, ProtectGuestMemoryReadOnly's stop-5 guards: none of those is ever
//     watched, so their stops are unchanged). The handler runs on the faulting guest thread: the
//     hook does no allocation, only atomic stores and the backend's mprotect.
//  4. Coexistence with the heap (kernel/virtual_memory.cpp Heap::SetHostAccess recomputes a host
//     page's access from the guest protections whenever a neighbouring page is committed, protected
//     or released, which would silently make a watched page writable again): the heap masks out
//     write access for host pages WriteWatch::IsWatchedHostPage reports, and the watch's own
//     unprotect restores the heap's computed access (never a blind read-write: a guest page the title
//     protected read-only stays read-only).
//  5. A GPU write into guest memory (a resolve, M4 unit 6) calls WriteWatch::MarkWritten for the
//     range: no fault is involved, the sequences move explicitly.
//  6. A texture the CPU rewrites every frame (the intro video's three k_8 planes, 1280x720 and two
//     640x360, ForzaRecomp M3-runtime.md:80): each frame the title's writes fault once per host
//     page (57 + 15 + 15 = 87 faults and unprotects), the next draw finds the three entries stale
//     (one page walk each), reloads them (1,382,400 bytes untiled and copied) and watches them again
//     (87 protects). That is the bound: faults = ceil(bytes written / 16 KB) per written range per
//     frame; no per-draw hashing. WriteWatch::Stats counts it for the [gpu] log. The known failure
//     mode is false sharing (KB xbox360/gpu-edram-and-ring.md "Write-watch stalls are false sharing
//     with D3D resource headers"): a resource header in the same 16 KB page as texture data faults
//     once per header write; the counters show it before it is a problem.
#pragma once
#include "texture_info.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace runtime::video {

constexpr uint32_t kGuestPageSize = 4096;                 // the sequence granularity (a 4 KB guest page)
constexpr uint32_t kGuestPageCount = 0x20000000 / kGuestPageSize;  // 512 MB of physical memory

// The host side of the watch. `Protect(physical, size, true)` makes the host pages covering the range
// read-only in every view that maps it (A0000000 and C0000000 share memory; the E0000000 view has its
// own, kernel/virtual_memory.h); `false` restores the heap's computed access (rule 4). False on failure:
// the caller stops (a watch that may not have taken must never be trusted).
struct WatchBackend
{
    virtual ~WatchBackend() = default;
    virtual bool Protect(uint32_t physical, uint32_t size, bool readOnly) = 0;
};

class WriteWatch
{
public:
    struct Stats  // FM4 Recomp: atomics (faults are counted inside the signal handler)
    {
        std::atomic<uint64_t> faults{0}, protects{0}, unprotects{0}, rearms{0}, marked{0};
    };

    explicit WriteWatch(WatchBackend& backend, uint32_t hostPageSize = 16384)
        : backend_(backend), hostPage_(hostPageSize), hostPages_(kGuestPageCount * kGuestPageSize / hostPageSize),
          sequence_(new std::atomic<uint64_t>[kGuestPageCount]), watched_(new std::atomic<uint8_t>[hostPages_])
    {
        for (uint32_t i = 0; i < kGuestPageCount; i++)
            sequence_[i].store(0, std::memory_order_relaxed);
        for (uint32_t i = 0; i < hostPages_; i++)
            watched_[i].store(0, std::memory_order_relaxed);
    }
    WriteWatch(const WriteWatch&) = delete;
    WriteWatch& operator=(const WriteWatch&) = delete;

    uint32_t HostPageSize() const { return hostPage_; }
    // The sequence a load records before it reads guest memory (then WriteSequence(range) <= it means
    // no watched page of the range was written since).
    uint64_t Current() const { return current_.load(std::memory_order_acquire); }
    // Changes whenever any sequence moves: an entry checked at the current epoch needs no page walk.
    uint64_t Epoch() const { return epoch_.load(std::memory_order_acquire); }
    const Stats& GetStats() const { return stats_; }

    // Protect the host pages covering [physical, physical + size) (size 0: nothing). False: the
    // backend refused (the caller stops).
    bool Watch(uint32_t physical, uint32_t size)
    {
        if (size == 0)
            return true;
        if (!InRange(physical, size))
            return false;
        const uint32_t first = physical / hostPage_, last = (physical + size - 1) / hostPage_;
        // FM4 Recomp: the flags first, then the protection (a store that faults in between must find its page watched); a refusal
        // takes them back.
        for (uint32_t h = first; h <= last; h++)
            watched_[h].store(1, std::memory_order_release);
        if (!backend_.Protect(first * hostPage_, (last - first + 1) * hostPage_, true))
        {
            for (uint32_t h = first; h <= last; h++)
                watched_[h].store(0, std::memory_order_release);
            return false;
        }
        stats_.protects += last - first + 1;
        return true;
    }

    // The fault handler's question (rule 3): a write fault at physical address `physical`. True: it
    // was a watched host page; its guest pages' sequences moved, the epoch moved, the page is
    // writable again (backend Protect false), the store may re-run. False: not ours, unchanged.
    bool OnWriteFault(uint32_t physical)
    {
        if (physical >= kGuestPageCount * uint64_t(kGuestPageSize))
            return false;
        const uint32_t h = physical / hostPage_;
        if (watched_[h].load(std::memory_order_acquire) == 0)
            return false;
        BumpHostPage(h);
        watched_[h].store(0, std::memory_order_release);
        stats_.faults++, stats_.unprotects++;
        return backend_.Protect(h * hostPage_, hostPage_, false);
    }

    // FM4 Recomp (FM4_WRITE_WATCH; the guest fault hook, texture_vk.cpp): a write fault on a host page the heap means to be
    // writable, so only the watch can have protected it, whether or not its flag still says so (a concurrent fault, or a heap
    // recompute racing a Watch). Writable again first (backend Protect false: signal-safe), then the sequences and the epoch move,
    // then the flag clears: concurrent faulters on one page all take this path and all return true. Atomics and mprotect only.
    bool OnProtectionFault(uint32_t physical)
    {
        if (physical >= kGuestPageCount * uint64_t(kGuestPageSize))
            return false;
        const uint32_t h = physical / hostPage_;
        if (!backend_.Protect(h * hostPage_, hostPage_, false))
            return false;
        BumpHostPage(h);
        watched_[h].store(0, std::memory_order_release);
        stats_.faults++, stats_.unprotects++;
        return true;
    }

    // FM4 Recomp: the heap changes the host access of [physical, physical + size) (a commit, decommit, release, protect or a
    // zeroing clear): its protection is the heap's from now on, so the range counts as written and is no longer watched.
    void HostAccessChanged(uint32_t physical, uint32_t size)
    {
        if (size == 0 || !InRange(physical, size))
            return;
        for (uint32_t h = physical / hostPage_; h <= (physical + size - 1) / hostPage_; h++)
            watched_[h].store(0, std::memory_order_release);
        MarkWritten(physical, size);
    }

    // FM4 Recomp: before a system call writes [physical, physical + size) (pread: it cannot fault, it would return EFAULT): its
    // watched host pages are made writable and unwatched, and the range counts as written.
    void BeforeHostWrite(uint32_t physical, uint32_t size)
    {
        if (size == 0 || !InRange(physical, size))
            return;
        // FM4 Recomp (M7 CPU cost): contiguous watched pages are made writable with one call per run (a render target's write-back
        // covers hundreds of pages), the flags cleared first as before.
        const uint32_t last = (physical + size - 1) / hostPage_;
        for (uint32_t h = physical / hostPage_; h <= last;)
        {
            if (watched_[h].exchange(0, std::memory_order_acq_rel) == 0)
            {
                h++;
                continue;
            }
            uint32_t end = h + 1;
            while (end <= last && watched_[end].exchange(0, std::memory_order_acq_rel) != 0)
                end++;
            backend_.Protect(h * hostPage_, (end - h) * hostPage_, false);
            stats_.unprotects++;
            h = end;
        }
        MarkWritten(physical, size);
    }

    // A GPU write (a resolve) into [physical, physical + size): the guest pages' sequences move; the
    // pages stay as they are (a watched page the GPU writes is still watched against the CPU).
    void MarkWritten(uint32_t physical, uint32_t size)
    {
        if (size == 0 || !InRange(physical, size))
            return;
        const uint64_t seq = current_.fetch_add(1, std::memory_order_acq_rel) + 1;
        for (uint32_t p = physical / kGuestPageSize; p <= (physical + size - 1) / kGuestPageSize; p++)
            sequence_[p].store(seq, std::memory_order_release);
        epoch_.fetch_add(1, std::memory_order_acq_rel);
        stats_.marked++;
    }

    // The highest write sequence of the guest pages of the range (0 for an empty one).
    uint64_t WriteSequence(uint32_t physical, uint32_t size) const
    {
        if (size == 0 || !InRange(physical, size))
            return 0;
        uint64_t highest = 0;
        for (uint32_t p = physical / kGuestPageSize; p <= (physical + size - 1) / kGuestPageSize; p++)
            highest = std::max(highest, sequence_[p].load(std::memory_order_acquire));
        return highest;
    }

    // For the heap (rule 4): is the host page holding `physical` watched (its write access must stay
    // masked when the heap recomputes it)?
    bool IsWatchedHostPage(uint32_t physical) const
    {
        return physical < kGuestPageCount * uint64_t(kGuestPageSize) && watched_[physical / hostPage_].load(std::memory_order_acquire) != 0;
    }

    // The alternative to the mask, for a heap that cannot consult the watch: after the heap recomputed
    // host access over [physical, physical + size), protect the watched host pages inside it again.
    // Either the mask or this must be wired, or a neighbouring commit un-watches a texture silently
    // (the test's negative control).
    bool Rearm(uint32_t physical, uint32_t size)
    {
        if (size == 0 || !InRange(physical, size))
            return size == 0;
        for (uint32_t h = physical / hostPage_; h <= (physical + size - 1) / hostPage_; h++)
            if (watched_[h].load(std::memory_order_acquire) != 0)
            {
                if (!backend_.Protect(h * hostPage_, hostPage_, true))
                    return false;
                stats_.rearms++;
            }
        return true;
    }

private:
    bool InRange(uint32_t physical, uint32_t size) const
    {
        return uint64_t(physical) + size <= kGuestPageCount * uint64_t(kGuestPageSize);
    }
    void BumpHostPage(uint32_t h)
    {
        // ReCarbon's rule (write_watch.h:9-10): a CPU write marks every guest page of its host page
        // (the handler cannot tell which 4 KB page the title wrote).
        const uint64_t seq = current_.fetch_add(1, std::memory_order_acq_rel) + 1;
        const uint32_t first = h * (hostPage_ / kGuestPageSize);
        for (uint32_t p = first; p < first + hostPage_ / kGuestPageSize; p++)
            sequence_[p].store(seq, std::memory_order_release);
        epoch_.fetch_add(1, std::memory_order_acq_rel);
    }

    WatchBackend& backend_;
    const uint32_t hostPage_, hostPages_;
    std::unique_ptr<std::atomic<uint64_t>[]> sequence_;  // per guest page
    std::unique_ptr<std::atomic<uint8_t>[]> watched_;    // per host page
    std::atomic<uint64_t> current_{0}, epoch_{0};
    Stats stats_;
};

// What identifies a host texture (Xenia texture_cache.h:150-180 TextureKey, minus its signed /
// scaled-resolve bits, which this cache does not have): the raw fetch-constant fields that decide the
// storage and the host image. Two fetch constants with the same key share one host image; the
// sampler, swizzle, signs and filters are per draw (the description) and do not change the image.
struct TextureKey
{
    uint32_t base = 0, mip = 0;          // physical addresses (page-aligned)
    uint32_t width = 0, height = 0;      // texels
    uint32_t pitch = 0;                  // texels
    uint32_t format = 0, endian = 0, mipMaxLevel = 0;
    bool tiled = false, packedMips = false;
    uint32_t depth = 1, dimension = 1;   // 3D (dimension 2): size_3d's depth; width / height from size_3d too; FM4 Recomp: a cube
                                         // (dimension 3): size_2d's stack depth + 1, so a constant of other face count never reuses it
    bool stacked = false;                // dword 1 bit 10: an array, other storage (refused by DescribeTexture; keyed so a
                                         // stacked constant never reuses a non-stacked entry with the same fields)
    bool signedData = false;             // FM4 Recomp: every component signed (texture_info.h SignedSeparate): another host image
    bool operator==(const TextureKey&) const = default;
};
inline TextureKey KeyOf(const TextureFetch& f)
{
    const bool volume = f.dimension == 2;  // xenos.h:1255-1271: dword 2 is size_3d for 3D
    return {f.baseAddress, f.mipAddress, volume ? f.width3D : f.dimension == 0 ? f.width1D : f.width, volume ? f.height3D : f.dimension == 0 ? 1 : f.height,
            f.pitchTexels, f.format, f.endian,
            f.mipMaxLevel, f.tiled, f.packedMips, volume ? f.depth3D : f.dimension == 3 ? f.stackDepth + 1 : 1, f.dimension, f.stacked,
            SignedSeparate(f)};
}
struct TextureKeyHash
{
    size_t operator()(const TextureKey& k) const
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint64_t v : {uint64_t(k.base), uint64_t(k.mip), uint64_t(k.width) << 32 | k.height, uint64_t(k.pitch) << 32 | k.format,
                           uint64_t(k.endian) << 32 | k.mipMaxLevel, uint64_t(k.tiled) << 1 | k.packedMips, uint64_t(k.depth) << 32 | k.dimension, uint64_t(k.stacked) | uint64_t(k.signedData) << 1})
            h = (h ^ v) * 0x100000001b3ull;
        return size_t(h);
    }
};

struct TextureEntry
{
    TextureKey key;
    TextureDescription description;  // valid (ok) for every entry the cache holds
    uint64_t loadedSequence = 0;     // WriteWatch::Current when last loaded
    uint64_t checkedEpoch = ~0ull;   // WriteWatch::Epoch at the last validity check
    // FM4 Recomp (M7 CPU cost): TextureCache::Plan's per-description part, computed once (mutable: Plan is const)
    mutable bool planned = false;
    mutable size_t planStaging = 0;
    mutable std::vector<size_t> planOffsets, planBytes;
    bool uploaded = false;           // a host image holds the guest data of loadedSequence
    uint32_t generation = 0;         // incremented per load: the renderer keys its host image by it
    uint64_t lastUse = 0;            // Find count at the last lookup (eviction is a later unit)
};

struct LoadPlan
{
    uint64_t sequence = 0;              // WriteWatch::Current before the untile reads guest memory
    size_t stagingBytes = 0;            // the slot's staging buffer: every level, tightly packed rows
    std::vector<size_t> levelOffsets;   // each level's offset in the staging buffer (16-byte aligned)
    std::vector<size_t> levelBytes;     // each level's bytes (widthBlocks * heightBlocks * depth * bytes per block)
};

class TextureCache
{
public:
    struct Stats
    {
        uint64_t finds = 0, described = 0, refused = 0, walks = 0, stale = 0, loads = 0;
    };
    struct Lookup
    {
        TextureEntry* entry = nullptr;  // null with `error`: the fetch constant is refused (the caller stops)
        bool load = false;              // the entry has no valid host image: Plan, untile, Loaded
        std::string error;
    };

    explicit TextureCache(WriteWatch& watch) : watch_(watch) {}

    // Per draw, per texture slot the shader reads (rule 1): the *storage* entry for the fetch
    // constant's key. The storage description depends only on the key's fields, so it is computed
    // once per key; the per-draw part (swizzle, signs, filters, the shader's dimension and overrides)
    // is DescribeBinding(fetch, entry->description.host, shader), computed at every draw by the
    // renderer (bit arithmetic, no level loop) and refused per draw. Two fetch constants with the
    // same key and different swizzles or samplers share one entry and one host image.
    // FM4 Recomp (M7 CPU cost): `validate` false skips rule 1's page walk (Valid) when the caller reloads at every bind anyway
    // (TextureValidity::ReloadEveryBind): the lookup then always says load.
    Lookup Find(const uint32_t fetch[6], bool validate = true)
    {
        stats_.finds++;
        const TextureKey key = KeyOf(DecodeTextureFetch(fetch));
        auto it = entries_.find(key);
        if (it == entries_.end())
        {
            TextureDescription d = DescribeTexture(fetch);
            if (!d.ok)
            {
                stats_.refused++;
                return {nullptr, false, d.error};
            }
            stats_.described++;
            TextureEntry fresh;
            fresh.key = key, fresh.description = std::move(d);
            it = entries_.emplace(key, std::move(fresh)).first;
        }
        TextureEntry& e = it->second;
        e.lastUse = stats_.finds;
        return {&e, !validate || !Valid(e), {}};
    }

    // FM4 Recomp (M7 CPU cost): the level offsets and bytes depend only on the description, so they are computed once per entry
    // and copied; the sequence is fresh at every call (WriteWatch::Current before the untile reads guest memory).
    LoadPlan Plan(const TextureEntry& e) const
    {
        if (!e.planned)
        {
            e.planStaging = 0;
            for (const MipLevel& l : e.description.levels)
            {
                e.planOffsets.push_back(e.planStaging);
                const size_t bytes = (size_t(l.widthBlocks) * l.heightBlocks * l.depth * e.description.layers) << e.description.log2BytesPerBlock;
                e.planBytes.push_back(bytes);
                e.planStaging += (bytes + 15) & ~size_t(15);
            }
            e.planned = true;
        }
        LoadPlan p;
        p.sequence = watch_.Current();
        p.stagingBytes = e.planStaging;
        p.levelOffsets = e.planOffsets;
        p.levelBytes = e.planBytes;
        return p;
    }

    // After the copies are recorded (rule 2): the entry holds the guest data as of `plan.sequence` and
    // its ranges are watched. False: the watch could not be taken (the caller stops; the entry stays
    // invalid, so nothing is ever drawn from an unwatched image).
    bool Loaded(TextureEntry& e, const LoadPlan& plan)
    {
        e.uploaded = false;
        if (!watch_.Watch(e.description.base.physical, e.description.base.size) ||
            !watch_.Watch(e.description.mips.physical, e.description.mips.size))
            return false;
        e.loadedSequence = plan.sequence;
        e.checkedEpoch = ~0ull;  // the next Find walks once (a write between Current and Watch counts)
        e.uploaded = true;
        e.generation++;
        stats_.loads++;
        return true;
    }

    size_t Size() const { return entries_.size(); }
    const Stats& GetStats() const { return stats_; }

private:
    // Rule 1's cost: O(1) at an unchanged epoch; a page walk of the entry's ranges otherwise.
    bool Valid(TextureEntry& e)
    {
        if (!e.uploaded)
            return false;
        const uint64_t epoch = watch_.Epoch();
        if (e.checkedEpoch == epoch)
            return true;
        stats_.walks++;
        const bool valid = watch_.WriteSequence(e.description.base.physical, e.description.base.size) <= e.loadedSequence &&
                           watch_.WriteSequence(e.description.mips.physical, e.description.mips.size) <= e.loadedSequence;
        if (!valid)
        {
            e.uploaded = false;
            stats_.stale++;
            return false;
        }
        e.checkedEpoch = epoch;
        return true;
    }

    WriteWatch& watch_;
    std::unordered_map<TextureKey, TextureEntry, TextureKeyHash> entries_;
    Stats stats_;
};

}  // namespace runtime::video
