// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, guest memory for the GPU): T0 tests of the GPU read guard (runtime/video/gpu_read_guard.h), CPU only: no Vulkan
// header, no device, no thread. A deterministic model stands for the rest: a fake clock (nanoseconds), a fake GPU that executes a
// part when the command processor retires it (reading the model's guest memory then), and a fake command processor that honours
// the guard's request the way renderer.cpp FlushForGpuReader does (PlanGuardFlush: submit the open part when it reads the range,
// then retire up to the reader, or the whole slot without partial retires). A waiting writer's sleep() is one step of that
// command processor, so every interleaving is fixed. Every expected order and value is derived by hand from the header's rules
// (G1-G8) in the comment next to it. Physical addresses are synthetic (no title address). "CONTROL" names a negative control.
//
//   tests/run_gpu_read_guard_tests.sh
#include "gpu_read_guard.h"
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using runtime::video::ChooseGuardPath;
using runtime::video::DirectBaseDword;
using runtime::video::GpuReadGuard;
using runtime::video::GuardFacts;
using runtime::video::GuardFlushPlan;
using runtime::video::GuardPath;
using runtime::video::PlanGuardFlush;

namespace {
int failures = 0, controls = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}
#define CONTROL(wrong, name)                \
    do                                      \
    {                                       \
        controls++;                         \
        Expect(!(wrong), "control: " name); \
    } while (0)

constexpr uint64_t kPhysical = 64ull << 20;     // a 64 MB model of physical memory (the real one is 512 MB)
constexpr uint32_t kGuestPage = 4096, kHostPage = 16384;
constexpr uint64_t kStepNs = 20000;             // one sleep: 20 us, as the fault handler's nanosleep
constexpr uint64_t kBoundNs = 5000000;          // the tests' wait bound: 5 ms = 250 steps

// The model: guest memory as one word per address used, the GPU's reads recorded per part, the command processor's state.
struct World
{
    GpuReadGuard guard{kPhysical, kGuestPage, kHostPage};
    uint64_t clock = 0;
    uint64_t open = 1;  // the open part's serial (render_hooks.h currentPartSerial)
    bool partialRetire = true;
    bool honours = true;  // the command processor takes requests (false: a wait loop that does not poll the guard)
    std::vector<uint64_t> inFlight;  // submitted, not retired
    std::vector<std::string> log;
    struct Read { uint64_t part; uint32_t address; uint32_t seen = 0; bool done = false; };
    std::vector<Read> reads;
    std::vector<std::pair<uint32_t, uint32_t>> memory;  // address -> value
    std::function<void()> extraStep;  // the command processor's other work in a step (a wait loop's poll, a split)

    uint32_t Load(uint32_t a) const
    {
        for (const auto& [address, value] : memory)
            if (address == a)
                return value;
        return 0;
    }
    void Store(uint32_t a, uint32_t v)
    {
        for (auto& [address, value] : memory)
            if (address == a)
            {
                value = v;
                return;
            }
        memory.push_back({a, v});
    }
    // A draw recorded into the open part reads `address` (the renderer: NoteReadThen at the entry, before recording).
    void RecordRead(uint32_t address, uint32_t bytes)
    {
        guard.NoteReadThen(address, bytes, open, [] { return true; });
        reads.push_back({open, address});
    }
    void Submit()
    {
        log.push_back("submit " + std::to_string(open));
        guard.Submitted(open);
        inFlight.push_back(open++);
    }
    // The GPU executes every submitted part up to `serial` (its reads see guest memory now), then the retire publishes it.
    void RetireUpTo(uint64_t serial)
    {
        uint64_t newest = 0;
        for (auto it = inFlight.begin(); it != inFlight.end();)
            if (*it <= serial)
            {
                for (Read& r : reads)
                    if (r.part == *it && !r.done)
                        r.seen = Load(r.address), r.done = true;
                log.push_back("complete " + std::to_string(*it));
                newest = *it > newest ? *it : newest;
                it = inFlight.erase(it);
            }
            else
                ++it;
        if (newest != 0)
            guard.Completed(newest);
    }
    // renderer.cpp FlushForGpuReader.
    void Flush(uint64_t reader)
    {
        const GuardFlushPlan plan = PlanGuardFlush(reader, guard.CompletedSerial(), open, partialRetire);
        if (!plan.flush)
            return;
        if (plan.submitOpen || plan.wholeSlot)
            Submit();
        RetireUpTo(plan.wholeSlot ? open - 1 : plan.retireUpTo);
    }
    void Step()
    {
        clock += kStepNs;
        if (extraStep)
            extraStep();
        if (honours)
            if (const uint64_t r = guard.TakeRequest(); r != 0)
                Flush(r);
    }
    // A guest store through the guard (the fault handler: EnterWrite, the page opened and the write recorded, LeaveWrite).
    GpuReadGuard::WaitResult GuardedStore(uint32_t address, uint32_t value)
    {
        const auto r = guard.EnterWrite(address, 4, kBoundNs, [this] { return clock; }, [this] { Step(); });
        if (r == GpuReadGuard::WaitResult::Done)
        {
            Store(address, value);
            log.push_back("write " + std::to_string(value));
            guard.LeaveWrite();
        }
        return r;
    }
    size_t At(const std::string& event) const
    {
        for (size_t i = 0; i < log.size(); i++)
            if (log[i] == event)
                return i;
        return SIZE_MAX;
    }
};

