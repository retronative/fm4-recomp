// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// T0 tests of the GPU safety components (runtime/gpu/), CPU only: no GPU device is created, and no
// invalid GPU work is ever executed (GPU safety); the failure transitions are tested on
// the CPU-side models instead. Built and run by tests/run_gpu_safety_tests.sh, which passes a work
// directory (argv[1]) for the GPU_STOP files.
//
// The submission gate and lifetime cases are taken from ForzaRecomp (forzarecomp-recarbon)
// tests/video/test_submission_gate.cpp and test_submission_lifetime.cpp
// (GPL-3.0-or-later, same owner), reviewed and re-tested here. FM4 Recomp changes: this file's
// Expect/pass/FAIL style; a stranded submission now blocks the gate (the device-loss case is
// inverted); the fence-timeout, loop-work, dump-ring and GPU_STOP cases are new. Every expected
// value is computed by hand in the comment next to it; "CONTROL" names a negative control (a wrong
// behaviour that must not happen).
#include "gpu/dump_ring.h"
#include "gpu/fence_wait.h"
#include "gpu/gpu_stop.h"
#include "gpu/loop_work.h"
#include "gpu/submission_gate.h"
#include "gpu/submission_lifetime.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace runtime::gpu;

namespace {
int failures = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}

using Outcome = SubmissionGate::Outcome;
constexpr uint64_t kMs = 1000000, kLimit = 6000 * kMs;

// A fake clock: each sleep advances it 1 ms, so the bounds are checked exactly and nothing waits for real.
struct FakeTime
{
    uint64_t now = 1000 * kMs;
    int sleeps = 0;
    Outcome Await(const SubmissionGate& g)
    {
        return g.AwaitIdle(kLimit, [this] { return now; }, [this] {
            now += kMs;
            sleeps++;
        });
    }
};

// Runs `body` on a thread of its own (the gate's per-thread count starts at zero there).
template <typename F>
void OnThread(F body)
{
    std::thread(body).join();
}

