// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, vertex reuse across submissions): the placement and lifetime policy of the persistent vertex cache
// (opt-in FM4_VERTEX_CACHE_MB=<n>, with FM4_VERTEX_REUSE=1; renderer.cpp). Vulkan-free, so tests/runtime/test_vertex_cache.cpp
// covers it alone (tests/run_vertex_cache_tests.sh), as UploadRing is covered in pipeline_key.h.
//
// Why: FM4_VERTEX_REUSE keeps a copied vertex window in the slot's
// upload ring for the rest of the ring's lap (UploadRing::Intact). The ring is shared with the per-draw system, constant, index
// and error blocks, which fill it ~10 MB a race swap, so it laps every ~5-17 swaps and every window is copied again at once:
// the "vertices copied" spikes. Here vertex windows live in their own buffer, which no other block uses, and outlive laps,
// parts and flushes; a window is copied again only after a guest write (the write watch) or when its chunk is recycled.
//
// Reference: ReCarbon (GPL-3.0, read-only) reads vertices from `s_shared`, a GPU buffer of all guest physical memory
// (runtime/gpu/renderer.cpp:189-212), imported from host memory or a shadow copy kept current page by page from the write
// watch (renderer.cpp:1169-1188 EnsureShadow, runtime/kernel/write_watch.cpp:1291 SyncShadow). Updating a shadow page in place
// first waits for every submission that reads it (EnsureShadow: `while (busy = SyncShadow(...)) WaitForSubmission(busy)`). This
// cache is copy-on-write instead: a rewritten window gets a new place and its old bytes stay untouched until nothing in flight
// can read them, so a guest rewrite never waits for the GPU. Adapted idea, not code.
//
// Lifetime rules (each tested):
//   L1 The buffer is created once at device setup and destroyed only at shutdown, after every submission is confirmed complete
//      (renderer.cpp Shutdown, after GpuGuard::Shutdown). It is never resized or freed while the run goes on.
//   L2 The buffer is `chunks` fixed chunks of `chunkBytes`. Space is bump-allocated in the current chunk; bytes are written
//      only into space just allocated, never into space an entry already holds.
//   L3 Every chunk records the newest submission part (render_hooks.h currentPartSerial) that reads it: MarkRead at placement,
//      at a hit, and again when the draw is recorded (a split between the two moves the draw to a later part).
//   L4 A chunk is recycled (its generation bumped, its space reused from 0) only when its last reader is at or below the
//      retired watermark: Retired(serial) after RetireSlot (every part) or RetireUpTo (parts up to `serial`, in order).
//      The part being recorded is always above the watermark, so a chunk the open part reads is never recycled.
//   L5 Entries are valid only while their chunk keeps the generation they were placed in (checked lazily by Find).
//   L6 When no chunk can be recycled, Allocate answers NeedsRetire: the caller flushes (submit + confirmed retire) at the draw's
//      entry, where no command buffer handle is live, then places the draw's windows again from the first.
#pragma once
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace runtime::video {

class VertexCache
{
public:
    struct Entry
    {
        uint32_t chunk = 0;
        uint64_t generation = 0, offset = 0;  // offset: bytes into the whole buffer
        uint32_t dwords = 0;
        uint64_t sequence = 0;  // the write watch's sequence the window was copied under (guest_writes.h WatchGuestRange)
    };
    enum class Place { Ok, NeedsRetire, TooLarge };

    // `chunkBytes` divides `capacity`, at least 2 chunks; `align` a power of two that divides `chunkBytes`. Valid() says so.
    VertexCache(uint64_t capacity, uint64_t chunkBytes, uint64_t align)
        : chunkBytes_(chunkBytes), align_(align), chunks_(chunkBytes != 0 && capacity % chunkBytes == 0 ? capacity / chunkBytes : 0)
    {
    }
    bool Valid() const
    {
        return chunks_.size() >= 2 && align_ != 0 && (align_ & (align_ - 1)) == 0 && chunkBytes_ % align_ == 0;
    }

    // The entry for `key` while its chunk keeps the generation it was placed in (L5); nullptr otherwise.
    const Entry* Find(uint64_t key) const
    {
        const auto it = entries_.find(key);
        if (it == entries_.end() || it->second.generation != chunks_[it->second.chunk].generation)
            return nullptr;
        return &it->second;
    }

    // `bytes` of fresh space (L2) read by part `serial` (L3): in the current chunk, else in the least recently read chunk whose
    // last reader is retired (L4; ties: the lowest index). `offset` is into the whole buffer.
    Place Allocate(uint64_t bytes, uint64_t serial, uint64_t& offset, uint32_t& chunk)
    {
        if (!Valid() || bytes == 0 || bytes > chunkBytes_)
            return Place::TooLarge;
        if (current_ < chunks_.size())
        {
            Chunk& c = chunks_[current_];
            const uint64_t start = (c.used + align_ - 1) & ~(align_ - 1);
            if (start <= chunkBytes_ && bytes <= chunkBytes_ - start)
            {
                c.used = start + bytes;
                MarkRead(current_, serial);
                offset = uint64_t(current_) * chunkBytes_ + start, chunk = current_;
                return Place::Ok;
            }
        }
        uint32_t pick = uint32_t(chunks_.size());
        for (uint32_t k = 0; k < chunks_.size(); k++)
            if (chunks_[k].lastRead <= retired_ && (pick == chunks_.size() || chunks_[k].lastRead < chunks_[pick].lastRead))
                pick = k;
        if (pick == chunks_.size())
            return Place::NeedsRetire;
        Chunk& c = chunks_[pick];
        if (c.used != 0 || c.generation != 0)
            recycles_++;
        c.generation++, c.used = bytes, current_ = pick;
        MarkRead(pick, serial);
        offset = uint64_t(pick) * chunkBytes_, chunk = pick;
        return Place::Ok;
    }

    // A window copied at `offset` of `chunk` (just allocated) under `sequence`; replaces any entry for `key`. The replaced entry's
    // bytes are not reused before its chunk is recycled (L2, L4).
    void Remember(uint64_t key, uint32_t chunk, uint64_t offset, uint32_t dwords, uint64_t sequence)
    {
        if (entries_.size() >= sweepAt_)
            Sweep(), sweepAt_ = entries_.size() * 2 > kSweepEntries ? entries_.size() * 2 : kSweepEntries;
        entries_[key] = {chunk, chunks_[chunk].generation, offset, dwords, sequence};
    }
    void Forget(uint64_t key) { entries_.erase(key); }
    void ForgetAll() { entries_.clear(); }  // FM4 Recomp (diagnostic, FM4_DIAG_TRIGGER stale-vertices)

    void MarkRead(uint32_t chunk, uint64_t serial)
    {
        if (chunk < chunks_.size() && serial > chunks_[chunk].lastRead)
            chunks_[chunk].lastRead = serial;
    }
    // Every part up to `serial` is confirmed complete (L4).
    void Retired(uint64_t serial)
    {
        if (serial > retired_)
            retired_ = serial;
    }

    uint64_t Capacity() const { return uint64_t(chunks_.size()) * chunkBytes_; }
    uint64_t ChunkBytes() const { return chunkBytes_; }
    uint32_t Chunks() const { return uint32_t(chunks_.size()); }
    uint64_t RetiredSerial() const { return retired_; }
    uint64_t LastRead(uint32_t chunk) const { return chunks_[chunk].lastRead; }
    uint64_t Recycles() const { return recycles_; }
    size_t EntryCount() const { return entries_.size(); }

private:
    // Stale entries (their chunk recycled) are dropped when the map reaches sweepAt_ (at least kSweepEntries; twice the live
    // entries after a sweep, so a map of live entries is not swept at every insert).
    static constexpr size_t kSweepEntries = 65536;
    size_t sweepAt_ = kSweepEntries;
    struct Chunk { uint64_t generation = 0, used = 0, lastRead = 0; };
    void Sweep()
    {
        for (auto it = entries_.begin(); it != entries_.end();)
            it = it->second.generation != chunks_[it->second.chunk].generation ? entries_.erase(it) : std::next(it);
    }
    const uint64_t chunkBytes_, align_;
    std::vector<Chunk> chunks_;
    uint32_t current_ = UINT32_MAX;
    uint64_t retired_ = 0, recycles_ = 0;
    std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace runtime::video