constexpr uint32_t kX = 0x00100000, kY = 0x00200000;

// A store to a range whose read is recorded but not submitted: Plan SubmitThenWait (G3: reader 1 > submitted 0); the command
// processor submits part 1 and retires it; the GPU reads X = 7 (the old value) at execution; the store lands after: log order
// submit 1, complete 1, write 9. One wait, one forced submit.
void TestUnsubmittedRead()
{
    World w;
    w.Store(kX, 7);
    w.RecordRead(kX, 64);
    Expect(w.guard.Plan(w.guard.WriteReader(kX, 4)) == GpuReadGuard::Action::SubmitThenWait, "unsubmitted: the store must force a submit");
    Expect(w.GuardedStore(kX, 9) == GpuReadGuard::WaitResult::Done, "unsubmitted: the store lands");
    Expect(w.At("submit 1") < w.At("complete 1") && w.At("complete 1") < w.At("write 9"), "unsubmitted: submit 1, complete 1, then the write");
    Expect(w.reads[0].done && w.reads[0].seen == 7, "unsubmitted: the GPU read the old bytes (7)");
    Expect(w.guard.GetStats().waits == 1 && w.guard.GetStats().forcedSubmits == 1, "unsubmitted: one wait, one forced submit");
    // The same store without the guard lands first; the GPU then reads 9.
    World raw;
    raw.Store(kX, 7);
    raw.RecordRead(kX, 64);
    raw.Store(kX, 9);
    raw.Submit(), raw.RetireUpTo(1);
    CONTROL(raw.reads[0].seen == 7, "an unguarded store before the GPU read is seen by it (the model can tell)");
}

// The read's part is submitted and in flight (submitted 1, completed 0): Plan Wait, no forced submit; the store lands after
// complete 1.
void TestInFlightRead()
{
    World w;
    w.Store(kX, 7);
    w.RecordRead(kX, 64);
    w.Submit();  // part 1 in flight; the open part is 2
    Expect(w.guard.Plan(w.guard.WriteReader(kX, 4)) == GpuReadGuard::Action::Wait, "in flight: the store waits (no submit needed)");
    Expect(w.GuardedStore(kX, 9) == GpuReadGuard::WaitResult::Done, "in flight: the store lands");
    Expect(w.At("complete 1") < w.At("write 9") && w.At("submit 2") == SIZE_MAX, "in flight: complete 1, then the write; part 2 is not submitted");
    Expect(w.reads[0].seen == 7 && w.guard.GetStats().forcedSubmits == 0, "in flight: the GPU read 7; no forced submit");
}