void TestSubmissionGate()
{
    {
        // A stop with nothing in flight: no wait at all.
        SubmissionGate g;
        Expect(g.BeginStop(), "gate: the first BeginStop begins the stop");
        Expect(!g.BeginStop(), "gate: a second BeginStop does not begin it again");
        FakeTime t;
        Expect(t.Await(g) == Outcome::Idle && t.sleeps == 0, "gate: nothing in flight, the stop returns at once");
        Expect(!g.Enter(), "gate CONTROL: nothing is submitted once the stop has begun");
        Expect(g.Load().stopping && g.Load().inFlight == 0, "gate: the refused submission is not counted");
    }
    {
        // Normal completion: the stop waits for another thread's submission until its fence signals.
        SubmissionGate g;
        std::atomic<bool> entered{false}, release{false};
        std::thread submitter([&] {
            Expect(g.Enter(), "gate: the submission enters before the stop");
            entered = true;
            while (!release)
                std::this_thread::yield();
            g.Leave();
        });
        while (!entered)
            std::this_thread::yield();
        g.BeginStop();
        Expect(!g.Enter(), "gate CONTROL: no new submission while the stop waits");
        FakeTime t;
        const Outcome o = g.AwaitIdle(kLimit, [&] { return t.now; }, [&] {
            t.now += kMs;
            if (++t.sleeps == 25)  // the fence signals after 25 ms of waiting
            {
                release = true;
                while (g.Load().inFlight != 0)
                    std::this_thread::yield();
            }
        });
        submitter.join();
        Expect(o == Outcome::Completed && t.sleeps == 25, "gate: the stop waited 25 ms for the submission in flight and saw it complete");
    }
    {
        // Timeout: a submission that never completes; the wait ends at its limit, not later.
        SubmissionGate g;
        OnThread([&] { g.Enter(); });  // entered on another thread, never left
        g.BeginStop();
        FakeTime t;
        const Outcome o = t.Await(g);
        Expect(o == Outcome::TimedOut, "gate: a submission still in flight at the limit times out");
        Expect(t.sleeps == int(kLimit / kMs), "gate: the wait ends exactly at its limit (6000 sleeps of 1 ms)");
    }
    {
        // Unknown completion (a lost device, a fence wait that timed out): stranded, reported, never waited for.
        SubmissionGate g;
        OnThread([&] {
            Expect(g.Enter(), "gate: a submission enters before it is stranded");
            g.MarkStranded();
        });
        Expect(g.Load().stranded && g.Load().inFlight == 1, "gate: a stranded submission stays counted");
        Expect(!g.Enter(), "gate CONTROL: nothing is submitted after a submission is stranded");
        Expect(g.Load().inFlight == 1, "gate: the submission refused after a strand is not counted");
        g.BeginStop();
        FakeTime t;
        Expect(t.Await(g) == Outcome::Stranded, "gate: a stranded submission is reported as stranded");
        Expect(t.sleeps == 0, "gate CONTROL: the stop does not wait for a stranded submission");
    }
    {
        // A stranded submission and another one still in flight: stranded at once.
        SubmissionGate g;
        OnThread([&] { g.Enter(); });
        OnThread([&] {
            g.Enter();
            g.MarkStranded();
        });
        g.BeginStop();
        FakeTime t;
        Expect(t.Await(g) == Outcome::Stranded && t.sleeps == 0, "gate: stranded with another in flight, reported with no wait");
    }
    {
        // Self-stop: the stopping thread's own submission is in flight; it must never wait for itself.
        SubmissionGate g;
        OnThread([&] {
            g.Enter();
            g.BeginStop();
            FakeTime t;
            Expect(t.Await(g) == Outcome::OwnThread, "gate: the stopping thread's own submission is reported, not waited for");
            Expect(t.sleeps == 0, "gate CONTROL: the stopping thread does not wait for its own submission");
            g.Leave();
        });
    }
    {
        // Self-stop between submissions on the submitting thread: nothing in flight, no wait.
        SubmissionGate g;
        OnThread([&] {
            g.Enter();
            g.Leave();
            g.BeginStop();
            FakeTime t;
            Expect(t.Await(g) == Outcome::Idle && t.sleeps == 0, "gate: a stop on the submitting thread between submissions does not wait");
        });
    }
    {
        // Concurrent stop and submission: whatever the interleaving, no submission enters after the
        // stop began, and the stop never returns while one is between Enter and Leave. Repeated.
        int violations = 0, lostAtStop = 0, entries = 0;
        for (int round = 0; round < 2000; round++)
        {
            SubmissionGate g;
            std::atomic<bool> go{false}, stopBegan{false}, stopReturned{false};
            std::atomic<int> inside{0}, insideAtStop{-1}, roundViolations{0}, roundEntries{0};
            Outcome o = Outcome::Idle;
            std::thread submitter([&] {
                while (!go)
                {
                }
                for (int i = 0; i < 50; i++)
                {
                    const bool begunBefore = stopBegan.load();
                    if (!g.Enter())
                        break;
                    if (begunBefore || stopReturned.load())
                        roundViolations++;
                    inside++;
                    roundEntries++;
                    inside--;
                    g.Leave();
                }
            });
            std::thread stopper([&] {
                while (!go)
                {
                }
                g.BeginStop();
                stopBegan = true;
                uint64_t now = 0;
                o = g.AwaitIdle(1000000 * kMs, [&] { return now; }, [&] {
                    now += kMs;
                    std::this_thread::yield();
                });
                insideAtStop = inside.load();
                stopReturned = true;
            });
            go = true;
            submitter.join();
            stopper.join();
            violations += roundViolations.load();
            entries += roundEntries.load();
            if ((o != Outcome::Idle && o != Outcome::Completed) || insideAtStop.load() != 0)
                lostAtStop++;
        }
        Expect(violations == 0, "gate CONTROL (race, 2000 rounds): no submission enters after the stop began");
        Expect(lostAtStop == 0, "gate CONTROL (race, 2000 rounds): the stop never returns while a submission is in flight");
        Expect(entries > 0, "gate: the race test did submit");
    }
}

using Lifetime = SubmissionLifetime<int>;

