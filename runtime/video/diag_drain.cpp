// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (diagnostic, FM4_DIAG_DRAIN=1): see diag_drain.h.
#include "diag_drain.h"
#include "kernel/command_processor.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>

namespace runtime::video::drain {
namespace {

constexpr uint32_t kMaxSlots = 16;

// Per slot, written by the thread named in diag_drain.h, read by the command processor after the slot retired (or as a snapshot at
// a drain's start). Zero: not reached yet.
struct SlotStamps
{
    std::atomic<uint64_t> serial{0}, draws{0}, handed{0}, lockNs{0}, submitBegin{0}, submitEnd{0}, fenceWaitBegin{0}, fenceObserved{0},
        writeBackBegin{0}, writeBackEnd{0};
};
SlotStamps stamps[kMaxSlots];
std::atomic<uint64_t> pendingLockNs{0};  // the submit worker: QueueLockWaited -> the next SubmitBegin

// One drain (command-processor thread only).
struct Drain
{
    uint32_t opcode = 0, lastSlot = 0;
    uint64_t serial = 0, lastDraws = 0;
    uint64_t waitStart = 0, flushBegin = 0, handed = 0, lockNs = 0, submitBegin = 0, submitEnd = 0, fenceWaitBegin = 0, fenceObserved = 0,
             writeBackBegin = 0, writeBackEnd = 0, retireObserved = 0, flushEnd = 0, waitEnd = 0, gpuSpanNs = 0;
    // At the drain's start: the open part's draws; parts handed but not yet through vkQueueSubmit; submitted parts whose fence was not
    // yet observed; parts whose fence was observed but not yet retired by the command processor.
    uint64_t openDraws = 0, queuedDraws = 0, runningDraws = 0;
    uint32_t queuedParts = 0, runningParts = 0, doneParts = 0;
    bool gpuSpan = false;
};
bool inDrain = false;
Drain current;
std::vector<Drain> swapDrains;
uint64_t detailPrinted = 0;

uint32_t DetailLimit()
{
    static const uint32_t n = [] {
        const char* v = std::getenv("FM4_DIAG_DRAIN_DETAIL");
        const unsigned long k = v != nullptr ? std::strtoul(v, nullptr, 10) : 30;
        return uint32_t(std::min<unsigned long>(k, 100000));
    }();
    return n;
}

// b - a in ms; NaN when either stamp is missing (the stage did not run on this path).
double Ms(uint64_t a, uint64_t b) { return a == 0 || b == 0 ? std::nan("") : (double(b) - double(a)) / 1e6; }

// The intervals of one drain, in the order printed (diag_drain.h).
// flush = cp->hand + submit queue + vkQueueSubmit + gpu after submit + fence->cp + cp after retire (consecutive stamps: they add up exactly
// when every stamp is present; a negative one means the stamps were taken out of the expected order).
enum Field { kWait, kPre, kFlush, kCpHand, kQueue, kLock, kSubmit, kGpu, kFenceLag, kFenceToCp, kWorkerJobs, kAfterRetire, kPoll, kFields };
const char* const kFieldNames[kFields] = {"wait", "pre", "flush", "cp->hand", "submit queue", "presenter lock", "vkQueueSubmit", "gpu after submit",
                                          "fence wait start lag", "fence->cp", "worker jobs", "cp after retire", "poll tail"};
void Fields(const Drain& d, double f[kFields])
{
    f[kWait] = Ms(d.waitStart, d.waitEnd);
    f[kPre] = Ms(d.waitStart, d.flushBegin);
    f[kFlush] = Ms(d.flushBegin, d.flushEnd);
    f[kCpHand] = Ms(d.flushBegin, d.handed);
    f[kQueue] = Ms(d.handed, d.submitBegin);
    f[kLock] = d.submitBegin != 0 ? d.lockNs / 1e6 : std::nan("");
    f[kSubmit] = Ms(d.submitBegin, d.submitEnd);
    f[kGpu] = Ms(d.submitEnd, d.fenceObserved);
    f[kFenceLag] = Ms(d.submitEnd, d.fenceWaitBegin);
    f[kFenceToCp] = Ms(d.fenceObserved, d.retireObserved);
    f[kWorkerJobs] = Ms(d.writeBackBegin, d.writeBackEnd);
    f[kAfterRetire] = Ms(d.retireObserved, d.flushEnd);
    f[kPoll] = Ms(d.flushEnd, d.waitEnd);
}

void PrintDetail(uint64_t swap, size_t index, const Drain& d)
{
    double f[kFields];
    Fields(d, f);
    std::fprintf(stderr,
                 "[diag drain] swap %llu drain %zu (packet %02X): wait %.3f ms = pre %.3f + flush %.3f + poll tail %.3f; flush: cp->hand %.3f, "
                 "submit queue %.3f (presenter lock %.3f), vkQueueSubmit %.3f, gpu after submit %.3f (fence wait started %.3f after submit), "
                 "fence->cp %.3f (worker write-back %.3f), cp after retire %.3f; at start: open part %llu draw(s), "
                 "%u part(s) %llu draw(s) not yet through vkQueueSubmit, %u part(s) %llu draw(s) running, %u part(s) complete not retired; "
                 "last part slot %u serial %llu (%llu draw(s)); GPU span %s\n",
                 (unsigned long long)swap, index, d.opcode, f[kWait], f[kPre], f[kFlush], f[kPoll], f[kCpHand], f[kQueue], f[kLock], f[kSubmit], f[kGpu],
                 f[kFenceLag], f[kFenceToCp], f[kWorkerJobs], f[kAfterRetire], (unsigned long long)d.openDraws, d.queuedParts,
                 (unsigned long long)d.queuedDraws, d.runningParts, (unsigned long long)d.runningDraws, d.doneParts, d.lastSlot,
                 (unsigned long long)d.serial, (unsigned long long)d.lastDraws,
                 d.gpuSpan ? (std::to_string(d.gpuSpanNs / 1000) + " us").c_str() : "n/a");
}

double Median(std::vector<double>& v)
{
    if (v.empty())
        return std::nan("");
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}

}  // namespace

bool Enabled()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_DRAIN");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[diag drain] diagnostic FM4_DIAG_DRAIN=1: each CPU-sync drain split into hand-off, submit, GPU, retire and "
                                 "poll tail; one line a swap, the first %u race drains individually (FM4_DIAG_DRAIN_DETAIL)\n", DetailLimit());
        return enabled;
    }();
    return on;
}