// The read's part completed: Proceed (reader 1 <= completed 1); no wait counted, no command processor step.
void TestRetiredRead()
{
    World w;
    w.RecordRead(kX, 64);
    w.Submit(), w.RetireUpTo(1);
    const uint64_t clock = w.clock;
    Expect(w.GuardedStore(kX, 9) == GpuReadGuard::WaitResult::Done && w.clock == clock && w.guard.GetStats().waits == 0,
           "retired: the store proceeds at once (no step, no wait counted)");
}

// Two reads of X at parts 3 and 1 (in that order: the per-page serial only rises, G1): the store waits for part 3. With partial
// retires the command processor retires up to 3; the write follows complete 3.
void TestOverlappingReads()
{
    World w;
    w.Submit(), w.Submit();  // parts 1, 2 (nothing read); open 3
    w.RecordRead(kX, 64);    // part 3
    w.guard.NoteRead(kX, 64, 1);
    Expect(w.guard.WriteReader(kX, 4) == 3, "overlap: the newest reader of X is part 3 (a later NoteRead of part 1 does not lower it)");
    Expect(w.GuardedStore(kX, 9) == GpuReadGuard::WaitResult::Done && w.At("complete 3") < w.At("write 9"), "overlap: the write follows complete 3");
    CONTROL(w.At("write 9") < w.At("complete 3"), "the write lands while part 3 still reads X");
}

// Partial page overlap: a read of guest page 0 of host page 0 ([0, 4096)). A store to 0x3000 (guest page 3, the same 16 KB host
// page, which the fault opens whole) waits; a store to 0x4000 (the next host page) does not.
void TestPartialPage()
{
    World w;
    w.RecordRead(0x0000, 4096);
    Expect(w.guard.WriteReader(0x3000, 4) == 1, "partial page: a store to 0x3000 shares host page 0 with the read: reader part 1");
    Expect(w.guard.WriteReader(0x4000, 4) == 0, "partial page: 0x4000 is the next host page: no reader");
    Expect(w.guard.Busy(0x3FFC, 8), "partial page: a write straddling into host page 1 still touches host page 0");
    CONTROL(w.guard.WriteReader(0x3000, 4) == 0, "only the written guest page is consulted (the fault opens the whole host page)");
}

// The command processor's own store (renderer.cpp BeforeCpWrite): inline, no sleep: the flush submits and retires part 1, then the
// store; the reader is completed after the flush (no wait loop, so no deadlock possible).
void TestCpInlineWrite()
{
    World w;
    w.Store(kX, 7);
    w.RecordRead(kX, 64);
    const uint64_t reader = w.guard.WriteReader(kX, 4);
    Expect(w.guard.Plan(reader) == GpuReadGuard::Action::SubmitThenWait, "CP store: the open part reads it");
    w.Flush(reader);
    Expect(w.guard.CompletedSerial() >= reader, "CP store: completed after the inline flush");
    w.Store(kX, 9), w.log.push_back("write 9");
    Expect(w.At("complete 1") < w.At("write 9") && w.reads[0].seen == 7, "CP store: the GPU read 7 before the store");
}

// The command processor is in a WAIT_REG_MEM loop for word W, which the guest thread writes only after its store to X; the store
// waits for part 1, which the open slot holds. The wait loop polls the guard (command_processor.cpp HonourGpuReadGuard): it
// submits and retires part 1; the store lands, then W; the wait ends. Without the poll the store times out at the bound (a stop,
// never a hang).
void TestCpBlockedInWait()
{
    constexpr uint32_t kW = 0x00300000;
    World w;
    w.RecordRead(kX, 64);
    bool waitDone = false;
    w.extraStep = [&] { waitDone = waitDone || w.Load(kW) == 1; };  // the wait loop's poll of W
    Expect(w.GuardedStore(kX, 9) == GpuReadGuard::WaitResult::Done, "blocked CP: the store lands (the wait loop honours the request)");
    w.Store(kW, 1);  // the guest thread's next store, which the CP waits for
    w.Step();
    Expect(waitDone && w.At("complete 1") < w.At("write 9"), "blocked CP: complete 1, the store, then the wait ends");
    World stuck;
    stuck.RecordRead(kX, 64);
    stuck.honours = false;
    const auto r = stuck.GuardedStore(kX, 9);
    Expect(r == GpuReadGuard::WaitResult::TimedOut && stuck.guard.GetStats().timeouts == 1 && stuck.clock > kBoundNs,
           "a wait loop that does not poll: the store times out at the bound (5 ms)");
    CONTROL(r == GpuReadGuard::WaitResult::Done, "a store lands while nothing retired the part that reads it");
}