void TestSubmissionLifetime()
{
    {
        // The successful path: held while recorded and in flight, all released once confirmed.
        Lifetime l;
        Expect(l.Hold(1) && l.Hold(2) && l.Hold(3) && l.Held() == 3 && l.Reusable(), "lifetime: recording holds three resources");
        Expect(l.Submitted() && l.Current() == Lifetime::State::InFlight, "lifetime: submitted is in flight");
        Expect(!l.Reusable(), "lifetime CONTROL: not reusable while a submission is in flight");
        Expect(l.Held() == 3, "lifetime CONTROL: nothing is released at submission");
        Expect(!l.Hold(4), "lifetime CONTROL: no resource joins a submission in flight");
        Expect(!l.Submitted(), "lifetime CONTROL: a second submission is refused while one is in flight");
        Expect(l.Completed(Completion::Confirmed) == std::vector<int>{1, 2, 3} && l.Held() == 0 && l.Reusable(),
               "lifetime: confirmed releases every resource, oldest first");
        Expect(l.Completed(Completion::Confirmed).empty(), "lifetime: a completion with nothing in flight releases nothing");
        Expect(l.Hold(5), "lifetime: the next batch records");
        Expect(l.Completed(Completion::Confirmed).empty() && l.Held() == 1,
               "lifetime CONTROL: a completion before the batch is submitted releases nothing it holds");
        Expect(l.Submitted() && l.Completed(Completion::Confirmed) == std::vector<int>{5}, "lifetime: the next batch releases only its own");
    }
    {
        // A stranded completion strands everything: nothing released, no more work.
        Lifetime l;
        l.Hold(1);
        l.Hold(2);
        l.Submitted();
        Expect(l.Completed(Completion::Stranded).empty(), "lifetime CONTROL: nothing is released after a stranded completion");
        Expect(l.Current() == Lifetime::State::Stranded && l.Held() == 2 && !l.Reusable(), "lifetime: stranded keeps both resources");
        Expect(!l.Hold(3), "lifetime CONTROL: nothing is recorded while stranded");
        Expect(!l.Submitted(), "lifetime CONTROL: nothing is submitted while stranded");
        Expect(l.Completed(Completion::Confirmed).empty(), "lifetime CONTROL: a later confirmation without a submission releases nothing");
        Expect(l.Shutdown(Completion::Stranded).empty() && l.Held() == 2, "lifetime CONTROL: shutdown without a confirmed final wait keeps them");
        Expect(l.Shutdown(Completion::Confirmed) == std::vector<int>{1, 2} && l.Held() == 0, "lifetime: shutdown after a confirmed final wait releases");
    }
    {
        Lifetime l;
        l.Hold(7);
        Expect(l.Shutdown(Completion::Stranded) == std::vector<int>{7}, "lifetime: shutdown while idle releases what was never submitted");
    }
    {
        Lifetime l;
        l.Hold(8);
        l.Submitted();
        Expect(l.Shutdown(Completion::Stranded).empty(), "lifetime CONTROL: shutdown in flight without a confirmed wait releases nothing");
        Expect(l.Shutdown(Completion::Confirmed) == std::vector<int>{8}, "lifetime: shutdown in flight after a confirmed wait releases");
    }
    {
        // A submission that never reached the queue: stranded, kept, and no final wait on its fence.
        Lifetime l;
        l.Hold(9);
        Expect(l.Submitted() && l.Awaitable(), "lifetime: a submission handed to the queue has a fence to wait for");
        l.Rejected();
        Expect(l.Current() == Lifetime::State::Stranded && l.Held() == 1, "lifetime: rejected is stranded, its resource kept");
        Expect(!l.Awaitable(), "lifetime CONTROL: a fence that was never submitted is not waited for");
        Expect(!l.Submitted() && !l.Hold(10), "lifetime CONTROL: nothing more after a rejected submission");
    }
    {
        // A timed-out wait: stranded, but its fence reached the queue, so a final wait may confirm it.
        Lifetime l;
        l.Hold(11);
        l.Submitted();
        l.Completed(Completion::Stranded);
        Expect(l.Awaitable(), "lifetime: a timed-out submission's fence can still be waited for at shutdown");
        Expect(!Lifetime().Awaitable(), "lifetime: an idle holder has nothing to wait for");
    }
    {
        // Transitions the renderer never makes change nothing.
        Lifetime l;
        l.Hold(12);
        Expect(l.Completed(Completion::Stranded).empty() && l.Current() == Lifetime::State::Idle,
               "lifetime CONTROL: a stranded completion with nothing in flight does not strand an idle holder");
        l.Rejected();
        Expect(l.Current() == Lifetime::State::Idle && l.Held() == 1, "lifetime: a rejection with nothing in flight changes nothing");
    }
}

