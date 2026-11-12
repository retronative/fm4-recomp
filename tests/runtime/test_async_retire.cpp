// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, FM4_ASYNC_RETIRE): T0 tests of the completion worker (runtime/video/async_retire.h A1-A7), the deferred-store
// partition (runtime/kernel/deferred_store_parts.h P1-P5) and the GPU read guard's completion from the worker (gpu_read_guard.h G4,
// G8), CPU only: no Vulkan header, no device. The worker runs stepped (no thread): a fake clock, a fake GPU whose parts complete when
// the test says, and a command processor whose waits call one step of that world. One threaded smoke test at the end. Every expected
// order and value is derived by hand from the rules in the headers, in the comment next to it. "CONTROL" names a negative control.
//
//   tests/run_async_retire_tests.sh
#include "async_retire.h"
#include "deferred_store_parts.h"
#include "gpu_read_guard.h"
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using runtime::gpu::ApplyStoresThrough;
using runtime::gpu::StorePartition;
using runtime::video::AsyncRetireBackend;
using runtime::video::GpuReadGuard;
using runtime::video::SubmissionBackend;

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

constexpr uint32_t kSlots = 8;
constexpr uint64_t kTimeoutNs = 2000000000;  // the fence timeout: 2 s (fence_wait.h default)
constexpr uint64_t kMarginNs = 1000000000;   // the command processor's margin: 1 s (renderer.cpp PresenterWaitBoundNs(0))
constexpr uint64_t kStepNs = 1000000;        // one command-processor poll: 1 ms

// The fake GPU behind the worker: a part completes when the test marks it. WaitFence returns at once: VK_SUCCESS when complete,
// otherwise VK_TIMEOUT after advancing the clock by the timeout (what vkWaitForFences does on a fence that never signals).
struct World;
struct FakeGpu final : SubmissionBackend<int>
{
    World* w = nullptr;
    bool complete[kSlots] = {};
    int Submit(uint32_t slot) override;
    int WaitFence(uint32_t slot, uint64_t timeoutNs) override;
    int FenceStatus(uint32_t slot) override { return complete[slot] ? 0 : 1; }
    void Destroy(int&) override {}
};

struct World
{
    uint64_t clock = 0;
    std::vector<std::string> log;
    FakeGpu gpu;
    std::function<void()> step;  // the command processor's poll: what the world does in 1 ms (set per test)
    bool writeBackFails = false;
    std::function<void(uint64_t)> duringWriteBack;
    AsyncRetireBackend<int>* backend = nullptr;
    GpuReadGuard* guard = nullptr;

    AsyncRetireBackend<int>::Hooks Hooks()
    {
        AsyncRetireBackend<int>::Hooks h;
        h.completed = [this](uint64_t serial) {
            log.push_back("completed " + std::to_string(serial));
            if (guard)
                guard->Completed(serial);
        };
        h.writeBack = [this](uint64_t serial, std::string& error) {
            log.push_back("write-back " + std::to_string(serial));
            if (duringWriteBack)
                duringWriteBack(serial);
            if (writeBackFails)
                error = "resolve destination outside its view";
            return !writeBackFails;
        };
        h.present = [this](uint64_t swap, std::string&) {
            log.push_back("present " + std::to_string(swap));
            return true;
        };
        return h;
    }
    AsyncRetireBackend<int>::Clock Clock()
    {
        return {[this] { return clock; }, [this] {
                    clock += kStepNs;
                    if (step)
                        step();
                }};
    }
    void Submit(uint32_t slot, uint64_t serial, uint64_t presentSwap = 0)
    {
        backend->Tag(slot, serial, presentSwap);
        Expect(backend->Submit(slot) == 0, "setup: submit");
    }
    size_t Index(const std::string& entry) const
    {
        for (size_t i = 0; i < log.size(); i++)
            if (log[i] == entry)
                return i;
        return log.size();
    }
    bool Has(const std::string& entry) const { return Index(entry) != log.size(); }
};
int FakeGpu::Submit(uint32_t slot)
{
    complete[slot] = false;
    w->log.push_back("submit " + std::to_string(slot));
    return 0;
}
int FakeGpu::WaitFence(uint32_t slot, uint64_t timeoutNs)
{
    w->log.push_back("fence " + std::to_string(slot));
    if (complete[slot])
        return 0;
    w->clock += timeoutNs;
    return 2;
}

