// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SpeedBreaker / ReCarbon contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Occlusion query reports (EVENT_WRITE_ZPD) counted on the GPU: the Vulkan-free bookkeeping (report kinds, the weight of a host
// sample, the open brackets, the finished segments' counts). Adapted from ReCarbon's runtime/gpu/zpd_report.h (GPL-3.0-or-later,
// commit 66401ea): Classify, Normalize, EndCount, BracketBook and ResultRing are its code; ReCarbon's END composition and store
// order are not taken (FM4 Recomp: FM4's D3D marks other words, below; the command processor stores the report as before, only the
// count is new). Tested alone by tests/zpd/test_zpd_report.cpp.
//
// FM4 Recomp, OBSERVED (research/m9/glare, g2_v_720_look_zpd, FM4_DIAG_ZPD_LOG): FM4 issues one query a race frame as two 64-byte
// blocks (its two predicated-tiling tiles), each a BEGIN ZPD at block+0x20 then an END ZPD at block+0x00, as ReCarbon's layout
// below; D3D's 0xFFFFFEED marker is in dwords 4 and 5 (Xenia's ZPass_A and ZPass_B, xenos.h:1547-1560) of the LAST tile's END
// report only, not in the four A lanes ReCarbon's game uses. Every BEGIN report is zeros.
//
// ReCarbon's notes (its game's D3D):
// D3D's Issue writes each query's reports into a 64-byte block per EDRAM
// tile: the BEGIN ZPD at block+0x20, the END ZPD at block+0x00 (the latched
// RB_SAMPLE_COUNT_ADDR). A report is 8 dwords, Total, ZFail, ZPass and
// StencilFail, each an A and a B lane, read little-endian (lwbrx). GetData
// returns the sum over tiles of END.ZPass(A+B) - BEGIN.ZPass(A+B). Every
// BEGIN report is all zeros, so an END report holds the samples of its own
// bracket: valid whenever it lands, even after the next BEGIN (a running
// counter would then go negative).
#pragma once
#include <cstdint>

namespace runtime::video::zpd {

    enum class Kind { Begin, End, Unknown };

    // By the report's offset in its block.
    inline Kind Classify(uint32_t address)
    {
        switch (address & 0x3F)
        {
        case 0x20: return Kind::Begin;
        case 0x00: return Kind::End;
        default: return Kind::Unknown;
        }
    }

    // Guest samples for `raw` host samples: `num` guest samples per host
    // pixel (the guest surface's MSAA; the host renders 1 sample), over
    // `den` host pixels per guest pixel (the render scale, sx * sy). Rounded
    // to nearest, at least 1 when anything passed (as Xenia does).
    inline uint64_t Normalize(uint64_t raw, uint32_t num, uint32_t den)
    {
        if (raw == 0)
            return 0;
        if (den == 0)
            den = 1;
        uint64_t guest = (raw * num + den / 2) / den;
        return guest ? guest : 1;
    }

    // FM4 Recomp: the weight of one host sample in guest samples, num / den, for Normalize: the guest surface's samples per pixel
    // (1, 2 or 4) over the host image's (the same, or 1 with FM4_HOST_MSAA_1X), over the host pixels per guest pixel (the internal
    // scale squared; render_scale.h counts it in halves: 2 = 1x, 3 = 1.5x, 4 = 2x, 6 = 3x). `hostSamples` divides `guestSamples`.
    struct Weight { uint32_t num, den; };
    constexpr Weight HostSampleWeight(uint32_t guestSamples, uint32_t hostSamples, uint32_t halves)
    {
        return {guestSamples / hostSamples * 4, halves * halves};
    }

    // An END's count from its segments' sum. A runaway one is rejected by
    // ReCarbon's game, which keeps its value; FM4's handling of such a count is
    // not known (UNRESOLVED), so it is saturated the same way, never cut to 32 bits.
    constexpr uint64_t kRunaway = 1u << 24;
    inline uint32_t EndCount(uint64_t sum)
    {
        return uint32_t(sum < kRunaway ? sum : kRunaway);
    }