// G4: the waiter needs part 1; in one step the command processor notes the read again for part 2 (a split moved the draw) and
// then publishes part 1 complete. The waiter must keep waiting for part 2.
void TestSplitMovesRead()
{
    World w;
    w.RecordRead(kX, 64);  // part 1 at the entry
    w.honours = false;
    int step = 0;
    w.extraStep = [&] {
        if (++step == 1)
        {
            w.Submit();                       // the split: part 1 submitted, the draw recorded in part 2
            w.guard.NoteRead(kX, 64, w.open);  // step 7: noted again for part 2
            w.RetireUpTo(1);                  // part 1 completes
        }
        else if (step == 3)
            w.Submit(), w.RetireUpTo(2);
    };
    Expect(w.GuardedStore(kX, 9) == GpuReadGuard::WaitResult::Done && w.At("complete 2") < w.At("write 9"),
           "split: the store waits for part 2, not the part the read was first noted for");
}

// Without partial retires the whole slot is submitted and retired (wholeSlot); the write follows.
void TestWholeSlotFlush()
{
    World w;
    w.partialRetire = false;
    w.RecordRead(kX, 64);
    w.Submit();
    w.RecordRead(kY, 64);  // part 2, open
    Expect(w.GuardedStore(kX, 9) == GpuReadGuard::WaitResult::Done, "whole slot: the store lands");
    Expect(w.At("submit 2") < w.At("write 9") && w.At("complete 2") < w.At("write 9"), "whole slot: part 2 is submitted and retired too");
}

void TestFlushPlan()
{
    const GuardFlushPlan a = PlanGuardFlush(5, 3, 6, true);
    Expect(a.flush && !a.submitOpen && !a.wholeSlot && a.retireUpTo == 5, "plan: reader 5 submitted (open 6): retire up to 5 only");
    const GuardFlushPlan b = PlanGuardFlush(6, 3, 6, true);
    Expect(b.flush && b.submitOpen && !b.wholeSlot && b.retireUpTo == 6, "plan: reader 6 is the open part: submit it, retire up to 6");
    const GuardFlushPlan c = PlanGuardFlush(5, 3, 6, false);
    Expect(c.flush && c.wholeSlot, "plan: without partial retires, the whole slot");
    CONTROL(PlanGuardFlush(3, 3, 6, true).flush, "a completed reader causes a flush");
}

void TestBoundsAndStrand()
{
    World w;
    w.RecordRead(kX, 64);
    w.guard.Strand();
    Expect(w.GuardedStore(kX, 9) == GpuReadGuard::WaitResult::Stranded && w.clock == 0, "stranded: the wait ends at once (no step)");
    Expect(!w.guard.GateHeld(), "stranded: the gate is not held");
}

// G8: the check runs under the gate; EnterWrite returns Done holding it; LeaveWrite clears it.
void TestGate()
{
    World w;
    bool heldInCheck = false;
    w.guard.NoteReadThen(kX, 64, 1, [&] { return heldInCheck = w.guard.GateHeld(); });
    Expect(heldInCheck && !w.guard.GateHeld(), "gate: held while the protection check runs, released after");
    w.Submit(), w.RetireUpTo(1);
    Expect(w.guard.EnterWrite(kX, 4, kBoundNs, [&] { return w.clock; }, [&] { w.Step(); }) == GpuReadGuard::WaitResult::Done && w.guard.GateHeld(),
           "gate: EnterWrite returns holding it");
    w.guard.LeaveWrite();
    Expect(!w.guard.GateHeld(), "gate: LeaveWrite releases it");
}