// A1, A2: two parts retire in submission order, each: fence, completed, write-back, then Retired.
void TestOrder()
{
    World w;
    w.gpu.w = &w;
    AsyncRetireBackend<int> b(w.gpu, kSlots, kTimeoutNs, kMarginNs, w.Hooks(), false, w.Clock());
    w.backend = &b;
    w.Submit(1, 10);
    w.Submit(2, 11);
    w.gpu.complete[1] = w.gpu.complete[2] = true;
    // A6: nothing retired yet, so FenceStatus is VK_NOT_READY although the fake fence is signalled.
    Expect(b.FenceStatus(1) == 1, "order: VK_NOT_READY before the worker retired the part");
    Expect(b.Step() && b.Step() && !b.Step(), "order: two parts, then an empty queue");
    const std::vector<std::string> expected = {"submit 1", "submit 2", "fence 1", "completed 10", "write-back 10", "fence 2", "completed 11", "write-back 11"};
    Expect(w.log == expected, "order: fence 1, completed 10, write-back 10, then part 11 (A1, A2)");
    Expect(w.Index("completed 10") < w.Index("write-back 10"), "order: the read guard's completion before the part's write-back");
    Expect(b.FenceStatus(1) == 0 && b.FenceStatus(2) == 0 && b.Retired(1), "order: both retired, VK_SUCCESS");
    CONTROL(w.Index("write-back 11") < w.Index("fence 1"), "the second part's write-back is not before the first part's fence");
}

// A3, A6: a drain (GpuGuard::Retire -> WaitFence) returns only after its part's write-back; during the write-back the slot is not
// retired yet (FenceStatus VK_NOT_READY), so neither a drain nor a reap can see it complete early.
void TestDrainAfterWriteBack()
{
    World w;
    w.gpu.w = &w;
    AsyncRetireBackend<int> b(w.gpu, kSlots, kTimeoutNs, kMarginNs, w.Hooks(), false, w.Clock());
    w.backend = &b;
    w.Submit(3, 20);
    int statusDuringWriteBack = -100;
    bool retiredDuringWriteBack = true;
    w.duringWriteBack = [&](uint64_t) { statusDuringWriteBack = b.FenceStatus(3), retiredDuringWriteBack = b.Retired(3); };
    uint32_t polls = 0;
    // The world: the GPU finishes the part after 5 polls of the command processor; the worker takes it at the next poll.
    w.step = [&] {
        if (++polls == 5)
            w.gpu.complete[3] = true;
        if (polls >= 6)
            b.Step();
    };
    const int r = b.WaitFence(3, kTimeoutNs);
    Expect(r == 0, "drain: VK_SUCCESS once the worker retired the part");
    Expect(w.Has("write-back 20"), "drain: returns only after the part's write-back (A3)");
    Expect(statusDuringWriteBack == 1 && !retiredDuringWriteBack, "drain: during the write-back the slot reads VK_NOT_READY (A6)");
    Expect(polls == 6, "drain: woken at the poll the worker retired it (5 polls, then the worker's)");
    CONTROL(r == 0 && !w.Has("fence 3"), "the drain does not succeed without the fence wait");
}