    // A query bracket: BEGIN seen, END not yet. `s0` is the first counting
    // segment that can belong to it (segments are numbered in recording
    // order and never span a ZPD), `frame` when it was opened.
    struct Bracket
    {
        uint32_t beginAddress = 0;
        uint64_t s0 = 0, frame = 0;
    };

    // The open brackets, oldest first; the command processor's.
    class BracketBook
    {
    public:
        static constexpr uint32_t kMax = 8;
        enum class Opened { New, Replaced, DroppedOldest };

        // A BEGIN at `beginAddress`. One already open there (a query issued
        // again before its END) is replaced; beyond kMax the oldest is
        // dropped. `old`: the replaced or dropped bracket.
        Opened Open(uint32_t beginAddress, uint64_t s0, uint64_t frame, Bracket* old = nullptr)
        {
            Opened result = Opened::New;
            for (uint32_t i = 0; i < m_count; i++)
                if (m_open[i].beginAddress == beginAddress)
                {
                    if (old)
                        *old = m_open[i];
                    Erase(i);
                    result = Opened::Replaced;
                    break;
                }
            if (result == Opened::New && m_count == kMax)
            {
                if (old)
                    *old = m_open[0];
                Erase(0);
                result = Opened::DroppedOldest;
            }
            m_open[m_count++] = { beginAddress, s0, frame };
            return result;
        }

        // The END at `endAddress` closes the bracket its BEGIN opened 0x20
        // above it. False when none is open (its count is then 0).
        bool Close(uint32_t endAddress, Bracket* closed = nullptr)
        {
            for (uint32_t i = 0; i < m_count; i++)
                if (m_open[i].beginAddress == endAddress + 0x20)
                {
                    if (closed)
                        *closed = m_open[i];
                    Erase(i);
                    return true;
                }
            return false;
        }

        // Drops the brackets opened before `frame`: how many; `first`, the
        // oldest of them.
        uint32_t DropBefore(uint64_t frame, Bracket* first = nullptr)
        {
            uint32_t dropped = 0;
            for (uint32_t i = 0; i < m_count;)
            {
                if (m_open[i].frame >= frame)
                {
                    i++;
                    continue;
                }
                if (first && dropped == 0)
                    *first = m_open[i];
                dropped++;
                Erase(i);
            }
            return dropped;
        }

        bool Empty() const { return m_count == 0; }
        uint32_t Size() const { return m_count; }
        const Bracket& operator[](uint32_t i) const { return m_open[i]; }

    private:
        void Erase(uint32_t i)
        {
            for (; i + 1 < m_count; i++)
                m_open[i] = m_open[i + 1];
            m_count--;
        }

        Bracket m_open[kMax];
        uint32_t m_count = 0;
    };

    // Finished segments' counts by segment id, the last kSize of them (the
    // completion thread's). Each entry carries its id: a bracket summing an
    // id that was never stored, or was overwritten, counts it as lost.
    // Ids start at 1 (an empty entry's id is 0).
    class ResultRing
    {
    public:
        static constexpr uint32_t kSize = 4096;

        void Put(uint64_t id, uint64_t guest, uint64_t raw)
        {
            m_entries[id % kSize] = { id, guest, raw };
        }

        // Guest samples of segments [s0, s1); `lost` and `raw` (host
        // samples) are added to.
        uint64_t Sum(uint64_t s0, uint64_t s1, uint32_t& lost, uint64_t* raw = nullptr) const
        {
            uint64_t sum = 0;
            if (s1 > s0 && s1 - s0 > kSize)
            {
                lost += uint32_t(s1 - s0 - kSize);
                s0 = s1 - kSize;
            }
            for (uint64_t id = s0; id < s1; id++)
            {
                const Entry& e = m_entries[id % kSize];
                if (e.id != id || id == 0)
                {
                    lost++;
                    continue;
                }
                sum += e.guest;
                if (raw)
                    *raw += e.raw;
            }
            return sum;
        }

    private:
        struct Entry
        {
            uint64_t id = 0, guest = 0, raw = 0;
        };
        Entry m_entries[kSize];
    };
}  // namespace runtime::video::zpd