// G7, hand-computed: 0x0FFFFFF0 + 16 ends exactly at 256 MB (inside a 256 MB memory), + 32 does not.
void TestFallbackDecision()
{
    GuardFacts f{true, 0x00123450, 64, 256ull << 20, true, false};
    Expect(ChooseGuardPath(f) == GuardPath::Direct, "decision: inside, aligned, guarded, nothing pending: direct");
    Expect(DirectBaseDword(0x00123450) == 0x48D14, "decision: xe_vertex_base = 0x00123450 / 4 = 0x48D14");
    GuardFacts g = f;
    g.view = false;
    Expect(ChooseGuardPath(g) == GuardPath::NoView, "decision: no import: snapshot");
    g = f, g.physical = 0x0FFFFFF0, g.bytes = 16;
    Expect(ChooseGuardPath(g) == GuardPath::Direct, "decision: a window ending at the last byte is inside");
    g.bytes = 32;
    Expect(ChooseGuardPath(g) == GuardPath::Outside, "decision: past the end: snapshot");
    g = f, g.physical = 0x00123452;
    Expect(ChooseGuardPath(g) == GuardPath::Unaligned, "decision: not dword-aligned: snapshot");
    g = f, g.guarded = false;
    Expect(ChooseGuardPath(g) == GuardPath::NotGuarded, "decision: the watch cannot protect it (E0000000 view): snapshot");
    g = f, g.pendingWrite = true;
    Expect(ChooseGuardPath(g) == GuardPath::PendingWrite, "decision: a pending write-back over its host pages: snapshot");
    g = f, g.bytes = 0;
    CONTROL(ChooseGuardPath(g) == GuardPath::Direct, "an empty window is read directly");
}

}  // namespace

// FM4 Recomp (M7, FM4_GUARD_HOT): a store that waits marks its 16 KB host page hot (G-hot). kX = 0x00100000 is host page 64 (0x100000 /
// 0x4000); the store at kX + 4 waits for part 1, so page 64 (physical 0x00100000) becomes hot with reader 1; its neighbour page 65
// (0x00104000) stays cold; a later wait on the same page adds no second log entry. A reader that checks Hot first takes its copy
// path and notes nothing, so the next store to the page proceeds without a step. Off (no SetHotTracking), nothing is ever hot.
void TestHotPages()
{
    World off;
    off.RecordRead(kX, 64);
    Expect(off.GuardedStore(kX + 4, 9) == GpuReadGuard::WaitResult::Done && off.guard.GetStats().waits == 1, "hot off: the store waits as before");
    CONTROL(off.guard.Hot(kX, 4) || off.guard.HotPages() != 0, "hot off: a waited-for page is not hot");
    World w;
    w.guard.SetHotTracking(true);
    Expect(!w.guard.Hot(kX, 4), "hot: nothing is hot before a store waits");
    w.RecordRead(kX, 64);
    Expect(w.GuardedStore(kX + 4, 9) == GpuReadGuard::WaitResult::Done, "hot: the first store waits and lands");
    uint32_t page = 0;
    uint64_t reader = 0;
    Expect(w.guard.Hot(kX, 4) && w.guard.Hot(kX + 0x3FFC, 4) && w.guard.HotPages() == 1 && w.guard.HotLogEntry(0, page, reader) && page == 0x00100000u &&
               reader == 1,
           "hot: host page 0x00100000 is hot after the wait, logged with reader part 1");
    CONTROL(w.guard.Hot(kX + 0x4000, 4), "hot: the neighbour host page 0x00104000 stays cold");
    Expect(w.guard.Hot(kX + 0x3FFC, 8), "hot: a range crossing into the hot page is hot");
    // A reader that honours Hot does not note the read; the next store proceeds at once.
    if (!w.guard.Hot(kX, 64))
        w.RecordRead(kX, 64);
    const uint64_t clock = w.clock;
    Expect(w.GuardedStore(kX + 8, 10) == GpuReadGuard::WaitResult::Done && w.clock == clock && w.guard.GetStats().waits == 1,
           "hot: the next store to the hot page proceeds at once (no new read noted)");
    // A second wait on the same page (a read noted anyway) logs nothing new.
    w.RecordRead(kX, 64);
    Expect(w.GuardedStore(kX + 12, 11) == GpuReadGuard::WaitResult::Done && w.guard.HotPages() == 1 && !w.guard.HotLogEntry(1, page, reader),
           "hot: a second wait on the hot page adds no log entry");
}