// A3: the command processor's bound restarts whenever the worker retires a part (one fence wait per part, as today): three parts that
// take 0.8 of the bound each are waited for in 2.4 bounds without a timeout; a worker that makes no progress times out at the bound.
void TestBound()
{
    World w;
    w.gpu.w = &w;
    AsyncRetireBackend<int> b(w.gpu, kSlots, kTimeoutNs, kMarginNs, w.Hooks(), false, w.Clock());
    w.backend = &b;
    for (uint32_t k = 1; k <= 3; k++)
        w.Submit(k, 30 + k);
    const uint64_t bound = kTimeoutNs + kMarginNs, each = bound / 10 * 8;
    uint64_t next = each;
    uint32_t done = 0;
    w.step = [&] {
        if (done < 3 && w.clock >= next)
            w.gpu.complete[++done] = true, b.Step(), next += each;
    };
    const uint64_t start = w.clock;
    Expect(b.WaitFence(3, kTimeoutNs) == 0, "bound: three slow parts, each within the bound: VK_SUCCESS");
    Expect(w.clock - start >= 2 * bound, "bound: the wait itself took more than two bounds (2.4 by construction)");
    // No progress at all: the worker is inside a fence wait that never returns (the step does nothing): VK_TIMEOUT after the bound.
    World stuck;
    stuck.gpu.w = &stuck;
    AsyncRetireBackend<int> s(stuck.gpu, kSlots, kTimeoutNs, kMarginNs, stuck.Hooks(), false, stuck.Clock());
    stuck.backend = &s;
    stuck.Submit(1, 40);
    const int r = s.WaitFence(1, kTimeoutNs);
    Expect(r == 2, "bound: no progress: VK_TIMEOUT (completion unknown)");
    Expect(stuck.clock > bound && stuck.clock <= bound + 2 * kStepNs, "bound: returned within one poll after timeout + margin");
    Expect(s.TakeCounters().cpTimeouts == 1, "bound: counted once");
    CONTROL(stuck.Has("completed 40"), "a part the worker never fenced is not completed");
}

// A4: a fence that times out is that part's verdict and every later part's, without waiting their fences; nothing of a failed part
// runs (no completion, no write-back, no present). The verdict strands in GpuGuard: WaitOutcome(2, FenceStatus) is not Confirmed.
void TestFenceFailure()
{
    World w;
    w.gpu.w = &w;
    AsyncRetireBackend<int> b(w.gpu, kSlots, kTimeoutNs, kMarginNs, w.Hooks(), false, w.Clock());
    w.backend = &b;
    w.Submit(1, 50, 7);  // a present part (swap 7) whose fence never signals
    w.Submit(2, 51);
    w.gpu.complete[2] = true;  // even a later part that did complete: its completion is not trusted after a failure (fail closed)
    b.Step(), b.Step();
    Expect(b.WaitFence(1, kTimeoutNs) == 2 && b.WaitFence(2, kTimeoutNs) == 2, "failure: VK_TIMEOUT for the part and the later part (A4)");
    Expect(!w.Has("fence 2"), "failure: the later part's fence is not waited for");
    Expect(!w.Has("completed 50") && !w.Has("write-back 50") && !w.Has("present 7"), "failure: nothing of the failed part runs");
    Expect(!w.Has("completed 51"), "failure: the later part is not published as complete to the read guard");
    Expect(runtime::gpu::WaitOutcome(b.WaitFence(1, kTimeoutNs), b.FenceStatus(1)) == runtime::gpu::Completion::Stranded,
           "failure: GpuGuard's outcome is Stranded (GPU_STOP, the existing stop)");
    Expect(b.Failed() == 2, "failure: remembered");
    CONTROL(b.FenceStatus(1) == 0, "a timed-out part never reads VK_SUCCESS");
}

// A5: a failed write-back is not a GPU failure: VK_SUCCESS (no stranding), the error kept for the slot, and no later job runs; the
// later part's fence is still waited for so its resources can be released.
void TestJobFailure()
{
    World w;
    w.gpu.w = &w;
    AsyncRetireBackend<int> b(w.gpu, kSlots, kTimeoutNs, kMarginNs, w.Hooks(), false, w.Clock());
    w.backend = &b;
    w.writeBackFails = true;
    w.Submit(1, 60);
    w.Submit(2, 61, 9);
    w.gpu.complete[1] = w.gpu.complete[2] = true;
    b.Step(), b.Step();
    Expect(b.WaitFence(1, kTimeoutNs) == 0 && b.FenceStatus(1) == 0, "job failure: the fence verdict stays VK_SUCCESS (no GPU_STOP)");
    Expect(b.Error(1).find("outside its view") != std::string::npos, "job failure: the error is kept for the slot, named");
    Expect(!w.Has("write-back 61") && !w.Has("present 9"), "job failure: no later write-back or present runs (fail closed)");
    Expect(w.Has("fence 2") && w.Has("completed 61") && !b.Error(2).empty(), "job failure: the later fence is waited for; its slot carries the error");
    CONTROL(b.Failed() != 0, "a job failure is not a fence failure");
}