uint64_t NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void PartHanded(uint32_t slot, uint64_t serial, uint64_t draws)
{
    if (slot >= kMaxSlots)
        return;
    SlotStamps& s = stamps[slot];
    s.serial.store(serial), s.draws.store(draws);
    for (std::atomic<uint64_t>* a : {&s.lockNs, &s.submitBegin, &s.submitEnd, &s.fenceWaitBegin, &s.fenceObserved, &s.writeBackBegin, &s.writeBackEnd})
        a->store(0);
    s.handed.store(NowNs());  // last: a snapshot that sees it sees the cleared stamps
}
void QueueLockWaited(uint64_t ns) { pendingLockNs.store(ns); }
void SubmitBegin(uint32_t slot)
{
    if (slot < kMaxSlots)
        stamps[slot].lockNs.store(pendingLockNs.exchange(0)), stamps[slot].submitBegin.store(NowNs());
}
void SubmitEnd(uint32_t slot)
{
    if (slot < kMaxSlots)
        stamps[slot].submitEnd.store(NowNs());
}
void FenceWaitBegin(uint32_t slot)
{
    if (slot < kMaxSlots)
        stamps[slot].fenceWaitBegin.store(NowNs());
}
void FenceObserved(uint32_t slot)
{
    if (slot < kMaxSlots)
        stamps[slot].fenceObserved.store(NowNs());
}
void WriteBackBegin(uint64_t serial)
{
    for (SlotStamps& s : stamps)
        if (s.serial.load() == serial && s.handed.load() != 0)
            s.writeBackBegin.store(NowNs());
}
void WriteBackEnd(uint64_t serial)
{
    for (SlotStamps& s : stamps)
        if (s.serial.load() == serial && s.handed.load() != 0)
            s.writeBackEnd.store(NowNs());
}