void TestFenceWait()
{
    // VkResult values, by hand from vulkan_core.h: VK_SUCCESS 0, VK_NOT_READY 1, VK_TIMEOUT 2, VK_ERROR_DEVICE_LOST -4.
    Expect(WaitOutcome(0, 0) == Completion::Confirmed, "fence: a successful wait with a signalled fence is confirmed");
    Expect(WaitOutcome(2, 1) == Completion::Stranded, "fence CONTROL: a timed-out wait is stranded");
    Expect(WaitOutcome(0, 1) == Completion::Stranded, "fence CONTROL: a successful wait with an unsignalled fence is stranded");
    Expect(WaitOutcome(-4, -4) == Completion::Stranded, "fence CONTROL: a lost device is stranded");
    Expect(WaitOutcome(-4, 0) == Completion::Stranded, "fence CONTROL: a failed wait is stranded even if the status reads signalled");
    Expect(WaitOutcome(1, 0) == Completion::Stranded, "fence CONTROL: VK_NOT_READY from the wait is stranded");

    const auto ms = [](const char* v) { return FenceTimeoutFromEnv(v).ms; };
    const auto bad = [](const char* v) { return FenceTimeoutFromEnv(v).malformed; };
    Expect(ms(nullptr) == 2000 && !bad(nullptr), "fence timeout: unset is 2000 ms (D-g)");
    Expect(ms("2500") == 2500 && !bad("2500"), "fence timeout: 2500 is 2500 ms");
    Expect(ms("100") == 100, "fence timeout: 100 (the minimum) is kept");
    Expect(ms("99") == 100, "fence timeout: 99 is clamped up to 100");
    Expect(ms("0") == 100, "fence timeout: 0 is clamped up to 100");
    Expect(ms("60000") == 60000, "fence timeout: 60000 (the maximum) is kept");
    Expect(ms("60001") == 60000, "fence timeout: 60001 is clamped down to 60000");
    Expect(ms("99999999999999999999999") == 60000 && !bad("99999999999999999999999"), "fence timeout: a 23-digit value saturates to 60000, never wraps");
    Expect(ms("") == 2000 && bad(""), "fence timeout: empty is malformed, default 2000");
    Expect(ms("5s") == 2000 && bad("5s"), "fence timeout: trailing text is malformed, default 2000");
    Expect(ms("-1") == 2000 && bad("-1"), "fence timeout: a sign is malformed, default 2000");
    Expect(ms(" 200") == 2000 && bad(" 200"), "fence timeout: a leading space is malformed, default 2000");
    Expect(FenceTimeoutNs(5000) == 5000000000ull, "fence timeout: 5000 ms is 5e9 ns");
    Expect(FenceTimeoutNs(60000) == 60000000000ull, "fence timeout: 60000 ms is 6e10 ns (no 32-bit overflow)");
}