// A2: a present part hands its frame over after its fence and its write-back, never before the fence.
void TestPresentAfterCompletion()
{
    World w;
    w.gpu.w = &w;
    AsyncRetireBackend<int> b(w.gpu, kSlots, kTimeoutNs, kMarginNs, w.Hooks(), false, w.Clock());
    w.backend = &b;
    w.Submit(4, 70, 12);
    Expect(!w.Has("present 12"), "present: nothing at submission");
    w.gpu.complete[4] = true;
    b.Step();
    Expect(w.Index("fence 4") < w.Index("present 12") && w.Index("write-back 70") < w.Index("present 12"), "present: after the fence and the write-back");
    Expect(b.Retired(4) && b.Error(4).empty(), "present: retired, no error");
    w.Submit(5, 71);  // an ordinary part presents nothing
    w.gpu.complete[5] = true;
    b.Step();
    CONTROL(w.Has("present 0"), "an ordinary part presents nothing");
}

// G4/G8 with the worker publishing Completed: a guest store to a page part 1 reads waits until the worker's completion of part 1;
// after that, a read the command processor notes for part 2 makes the next store wait for part 2 (nothing is lost).
void TestReadGuardFromWorker()
{
    GpuReadGuard guard(64ull << 20, 4096, 16384);
    World w;
    w.gpu.w = &w;
    w.guard = &guard;
    AsyncRetireBackend<int> b(w.gpu, kSlots, kTimeoutNs, kMarginNs, w.Hooks(), false, w.Clock());
    w.backend = &b;
    constexpr uint32_t kPage = 0x00200000;
    guard.NoteRead(kPage, 64, 1);
    guard.Submitted(1);
    w.Submit(1, 1);
    uint32_t steps = 0;
    const auto now = [&] { return w.clock; };
    const auto sleep = [&] {
        w.clock += 20000;
        if (++steps == 3)
            w.gpu.complete[1] = true, b.Step();  // the worker retires part 1 at the writer's third sleep
    };
    const auto r1 = guard.EnterWrite(kPage + 8, 4, 5000000, now, sleep);
    Expect(r1 == GpuReadGuard::WaitResult::Done && steps == 3, "guard: the store waits until the worker's Completed(1)");
    guard.LeaveWrite();
    // The command processor notes a read for part 2 under the gate (G8) after the worker published Completed(1): the next store waits.
    const bool stillProtected = guard.NoteReadThen(kPage, 64, 2, [] { return true; });
    Expect(stillProtected, "guard: the read of part 2 is noted (no write since the range was protected)");
    uint32_t idle = 0;
    const auto r2 = guard.EnterWrite(kPage + 8, 4, 100000, now, [&] { w.clock += 20000, idle++; });
    Expect(r2 == GpuReadGuard::WaitResult::TimedOut && idle > 0, "guard: part 2 unretired: the next store is not released by Completed(1)");
    CONTROL(guard.CompletedSerial() != 1, "only part 1 is complete");
}