// FM4 Recomp (M7, FM4_GUARD_HOT=2): a streaming ring (as observed: hot pages 8 host pages apart, one a frame). kX's host page 64 is made
// hot first (nothing hot behind it: no ring-ahead); then a store at kX + 0x20000 (host page 72, 8 pages later: page 64 is within
// kRingBack 16 behind) makes pages 73-328 hot too (kRingAhead 256): the next step at kX + 0x40000 (page 80) is already hot, so a reader
// honouring Hot notes nothing there and its store proceeds at once. Mode 1 does not mark ahead.
void TestRingAhead()
{
    for (const bool ahead : {true, false})
    {
        World w;
        w.guard.SetHotTracking(true, ahead);
        w.RecordRead(kX, 64);
        Expect(w.GuardedStore(kX, 1) == GpuReadGuard::WaitResult::Done && w.guard.RingAheadPages() == 0, "ring: the first hot page marks nothing ahead");
        w.RecordRead(kX + 0x20000, 64);
        Expect(w.GuardedStore(kX + 0x20000, 2) == GpuReadGuard::WaitResult::Done && w.guard.GetStats().waits == 2, "ring: the second step waits once");
        if (ahead)
        {
            Expect(w.guard.RingAheadPages() == 256 && w.guard.Hot(kX + 0x40000, 4) && w.guard.Hot(kX + 0x20000 + 256 * 0x4000, 4),
                   "ring ahead: pages 73-328 are hot after page 72 (8 behind it was hot)");
            CONTROL(w.guard.Hot(kX + 0x20000 + 257 * 0x4000, 4), "ring ahead: page 329 (past 256 ahead) stays cold");
            if (!w.guard.Hot(kX + 0x40000, 64))
                w.RecordRead(kX + 0x40000, 64);
            const uint64_t clock = w.clock;
            Expect(w.GuardedStore(kX + 0x40000, 3) == GpuReadGuard::WaitResult::Done && w.clock == clock && w.guard.GetStats().waits == 2,
                   "ring ahead: the third step's store proceeds at once");
        }
        else
            CONTROL(w.guard.RingAheadPages() != 0 || w.guard.Hot(kX + 0x40000, 4), "mode 1: nothing is marked ahead");
    }
    World far;  // a hot page 17 host pages after another (past kRingBack) marks nothing ahead
    far.guard.SetHotTracking(true, true);
    far.RecordRead(kX, 64);
    (void)far.GuardedStore(kX, 1);
    far.RecordRead(kX + 17 * 0x4000, 64);
    (void)far.GuardedStore(kX + 17 * 0x4000, 2);
    CONTROL(far.guard.RingAheadPages() != 0, "ring ahead: a hot page 17 pages back is not a ring step");
    // The command processor's own store that waited (renderer.cpp BeforeCpWrite) makes its page hot the same way.
    World cp;
    cp.guard.SetHotTracking(true);
    cp.guard.NoteWaitedStore(kY, 4, 3);
    uint32_t page = 0;
    uint64_t reader = 0;
    Expect(cp.guard.Hot(kY, 4) && cp.guard.HotLogEntry(0, page, reader) && page == kY && reader == 3, "hot: a command-processor store that waited makes its page hot");
}

int main()
{
    TestUnsubmittedRead();
    TestInFlightRead();
    TestRetiredRead();
    TestOverlappingReads();
    TestPartialPage();
    TestCpInlineWrite();
    TestHotPages();  // FM4 Recomp (M7, FM4_GUARD_HOT)
    TestRingAhead();  // FM4 Recomp (M7, FM4_GUARD_HOT=2)
    TestCpBlockedInWait();
    TestSplitMovesRead();
    TestWholeSlotFlush();
    TestFlushPlan();
    TestBoundsAndStrand();
    TestGate();
    TestFallbackDecision();
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures == 0 ? 0 : 1;
}