void TestLoopWork()
{
    // Window scissor registers: x in bits 0-14, y in bits 16-30; pitch in RB_SURFACE_INFO bits 0-13.
    const auto xy = [](uint32_t x, uint32_t y) { return (y << 16) | x; };
    Expect(PixelInvocations(xy(0, 0), xy(1280, 720), 1280) == 921600, "loop work: a 1280x720 scissor is 921600 pixels");
    Expect(PixelInvocations(xy(100, 50), xy(132, 82), 1280) == 1024, "loop work: a 32x32 scissor at (100,50) is 1024 pixels");
    Expect(PixelInvocations(xy(0, 0), xy(1280, 720), 640) == 460800, "loop work: width clamped to the pitch 640 (640x720)");
    Expect(PixelInvocations(xy(0, 0), xy(10000, 9000), 1280) == 10485760, "loop work: width clamped to 1280, height to 8192 (1280x8192)");
    Expect(PixelInvocations(xy(0, 0), xy(100, 8192), 1280) == 819200, "loop work: height 8192 is kept (100x8192)");
    Expect(PixelInvocations(xy(100, 100), xy(50, 50), 1280) == 0, "loop work: an inverted scissor is 0 pixels");
    Expect(PixelInvocations(xy(10, 0), xy(10, 720), 1280) == 0, "loop work: a zero-width scissor is 0 pixels");
    // Bit 31 of TL (window offset disable) and bit 15 are not coordinates: TL masks to (0,0).
    Expect(PixelInvocations((1u << 31) | (1u << 15), xy(32, 16), 1280) == 512, "loop work: TL bits 15 and 31 are masked off (32x16)");
    // Pitch bits above 13 are other fields: 0x0001C500 & 0x3FFF = 0x0500 = 1280.
    Expect(PixelInvocations(xy(0, 0), xy(2000, 1), 0x0001C500u) == 1280, "loop work: surface info bits above 13 are masked off (pitch 1280)");

    const std::vector<uint32_t> none, loops64x64{64, 64};
    Expect(LoopWorkAllowed(none, uint64_t(1) << 40), "loop work: a stage without loops is not bounded");
    Expect(LoopWorkAllowed(loops64x64, 1024), "loop work: 64x64 loops x 1024 pixels = 4194304 is allowed");
    Expect(LoopWorkAllowed(loops64x64, 16384), "loop work: 64x64 x 16384 = 2^26 exactly is allowed");
    Expect(!LoopWorkAllowed(loops64x64, 16385), "loop work: 64x64 x 16385 = 2^26 + 4096 is refused");
    Expect(LoopWorkAllowed(std::vector<uint32_t>{1}, 67108864), "loop work: 1 x 2^26 is allowed");
    Expect(!LoopWorkAllowed(std::vector<uint32_t>{1}, 67108865), "loop work: 1 x (2^26 + 1) is refused");
    Expect(!LoopWorkAllowed(loops64x64, 921600), "loop work: 64x64 x 1280x720 = 3774873600 is refused");
    Expect(LoopWorkAllowed(loops64x64, 0), "loop work: no invocations is allowed");
    // A count of 0 counts as 1: {0, 64} x (2^20 + 1) = 64 x 1048577 = 67108928 > 2^26 (as 0 it would be 0).
    Expect(!LoopWorkAllowed(std::vector<uint32_t>{0, 64}, 1048577), "loop work: a count of 0 counts as 1 (refused)");
    Expect(LoopWorkAllowed(std::vector<uint32_t>{0, 64}, 1048576), "loop work: {0, 64} x 2^20 = 2^26 is allowed");
    // Only the low byte is the count: 0x12345640 is count 0x40 = 64; 64 x 2^20 = 2^26.
    Expect(LoopWorkAllowed(std::vector<uint32_t>{0x12345640u}, 1048576), "loop work: the count is the low byte of the loop constant");
    // 255^4 = 4228250625 > 2^26: refused by the loop product alone.
    Expect(!LoopWorkAllowed(std::vector<uint32_t>{255, 255, 255, 255}, 1), "loop work: 255^4 loops are refused for one invocation");
    // 255^9 overflows 64 bits (about 4.6e21): must still be refused, never wrap.
    Expect(!LoopWorkAllowed(std::vector<uint32_t>(9, 255), 1), "loop work: 255^9 loops are refused without overflow");
    // 64^4 x 4 = 2^26 work x 2^38 invocations = 2^64, which an unchecked product wraps to 0.
    Expect(!LoopWorkAllowed(std::vector<uint32_t>{64, 64, 64, 64, 4}, uint64_t(1) << 38), "loop work: 2^26 x 2^38 is refused (an unchecked product wraps to 0)");
    Expect(LoopWorkAllowed(std::vector<uint32_t>{64, 64, 64, 64, 4}, 1), "loop work: 2^26 work x 1 is allowed");
}