// P1-P5: the deferred-store partition.
struct Entry { uint32_t address, value; bool interrupt = false; };
void TestStorePartition()
{
    std::vector<Entry> queue;
    StorePartition parts;
    std::vector<std::string> landed;
    const auto queueStore = [&](uint32_t a, uint32_t v, bool irq = false) { queue.push_back({a, v, irq}), parts.Queued(); };
    const auto all = [](const Entry&) { return true; };
    const auto record = [&](const Entry& e) { landed.push_back((e.interrupt ? "irq " : "store ") + std::to_string(e.value)); };
    queueStore(0x100, 1), queueStore(0x104, 2);
    parts.Submitted(1);  // P1: entries 1, 2 follow part 1
    parts.Submitted(1);  // nothing new: no empty part
    queueStore(0x108, 3);
    parts.Submitted(2);
    queueStore(0x10C, 4);  // P2: after the last boundary (the open part 3)
    Expect(parts.Parts() == 2, "partition: two parts, no empty one");
    Expect(ApplyStoresThrough(queue, parts, 1, all, record) == 2 && landed == std::vector<std::string>{"store 1", "store 2"},
           "partition: through part 1: its two stores, not part 2's (P1)");
    Expect(ApplyStoresThrough(queue, parts, 9, all, record) == 1 && landed.back() == "store 3", "partition: through any later serial: part 2's store only");
    Expect(queue.size() == 1 && queue.front().value == 4, "partition: the open part's store stays queued (P2)");
    // P4: an interrupt stops the reap; the stores behind it wait too.
    queueStore(0, 0x1, true), queueStore(0x110, 6);
    parts.Submitted(3);
    Expect(ApplyStoresThrough(queue, parts, 3, all, record) == 1 && landed.back() == "store 4" && queue.size() == 2 && queue.front().interrupt,
           "partition: stops at the interrupt; it and the store behind it wait for the full drain (P4)");
    // The full drain (ApplyDeferredStores): every entry, the partition reset.
    for (const Entry& e : queue)
        record(e);
    queue.clear(), parts.TakenAll();
    Expect(landed.size() == 6 && landed[4] == "irq 1" && landed[5] == "store 6", "partition: the full drain raises the interrupt before the store behind it");
    // P5: a refused store stops everything behind it.
    queueStore(0x200, 7), queueStore(0x204, 8);
    parts.Submitted(4);
    const auto notFirst = [](const Entry& e) { return e.address != 0x200; };
    Expect(ApplyStoresThrough(queue, parts, 4, notFirst, record) == 0 && queue.size() == 2, "partition: a refused store and the one behind it stay (P5)");
    Expect(ApplyStoresThrough(queue, parts, 4, all, record) == 2 && landed.back() == "store 8", "partition: both land once allowed, in order");
    // P3: a nested apply (a store's own flush landing more) continues in queue order, each entry once.
    queueStore(0x300, 9), queueStore(0x304, 10), queueStore(0x308, 11);
    parts.Submitted(5);
    landed.clear();
    bool nested = false;
    std::function<void(const Entry&)> nesting = [&](const Entry& e) {
        landed.push_back("store " + std::to_string(e.value));
        if (!nested)
            nested = true, ApplyStoresThrough(queue, parts, 5, all, nesting);
    };
    ApplyStoresThrough(queue, parts, 5, all, nesting);
    Expect(landed == std::vector<std::string>{"store 9", "store 10", "store 11"}, "partition: a nested apply keeps queue order, each once (P3)");
    CONTROL(!queue.empty(), "everything landed");
}

// A threaded smoke test: the real worker thread, a GPU that completes at once; three drains in order, then Stop joins.
void TestThreaded()
{
    struct Instant final : SubmissionBackend<int>
    {
        int Submit(uint32_t) override { return 0; }
        int WaitFence(uint32_t, uint64_t) override { return 0; }
        int FenceStatus(uint32_t) override { return 0; }
        void Destroy(int&) override {}
    } instant;
    std::vector<uint64_t> written;  // touched by the worker only until Stop
    AsyncRetireBackend<int>::Hooks h;
    h.writeBack = [&](uint64_t serial, std::string&) { written.push_back(serial); return true; };
    AsyncRetireBackend<int> b(instant, kSlots, kTimeoutNs, kMarginNs, h);
    for (uint32_t k = 1; k <= 3; k++)
        b.Tag(k, 80 + k, 0), b.Submit(k);
    bool ok = true;
    for (uint32_t k = 1; k <= 3; k++)
        ok = ok && b.WaitFence(k, kTimeoutNs) == 0;
    b.Stop();
    Expect(ok && written == std::vector<uint64_t>{81, 82, 83}, "threaded: three parts retired in order, then joined");
    CONTROL(b.Submits() != 3, "three submissions");
}
}  // namespace

int main()
{
    TestOrder();
    TestDrainAfterWriteBack();
    TestBound();
    TestFenceFailure();
    TestJobFailure();
    TestPresentAfterCompletion();
    TestReadGuardFromWorker();
    TestStorePartition();
    TestThreaded();
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures == 0 ? 0 : 1;
}
