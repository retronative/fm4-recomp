// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (diagnostic, FM4_DIAG_IGNORE_IDLE=1, with FM4_IMMEDIATE_INTERRUPTS=1): an INTERRUPT after the guest's WAIT_UNTIL
// 3D-idle request is raised at once when no store is queued before it, as the reference port does (it ignores WAIT_UNTIL). NOT
// guest-exact: FM4 asks for 3D idle before these callbacks (reference/perf/memory_waits.md). For measuring the upper bound of
// relaxing that idle only (timing runs, never a default); resolve write-backs, deferred stores and fences keep their rules.
#pragma once
#include <cstdio>
#include <cstdlib>

namespace runtime::diag {

inline bool IgnoreIdle()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_IGNORE_IDLE");
        const bool set = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (set)
            std::fprintf(stderr, "[diag idle] FM4_DIAG_IGNORE_IDLE=1: the guest's 3D-idle requests do not hold back an INTERRUPT (DIAGNOSTIC, not "
                                 "guest-exact; with FM4_IMMEDIATE_INTERRUPTS=1)\n");
        return set;
    }();
    return on;
}

// FM4 Recomp (diagnostic, FM4_DIAG_EARLY_IRQ=1; NOT guest-exact, timing experiments only, never a default): every INTERRUPT is
// raised when the command processor reaches it, even with work open, a 3D-idle request before it, and stream stores (D3D's fences)
// still queued, so the guest's callback may run before those stores and the resolve write-backs land (stale data possible). A
// memory wait on a word that no pending write or queued store covers (the callback's acknowledgement) polls for the 2 ms grace
// before it drains, even with stores queued. Directional only: whether keeping the GPU queue fed across FM4's idle callbacks raises
// the clock. Counted at the stop line ("early interrupts").
inline bool EarlyIrq()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_EARLY_IRQ");
        const bool set = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (set)
            std::fprintf(stderr, "[diag idle] FM4_DIAG_EARLY_IRQ=1: interrupts raised when reached, before queued stores and write-backs land "
                                 "(DIAGNOSTIC, not guest-exact)\n");
        return set;
    }();
    return on;
}

// FM4 Recomp (diagnostic, FM4_DIAG_IDLE_CENSUS=1; changes nothing): for each INTERRUPT after a 3D-idle request, on every 25th swap
// (at most 3,000 lines), the deferred queue in front of it (stores, the newest store's packet and whether it is still in the open
// part), when and how it was raised, and each memory wait that syncs while it is queued (its word, and whether a queued store
// writes it). It bounds what relaxing the idle to "wait only for the queued stores" could gain: if the newest store before every
// such interrupt is in the open part, that rule waits for the same drain as today.
inline bool IdleCensus()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_IDLE_CENSUS");
        const bool set = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (set)
            std::fprintf(stderr, "[idle census] FM4_DIAG_IDLE_CENSUS=1: the queue in front of each idle-asked interrupt, every 25th swap\n");
        return set;
    }();
    return on;
}

}  // namespace runtime::diag
