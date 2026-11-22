// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9, diagnostic FM4_DIAG_MEMORY=1): a ledger of the renderer's own GPU allocations, by kind, so memory can be reasoned
// about in ALLOCATED bytes (what each vkAllocateMemory asked for: VkMemoryRequirements::size) instead of the process footprint. On
// Apple silicon the two are different things and must not be added: device memory is ordinary process memory there (unified), so the
// footprint already contains whatever part of these allocations has been touched, plus the guest's memory and everything else;
// pages of an image that were never written are allocated but not in the footprint. Guest memory imported for the GPU
// (FM4_GUEST_MEMORY_GPU) is the same pages as the guest's RAM and is listed as shared, never added to the total.
//
// Pure (no Vulkan): tests/runtime/test_mem_ledger.cpp. Not thread-safe by itself: the renderer's units allocate on the command
// processor's thread (the presenter's swapchain is not in the ledger).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace runtime::video::mem {

enum class Kind : uint32_t
{
    EdramColor,          // EDRAM colour host targets (scaled above 1x)
    EdramDepth,          // EDRAM depth/stencil host targets
    ShadowColor,         // 1x resolve shadows of scaled colour targets
    ShadowDepth,         // 1x resolve shadows of scaled depth targets
    EdramUnit,           // the EDRAM unit's own images (sample dump, resident words, scaled resident words)
    Texture,             // texture images loaded from guest memory or written by resolves
    TextureTwin,         // scaled twins of resolve-written textures
    Display,             // the display image(s)
    Ring,                // per-slot upload rings (vertices, indices, constants, texture staging)
    VertexCache,         // the vertex cache buffer
    Staging,             // the EDRAM resolve read-back staging buffer
    Present,             // GPU present word buffers, the read-back buffer, per-swap staging
    Retired,             // images replaced by a larger one and kept until shutdown (GPU lifetime safety)
    GuestShared,         // guest memory imported for the GPU: the guest's own pages, NOT additional
    Count
};

inline const char* KindName(Kind k)
{
    static const char* const names[] = {"EDRAM colour targets", "EDRAM depth/stencil targets", "resolve shadows (colour)", "resolve shadows (depth/stencil)",
                                        "EDRAM unit images (dump, resident words)", "textures", "scaled texture twins", "display images",
                                        "upload rings (staging/upload)", "vertex cache", "resolve read-back staging", "present and read-back buffers",
                                        "replaced targets kept until shutdown", "guest memory imported for the GPU (shared, not additional)"};
    return names[uint32_t(k)];
}

struct Entry
{
    Kind kind = Kind::Count;
    uint64_t bytes = 0;
    std::string label;
};

class Ledger
{
public:
    // A new allocation; the returned id releases or re-labels it. 0 is never returned.
    uint64_t Add(Kind kind, uint64_t bytes, std::string label)
    {
        entries_[++next_] = {kind, bytes, std::move(label)};
        totals_[uint32_t(kind)] += bytes;
        peak_ = std::max(peak_, Allocated());
        return next_;
    }
    // Freed (id 0 or unknown: nothing).
    void Release(uint64_t id)
    {
        const auto it = entries_.find(id);
        if (it == entries_.end())
            return;
        totals_[uint32_t(it->second.kind)] -= it->second.bytes;
        entries_.erase(it);
    }
    // An allocation that stays allocated under another kind (a replaced target kept until shutdown).
    void Move(uint64_t id, Kind kind)
    {
        const auto it = entries_.find(id);
        if (it == entries_.end())
            return;
        totals_[uint32_t(it->second.kind)] -= it->second.bytes, totals_[uint32_t(kind)] += it->second.bytes;
        it->second.kind = kind;
    }
    uint64_t Total(Kind kind) const { return totals_[uint32_t(kind)]; }
    // Every kind but the shared guest memory: the bytes the renderer allocated on top of the guest's.
    uint64_t Allocated() const
    {
        uint64_t sum = 0;
        for (uint32_t k = 0; k < uint32_t(Kind::Count); k++)
            sum += k == uint32_t(Kind::GuestShared) ? 0 : totals_[k];
        return sum;
    }
    uint64_t Peak() const { return peak_; }
    size_t Count() const { return entries_.size(); }
    // The largest allocations first (ties: the older first), at most `n`.
    std::vector<Entry> Largest(size_t n) const
    {
        std::vector<std::pair<uint64_t, Entry>> all(entries_.begin(), entries_.end());
        std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        std::vector<Entry> out;
        for (size_t i = 0; i < all.size() && i < n; i++)
            out.push_back(all[i].second);
        return out;
    }
    // "[mem] <when>: ..." lines: the total, each kind that has bytes, the shared guest memory apart, the `n` largest allocations.
    std::string Report(const std::string& when, size_t n) const
    {
        char line[512];
        std::snprintf(line, sizeof line, "[mem] %s: %.1f MB allocated by the renderer in %zu allocation(s) (peak %.1f MB); process footprint is measured separately "
                                         "and already contains the touched part of this\n", when.c_str(), Allocated() / 1048576.0, entries_.size(), peak_ / 1048576.0);
        std::string text = line;
        for (uint32_t k = 0; k < uint32_t(Kind::Count); k++)
            if (totals_[k] != 0)
            {
                size_t count = 0;
                for (const auto& [id, e] : entries_)
                    count += e.kind == Kind(k) ? 1 : 0;
                std::snprintf(line, sizeof line, "[mem] %s:   %9.1f MB  %s (%zu)\n", when.c_str(), totals_[k] / 1048576.0, KindName(Kind(k)), count);
                text += line;
            }
        size_t rank = 0;
        for (const Entry& e : Largest(n))
        {
            std::snprintf(line, sizeof line, "[mem] %s:   largest %2zu: %8.1f MB  %s: %s\n", when.c_str(), ++rank, e.bytes / 1048576.0, KindName(e.kind), e.label.c_str());
            text += line;
        }
        return text;
    }

private:
    std::map<uint64_t, Entry> entries_;
    uint64_t totals_[uint32_t(Kind::Count)] = {};
    uint64_t next_ = 0, peak_ = 0;
};

// The process's ledger and whether FM4_DIAG_MEMORY=1 asked for its reports (the ledger itself always counts: a few map operations per
// allocation, none per draw).
inline Ledger& Global()
{
    static Ledger ledger;
    return ledger;
}
inline bool Wanted()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_MEMORY");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

}  // namespace runtime::video::mem