void FlushBegin(uint32_t opcode, uint32_t lastSlot, uint64_t openDraws, const std::vector<uint32_t>& submittedSlots)
{
    current = Drain{};
    current.flushBegin = NowNs();
    current.opcode = opcode, current.lastSlot = lastSlot, current.openDraws = openDraws;
    for (const uint32_t k : submittedSlots)
    {
        if (k >= kMaxSlots)
            continue;
        const SlotStamps& s = stamps[k];
        if (s.fenceObserved.load() != 0)
            current.doneParts++;
        else if (s.submitEnd.load() != 0)
            current.runningParts++, current.runningDraws += s.draws.load();
        else
            current.queuedParts++, current.queuedDraws += s.draws.load();
    }
    inDrain = true;
}
void RetireObserved()
{
    if (inDrain && current.retireObserved == 0)
        current.retireObserved = NowNs();
}
void GpuSpan(uint64_t ns)
{
    if (inDrain)
        current.gpuSpan = true, current.gpuSpanNs = ns;
}
void FlushEnd()
{
    if (!inDrain)
        return;
    inDrain = false;
    current.flushEnd = NowNs();
    if (current.lastSlot < kMaxSlots)  // the last part retired: its stamps are final
    {
        const SlotStamps& s = stamps[current.lastSlot];
        current.serial = s.serial.load(), current.lastDraws = s.draws.load(), current.handed = s.handed.load(), current.lockNs = s.lockNs.load();
        current.submitBegin = s.submitBegin.load(), current.submitEnd = s.submitEnd.load();
        current.fenceWaitBegin = s.fenceWaitBegin.load(), current.fenceObserved = s.fenceObserved.load();
        current.writeBackBegin = s.writeBackBegin.load(), current.writeBackEnd = s.writeBackEnd.load();
    }
    swapDrains.push_back(current);
}

void Swap(uint64_t swap, uint64_t drawsSinceSwap)
{
    // The wait each drain synced in (the command processor's record; none for a sync point that is not a memory WAIT_REG_MEM).
    uint32_t matched = 0;
    for (Drain& d : swapDrains)
        if (uint64_t start = 0, end = 0; runtime::gpu::SyncedMemWaitAround(d.flushBegin, start, end) && end >= d.flushEnd)
            d.waitStart = start, d.waitEnd = end, matched++;
    const bool race = drawsSinceSwap >= 1000;
    for (size_t i = 0; i < swapDrains.size() && race && detailPrinted < DetailLimit(); i++, detailPrinted++)
        PrintDetail(swap, i, swapDrains[i]);
    std::string fields;
    std::vector<double> column[kFields];
    for (const Drain& d : swapDrains)
    {
        double f[kFields];
        Fields(d, f);
        for (int k = 0; k < kFields; k++)
            if (!std::isnan(f[k]))
                column[k].push_back(f[k]);
    }
    for (int k = 0; k < kFields; k++)
    {
        if (k == kLock || k == kFenceLag || k == kWorkerJobs)
            continue;  // in the detail lines only
        double sum = 0;
        for (const double v : column[k])
            sum += v;
        char part[96];
        std::snprintf(part, sizeof part, "%s %s %.2f/%.2f", fields.empty() ? "" : ",", kFieldNames[k], sum, Median(column[k]));
        fields += part;
    }
    uint64_t openDraws = 0, queuedDraws = 0, runningDraws = 0, gpuSpanNs = 0;
    uint32_t queuedParts = 0, runningParts = 0, gpuSpans = 0;
    for (const Drain& d : swapDrains)
    {
        openDraws += d.openDraws, queuedDraws += d.queuedDraws, runningDraws += d.runningDraws;
        queuedParts += d.queuedParts, runningParts += d.runningParts;
        if (d.gpuSpan)
            gpuSpanNs += d.gpuSpanNs, gpuSpans++;
    }
    std::fprintf(stderr, "[diag drain] swap %llu: %zu drain(s) (%u in a memory wait), %llu draw(s); ms sum/median:%s; at the drains' start (sum): "
                         "open parts %llu draw(s), %u part(s) %llu draw(s) not yet through vkQueueSubmit, %u part(s) %llu draw(s) running; "
                         "GPU span %.2f ms in %u drain(s)\n",
                 (unsigned long long)swap, swapDrains.size(), matched, (unsigned long long)drawsSinceSwap, fields.c_str(), (unsigned long long)openDraws,
                 queuedParts, (unsigned long long)queuedDraws, runningParts, (unsigned long long)runningDraws, gpuSpanNs / 1e6, gpuSpans);
    swapDrains.clear();
}

}  // namespace runtime::video::drain