void TestDumpRing()
{
    using Ring = DumpRing<4>;
    Ring r(100);
    bool firstFour = true;
    for (uint64_t id = 1; id <= 4; id++)
    {
        const Ring::Pushed q = r.Push({id, id * 10});
        if (!q.accepted || !q.evicted.empty())
            firstFour = false;
    }
    Expect(firstFour, "dump ring: records 1-4 (10, 20, 30, 40 bytes) are accepted, nothing evicted");
    Expect(r.Count() == 4 && r.Bytes() == 100, "dump ring: four records, 100 bytes");
    // Full by count: record 5 (0 bytes, within the byte cap: 100 + 0 = 100) evicts record 1 only. Kept: 2,3,4,5 = 20+30+40+0 = 90 bytes.
    Ring::Pushed p = r.Push({5, 0});
    Expect(p.accepted && p.evicted.size() == 1 && p.evicted[0].id == 1, "dump ring: a fifth record evicts the oldest (record 1)");
    Expect(r.Count() == 4 && r.Bytes() == 90 && r.At(0).id == 2 && r.At(3).id == 5, "dump ring: records 2-5 kept, 90 bytes");
    // By bytes: record 6 (50 bytes): 90 + 50 = 140 > 100, evict 2 (70 + 50 = 120), evict 3 (40 + 50 = 90). Kept 4,5,6.
    p = r.Push({6, 50});
    Expect(p.accepted && p.evicted.size() == 2 && p.evicted[0].id == 2 && p.evicted[1].id == 3, "dump ring: 50 bytes evict records 2 and 3, oldest first");
    Expect(r.Count() == 3 && r.Bytes() == 90 && r.At(0).id == 4 && r.At(2).id == 6, "dump ring: records 4, 5, 6 kept, 90 bytes");
    // Larger than the whole cap: refused, nothing evicted.
    p = r.Push({7, 101});
    Expect(!p.accepted && p.evicted.empty() && r.Count() == 3 && r.Bytes() == 90, "dump ring CONTROL: a 101-byte record is refused and evicts nothing");
    // Exactly the cap: evicts everything else and is kept alone.
    p = r.Push({8, 100});
    Expect(p.accepted && p.evicted.size() == 3 && r.Count() == 1 && r.Bytes() == 100 && r.At(0).id == 8, "dump ring: a 100-byte record evicts all three and is kept alone");

    // Many records: the limits hold after every push (sizes 0..29 bytes, cycling).
    Ring many(50);
    bool within = true;
    uint64_t evicted = 0, accepted = 0;
    for (uint64_t id = 1; id <= 1000; id++)
    {
        const Ring::Pushed q = many.Push({id, id % 30});
        accepted += q.accepted;
        evicted += q.evicted.size();
        if (many.Count() > 4 || many.Bytes() > 50)
            within = false;
    }
    Expect(within, "dump ring CONTROL: never more than 4 records or 50 bytes after any of 1000 pushes");
    Expect(accepted == 1000 && accepted - evicted == many.Count(), "dump ring: every record is either kept or evicted exactly once");
}

void TestGpuStop(const std::string& dir)
{
    const std::string missing = dir + "/GPU_STOP_missing", flag = dir + "/GPU_STOP", empty = dir + "/GPU_STOP_empty",
                      locked = dir + "/GPU_STOP_locked", notDir = flag + "/GPU_STOP";
    GpuStopCheck c = CheckGpuStop(missing.c_str());
    Expect(!c.stopped && c.reason.empty(), "gpu stop: no file, not stopped");

    if (FILE* f = std::fopen(flag.c_str(), "w"))
    {
        std::fputs("lockup at 12:34 (watchdog)\r\nsecond line\n", f);
        std::fclose(f);
    }
    c = CheckGpuStop(flag.c_str());
    Expect(c.stopped && c.reason == "lockup at 12:34 (watchdog)", "gpu stop: the flag stops with its first line, line end removed");

    if (FILE* f = std::fopen(empty.c_str(), "w"))
        std::fclose(f);
    c = CheckGpuStop(empty.c_str());
    Expect(c.stopped && c.reason.empty(), "gpu stop: an empty flag file still stops");

    c = CheckGpuStop(nullptr);
    Expect(c.stopped && c.reason == "no GPU_STOP path given", "gpu stop CONTROL: no path refuses (no silent default)");
    c = CheckGpuStop("");
    Expect(c.stopped && c.reason == "no GPU_STOP path given", "gpu stop CONTROL: an empty path refuses");

    // A path under a regular file (ENOTDIR) cannot say whether the flag exists: refuse.
    c = CheckGpuStop(notDir.c_str());
    Expect(c.stopped && c.reason.find("cannot be opened") != std::string::npos, "gpu stop CONTROL: a path whose parent is a file refuses");

    // A flag that exists but cannot be read (mode 000; not meaningful as root, which reads anyway).
    if (FILE* f = std::fopen(locked.c_str(), "w"))
    {
        std::fputs("locked\n", f);
        std::fclose(f);
    }
    chmod(locked.c_str(), 0);
    c = CheckGpuStop(locked.c_str());
    Expect(c.stopped && (geteuid() == 0 || c.reason.find("cannot be opened") != std::string::npos), "gpu stop CONTROL: an unreadable flag refuses");
    chmod(locked.c_str(), 0600);
}
}  // namespace

int main(int argc, char** argv)
{
    alarm(60);  // the race test must never hang the suite
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // a FAIL line survives a later hang (the alarm) or abort
    std::printf("gpu safety tests: started\n");
    if (argc != 2)
    {
        std::printf("FAIL usage: test_gpu_safety <work dir>\n");
        return 2;
    }
    TestSubmissionGate();
    TestSubmissionLifetime();
    TestFenceWait();
    TestLoopWork();
    TestDumpRing();
    TestGpuStop(argv[1]);
    std::printf("gpu safety tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
