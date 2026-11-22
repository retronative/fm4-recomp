// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9 diagnostic, opt-in, off by default; the sun glare investigation, owner report 2026-11-22): what an occlusion
// query's end reports. EVENT_WRITE_ZPD answers Xenia's fixed 1000 passed samples (command_processor.cpp); this probe replaces that
// number inside one run to see whether a picture element (the sun's flare) follows it. A probe only, never a setting.
// FM4_DIAG_ZPD_SAMPLES=<n>[,<n>...] (each 0..1000000): the reported counts, one after another in windows of FM4_DIAG_ZPD_WINDOW
// swaps (default 240), repeating. Each switch is one log line with the swap, the new count and the window's event tallies.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace runtime::diag {

struct ZpdProbe
{
    std::vector<uint32_t> counts;
    uint64_t window = 240;
    uint64_t events = 0, ends = 0;  // in the current window
    long index = -1;
};

inline ZpdProbe& Zpd()
{
    static ZpdProbe probe = [] {
        ZpdProbe p;
        const char* v = std::getenv("FM4_DIAG_ZPD_SAMPLES");
        if (v == nullptr || *v == '\0')
            return p;
        for (const char* at = v; *at;)
        {
            char* end = nullptr;
            const unsigned long n = std::strtoul(at, &end, 10);
            if (end == at || n > 1000000 || (*end != ',' && *end != '\0'))
            {
                std::fprintf(stderr, "[diag zpd] FM4_DIAG_ZPD_SAMPLES '%s' is not a list of counts 0..1000000: off\n", v);
                p.counts.clear();
                return p;
            }
            p.counts.push_back(uint32_t(n));
            at = *end ? end + 1 : end;
        }
        if (const char* w = std::getenv("FM4_DIAG_ZPD_WINDOW"))
        {
            const unsigned long n = std::strtoul(w, nullptr, 10);
            if (n >= 1 && n <= 100000)
                p.window = n;
        }
        std::fprintf(stderr, "[diag zpd] on (FM4_DIAG_ZPD_SAMPLES=%s): an occlusion query's end reports these counts in turn, %llu swaps each (DIAGNOSTIC, not Xenia's 1000)\n",
                     v, (unsigned long long)p.window);
        return p;
    }();
    return probe;
}

// The passed-sample count an occlusion query's end reports at `swap`; `fallback` without the probe. `end`: this event is a query's end.
inline uint32_t ZpdSamples(uint64_t swap, bool end, uint32_t fallback)
{
    ZpdProbe& p = Zpd();
    if (p.counts.empty())
        return fallback;
    const long index = long((swap / p.window) % p.counts.size());
    if (index != p.index)
    {
        std::fprintf(stderr, "[diag zpd] swap %llu: reporting %u samples from here (the window before: %llu ZPD event(s), %llu query end(s))\n",
                     (unsigned long long)swap, p.counts[size_t(index)], (unsigned long long)p.events, (unsigned long long)p.ends);
        p.index = index, p.events = p.ends = 0;
    }
    p.events++, p.ends += end ? 1 : 0;
    return p.counts[size_t(index)];
}

// FM4_DIAG_ZPD_LOG=<lines> (1..100000): each EVENT_WRITE_ZPD as one line (the swap, RB_SAMPLE_COUNT_ADDR, whether D3D's finished
// marker makes it a query's end by Xenia's rule, and the report's eight dwords as the guest's bytes hold them before the event's
// own stores), from the first event at or after swap FM4_DIAG_ZPD_LOG_FROM (default 0).
inline void ZpdLog(uint64_t swap, uint32_t address, bool end, const uint8_t* report)
{
    static const long lines = [] { const char* v = std::getenv("FM4_DIAG_ZPD_LOG"); return v ? std::strtol(v, nullptr, 10) : 0L; }();
    static const uint64_t from = [] { const char* v = std::getenv("FM4_DIAG_ZPD_LOG_FROM"); return v ? std::strtoull(v, nullptr, 10) : 0ull; }();
    static long logged = 0;
    if (logged >= lines || lines > 100000 || swap < from)
        return;
    logged++;
    char text[8 * 9 + 1];
    for (int word = 0; word < 8; word++)
        std::snprintf(text + 9 * word, 10, "%02X%02X%02X%02X ", report[4 * word], report[4 * word + 1], report[4 * word + 2], report[4 * word + 3]);
    std::fprintf(stderr, "[diag zpd] swap %llu ZPD at %08X (%s): %s\n", (unsigned long long)swap, address, end ? "end" : "not an end", text);
}

}  // namespace runtime::diag
