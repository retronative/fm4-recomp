// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, guest memory for the GPU; opt-in FM4_GUEST_MEMORY_GPU=1 with FM4_WRITE_WATCH=1): the guard that lets the GPU
// read guest memory directly (guest_memory_vk.h) while guest stores and host writes stay ordered after every GPU read of the bytes
// they change. Vulkan-free and lock-free, so tests/runtime/test_gpu_read_guard.cpp covers it alone (tests/run_gpu_read_guard_tests.sh)
// and the fault handler may call it (atomics only: no allocation, no lock, no Vulkan call).
//
// Adapted from ReCarbon (GPL-3.0-or-later, read-only) runtime/kernel/write_watch.cpp: Guard (:840-878: the newest
// submission reading each 4 KB guest page, raised only, then the page protected), Hazard (:697-703: a write waits for every GPU
// use), WaitForGpu (:723-760: the faulting thread spins with nanosleep, sets g_submitRequested, until the completed submission
// passes the use), HandleFault (:1068-1150: re-checks the hazard after each wait), SetCompleted (:1211); the command processor
// honours g_submitRequested between batches, when idle and every 128 draws (runtime/gpu/command_processor.cpp:1121, :1146;
// runtime/gpu/renderer.cpp:9107). Changes here: the serial is the submission part (render_hooks.h currentPartSerial), completion is
// published by the command processor's confirmed retire (FM4 has no completion thread), a write waits for every guest page of the
// host pages it opens (FM4 opens a whole 16 KB host page per fault), the wait is bounded, and the request carries the serial so
// the command processor retires only up to it (PlanGuardFlush). Adapted design, new code.
//
// Rules (each tested):
//   G1 NoteRead(range, serial) before the range is protected and before the read is recorded; the per-page serial only rises.
//   G2 A write to [physical, physical + bytes) waits for WriterReader: the newest serial reading any guest page of the HOST pages the
//      range touches (a fault or a host write opens the whole host page, so a neighbour's store lets later stores through too).
//   G3 Plan: Proceed when that serial is completed; SubmitThenWait when it is not yet submitted (the open part reads it);
//      Wait otherwise.
//   G4 WaitForWrite reads Completed() first and the reader second, every iteration: NoteRead (raising a page to a later part, a
//      split) happens before the Completed() store that could release the waiter, on the same thread, so a waiter that sees the
//      completion also sees the later reader and keeps waiting. FM4 Recomp (FM4_ASYNC_RETIRE, async_retire.h): there the completion
//      worker publishes Completed(N) while the command processor may NoteRead for part N+1, so the same-thread order no longer
//      holds; nothing is lost because of G8: Completed(N) only says parts up to N finished reading, and a read noted for N+1 either
//      precedes the waiter's gate section (the waiter sees reader N+1 > N and keeps waiting) or follows it (NoteReadThen's check
//      finds the write recorded and the reader protects the range again). Tested in tests/runtime/test_async_retire.cpp.
//   G5 Every iteration posts the serial it needs (Request: an atomic maximum); the command processor takes it at each packet,
//      inside every WAIT_REG_MEM poll and when idle (TakeRequest), and plans the flush with PlanGuardFlush.
//   G6 The wait is bounded (TimedOut: the caller stops naming the range) and ends at once when the GPU is stranded (Strand: the
//      fence timeout wrote GPU_STOP; the caller stops).
//   G8 The gate (a spin flag; ReCarbon holds s_protectMutex for the same check, write_watch.cpp:1089-1091): EnterWrite returns
//      holding it once no unretired part reads the pages, and the caller makes the page writable and records the write
//      (write watch) before LeaveWrite; NoteReadThen raises the readers and runs the caller's "still protected" check under it.
//      So either the write is recorded before the read is noted (the check fails: the reader protects the range again) or the
//      read is noted before the write's check (the write waits for it). Holders do atomics and mprotect only, never wait.
//   G7 ChooseGuardPath: a read is direct only when the import exists, the range is inside physical memory, dword-aligned, guarded
//      by the write watch (A0000000/C0000000 views: the E0000000 view has its own memory, which the import does not see) and has
//      no pending GPU write-back; anything else takes a per-draw snapshot (today's copy into the ring or the vertex cache).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

namespace runtime::video {

enum class GuardPath : uint8_t { Direct, NoView, Outside, Unaligned, NotGuarded, PendingWrite };
struct GuardFacts
{
    bool view = false;          // GuestMemoryGpuView() != nullptr
    uint32_t physical = 0, bytes = 0;
    uint64_t physicalSize = 0;
    bool guarded = false;       // the write watch protects the range (WatchGuestRange, or an unwritten earlier watch)
    bool pendingWrite = false;  // a recorded resolve writes it back at a later retire (GuestWritesOverlap)
};
inline GuardPath ChooseGuardPath(const GuardFacts& f)
{
    if (!f.view)
        return GuardPath::NoView;
    if (f.bytes == 0 || uint64_t(f.physical) + f.bytes > f.physicalSize)
        return GuardPath::Outside;
    if (f.physical % 4 != 0)
        return GuardPath::Unaligned;
    if (!f.guarded)
        return GuardPath::NotGuarded;
    if (f.pendingWrite)
        return GuardPath::PendingWrite;
    return GuardPath::Direct;
}
// xe_vertex_base[slot] of a direct read: the binding is all of physical memory from offset 0 (guest_memory_vk.h).
inline uint32_t DirectBaseDword(uint32_t physical) { return physical / 4; }

// What the command processor does for a requested serial (G5). Nothing when it is completed; else submit the open part when it
// reads it (reader >= openSerial), then retire up to `reader` only when partial retires are on (FM4_PARTIAL_RETIRE), else the
// whole slot (wholeSlot: FlushSlot).
struct GuardFlushPlan
{
    bool flush = false, submitOpen = false, wholeSlot = false;
    uint64_t retireUpTo = 0;
};
inline GuardFlushPlan PlanGuardFlush(uint64_t reader, uint64_t completed, uint64_t openSerial, bool partialRetire)
{
    GuardFlushPlan p;
    if (reader <= completed)
        return p;
    p.flush = true;
    p.submitOpen = reader >= openSerial;
    p.wholeSlot = !partialRetire;
    p.retireUpTo = reader;
    return p;
}

class GpuReadGuard
{
public:
    enum class Action : uint8_t { Proceed, Wait, SubmitThenWait };
    enum class WaitResult : uint8_t { Done, TimedOut, Stranded };
    struct Stats
    {
        std::atomic<uint64_t> waits{0}, forcedSubmits{0}, waitNs{0}, timeouts{0};
    };

    // `guestPage` and `hostPage` powers of two, guestPage <= hostPage, both dividing `physicalSize`. Valid() says so.
    GpuReadGuard(uint64_t physicalSize, uint32_t guestPage, uint32_t hostPage)
        : physicalSize_(physicalSize), guestPage_(guestPage), hostPage_(hostPage),
          pages_(Shape(physicalSize, guestPage, hostPage) ? uint32_t(physicalSize / guestPage) : 0),
          reader_(pages_ != 0 ? new std::atomic<uint64_t>[pages_] : nullptr),
          hotWords_(pages_ != 0 ? uint32_t((physicalSize / hostPage + 63) / 64) : 0), hot_(hotWords_ != 0 ? new std::atomic<uint64_t>[hotWords_] : nullptr)
    {
        for (uint32_t i = 0; i < pages_; i++)
            reader_[i].store(0, std::memory_order_relaxed);
        for (uint32_t i = 0; i < hotWords_; i++)
            hot_[i].store(0, std::memory_order_relaxed);
    }
    bool Valid() const { return pages_ != 0; }

    // ---- The command processor's thread ------------------------------------------------------------------------------------
    // G1/G8: NoteRead under the gate, then `stillProtected()` (no write recorded since the range was last protected); its answer
    // is returned. False: the caller protects the range again (WatchGuestRange) before recording the read.
    template <class Check>
    bool NoteReadThen(uint32_t physical, uint32_t bytes, uint64_t serial, Check stillProtected)
    {
        LockGate();
        NoteRead(physical, bytes, serial);
        const bool ok = stillProtected();
        UnlockGate();
        return ok;
    }
    // G1: part `serial` reads [physical, physical + bytes). Call before protecting the range and before recording the read.
    void NoteRead(uint32_t physical, uint32_t bytes, uint64_t serial)
    {
        if (!Inside(physical, bytes))
            return;
        for (uint32_t p = physical / guestPage_; p <= (physical + bytes - 1) / guestPage_; p++)
            if (reader_[p].load(std::memory_order_relaxed) < serial)
                reader_[p].store(serial, std::memory_order_release);
    }
    // Parts up to `serial` were handed to the queue / confirmed complete (only rise; the completion after the fence wait and
    // before any write-back or deferred store of the retire). FM4_ASYNC_RETIRE: Completed is called by the completion worker.
    void Submitted(uint64_t serial) { Raise(submitted_, serial); }
    void Completed(uint64_t serial) { Raise(completed_, serial); }
    void Strand() { stranded_.store(true, std::memory_order_release); }
    // G5: the newest serial a waiter needs (0: none), cleared.
    uint64_t TakeRequest()  // a load first: it runs between every two packets
    {
        return request_.load(std::memory_order_acquire) == 0 ? 0 : request_.exchange(0, std::memory_order_acq_rel);
    }

    // ---- Any thread, lock-free -----------------------------------------------------------------------------------------------
    // G2: the newest part reading any guest page of the host pages [physical, physical + bytes) touches (0: none or outside).
    uint64_t WriteReader(uint32_t physical, uint32_t bytes) const
    {
        if (!Inside(physical, bytes))
            return 0;
        const uint64_t first = uint64_t(physical) / hostPage_ * hostPage_, end = (uint64_t(physical) + bytes + hostPage_ - 1) / hostPage_ * hostPage_;
        uint64_t newest = 0;
        for (uint64_t p = first / guestPage_; p < end / guestPage_; p++)
        {
            const uint64_t r = reader_[p].load(std::memory_order_acquire);
            newest = r > newest ? r : newest;
        }
        return newest;
    }
    uint64_t CompletedSerial() const { return completed_.load(std::memory_order_acquire); }
    uint64_t SubmittedSerial() const { return submitted_.load(std::memory_order_acquire); }
    bool Busy(uint32_t physical, uint32_t bytes) const
    {
        const uint64_t done = CompletedSerial();
        return WriteReader(physical, bytes) > done;
    }
    // G3.
    Action Plan(uint64_t reader) const
    {
        if (reader <= CompletedSerial())
            return Action::Proceed;
        return reader > SubmittedSerial() ? Action::SubmitThenWait : Action::Wait;
    }
    void Request(uint64_t serial) { Raise(request_, serial); }

    // G4-G6, G8: wait until no unretired part reads the host pages of the range; Done returns HOLDING the gate (LeaveWrite after
    // the page is writable and the write recorded); TimedOut and Stranded return without it. `now()` returns nanoseconds;
    // `sleep()` yields the thread (nanosleep in the fault handler; the tests advance a fake GPU and command processor in it).
    // Counted only when it had to wait.
    template <class Now, class Sleep>
    WaitResult EnterWrite(uint32_t physical, uint32_t bytes, uint64_t timeoutNs, Now now, Sleep sleep)
    {
        uint64_t start = 0;
        for (bool first = true;; first = false)
        {
            LockGate();
            const uint64_t done = CompletedSerial();  // G4: completion first, then the reader
            const uint64_t reader = WriteReader(physical, bytes);
            if (reader <= done)
            {
                if (!first)
                    stats_.waitNs.fetch_add(now() - start, std::memory_order_relaxed);
                return WaitResult::Done;
            }
            UnlockGate();
            if (first)
            {
                start = now();
                stats_.waits.fetch_add(1, std::memory_order_relaxed);
                MarkHot(physical, bytes, reader);
                if (Plan(reader) == Action::SubmitThenWait)
                    stats_.forcedSubmits.fetch_add(1, std::memory_order_relaxed);
            }
            if (stranded_.load(std::memory_order_acquire) || now() - start > timeoutNs)
            {
                const bool stranded = stranded_.load(std::memory_order_acquire);
                if (!stranded)
                    stats_.timeouts.fetch_add(1, std::memory_order_relaxed);
                stats_.waitNs.fetch_add(now() - start, std::memory_order_relaxed);
                return stranded ? WaitResult::Stranded : WaitResult::TimedOut;
            }
            Request(reader);
            sleep();
        }
    }
    void LeaveWrite() { UnlockGate(); }

    // FM4 Recomp (M7, opt-in FM4_GUARD_HOT=1; one write-watch wait was measured in 97 % of race swaps): a host page a
    // store had to wait for is "hot" from then on, set by EnterWrite (any thread, the fault handler included: one atomic or). A
    // reader that checks Hot before NoteRead takes its copy path for a hot page, so the guest's next store to it finds no
    // unretired reader and does not wait. Pages only become hot (no decay): FM4 rewrites such pages every frame.
    // Mode 2 (FM4_GUARD_HOT=2; observed: the hot pages advance by 8 host pages a frame, a ring the game writes one step
    // ahead of the GPU): a page made hot within kRingBack pages after a hot page also makes the next kRingAhead pages hot, so a
    // streaming ring costs one wait per kRingAhead pages instead of one per step.
    static constexpr uint32_t kRingBack = 16, kRingAhead = 256;
    void SetHotTracking(bool on, bool ringAhead = false)
    {
        ringAhead_.store(ringAhead, std::memory_order_release);
        hotOn_.store(on, std::memory_order_release);
    }
    uint64_t RingAheadPages() const { return ringAheadCount_.load(std::memory_order_acquire); }
    bool Hot(uint32_t physical, uint32_t bytes) const
    {
        if (!hotOn_.load(std::memory_order_acquire) || !Inside(physical, bytes))
            return false;
        for (uint64_t h = physical / hostPage_; h <= (uint64_t(physical) + bytes - 1) / hostPage_; h++)
            if (hot_[h / 64].load(std::memory_order_acquire) & (1ull << (h % 64)))
                return true;
        return false;
    }
    uint64_t HotPages() const { return hotCount_.load(std::memory_order_acquire); }
    // A store that waited outside EnterWrite (the command processor's own stores: renderer.cpp BeforeCpWrite) makes its pages hot too.
    void NoteWaitedStore(uint32_t physical, uint32_t bytes, uint64_t reader) { MarkHot(physical, bytes, reader); }
    // The first kHotLog pages made hot, in order (page physical address, the reader part that held the store): for a log line each.
    static constexpr uint32_t kHotLog = 32;
    bool HotLogEntry(uint64_t i, uint32_t& physical, uint64_t& reader) const
    {
        if (i >= kHotLog || i >= hotCount_.load(std::memory_order_acquire))
            return false;
        physical = hotLogPage_[i].load(std::memory_order_acquire), reader = hotLogReader_[i].load(std::memory_order_acquire);
        return true;
    }
    // EnterWrite then LeaveWrite: for a write whose page is opened elsewhere (a heap release, a system call's write).
    template <class Now, class Sleep>
    WaitResult WaitForWrite(uint32_t physical, uint32_t bytes, uint64_t timeoutNs, Now now, Sleep sleep)
    {
        const WaitResult r = EnterWrite(physical, bytes, timeoutNs, now, sleep);
        if (r == WaitResult::Done)
            LeaveWrite();
        return r;
    }
    bool GateHeld() const { return gate_.test(std::memory_order_acquire); }
    const Stats& GetStats() const { return stats_; }
    uint32_t HostPageBytes() const { return hostPage_; }

private:
    static bool Shape(uint64_t size, uint32_t guest, uint32_t host)
    {
        const auto pow2 = [](uint64_t v) { return v != 0 && (v & (v - 1)) == 0; };
        return pow2(guest) && pow2(host) && guest <= host && size != 0 && size % host == 0 && size / guest <= UINT32_MAX;
    }
    void LockGate()
    {
        while (gate_.test_and_set(std::memory_order_acquire))
        {
        }
    }
    void UnlockGate() { gate_.clear(std::memory_order_release); }
    void MarkHot(uint32_t physical, uint32_t bytes, uint64_t reader)  // async-signal-safe: atomics only
    {
        if (!hotOn_.load(std::memory_order_acquire) || !Inside(physical, bytes))
            return;
        for (uint64_t h = physical / hostPage_; h <= (uint64_t(physical) + bytes - 1) / hostPage_; h++)
        {
            const uint64_t bit = 1ull << (h % 64);
            if (hot_[h / 64].fetch_or(bit, std::memory_order_acq_rel) & bit)
                continue;
            const uint64_t i = hotCount_.fetch_add(1, std::memory_order_acq_rel);
            if (i < kHotLog)
                hotLogPage_[i].store(uint32_t(h * hostPage_), std::memory_order_release), hotLogReader_[i].store(reader, std::memory_order_release);
            if (ringAhead_.load(std::memory_order_acquire) && HotBehind(h))
                MarkAhead(h);
        }
    }
    bool HotBit(uint64_t h) const { return (hot_[h / 64].load(std::memory_order_acquire) >> (h % 64)) & 1; }
    bool HotBehind(uint64_t h) const
    {
        for (uint64_t d = 1; d <= kRingBack && d <= h; d++)
            if (HotBit(h - d))
                return true;
        return false;
    }
    void MarkAhead(uint64_t h)
    {
        const uint64_t hostPages = physicalSize_ / hostPage_;
        for (uint64_t a = h + 1; a <= h + kRingAhead && a < hostPages; a++)
            if (!(hot_[a / 64].fetch_or(1ull << (a % 64), std::memory_order_acq_rel) & (1ull << (a % 64))))
                ringAheadCount_.fetch_add(1, std::memory_order_relaxed);
    }
    bool Inside(uint32_t physical, uint32_t bytes) const { return pages_ != 0 && bytes != 0 && uint64_t(physical) + bytes <= physicalSize_; }
    static void Raise(std::atomic<uint64_t>& a, uint64_t v)
    {
        uint64_t old = a.load(std::memory_order_relaxed);
        while (old < v && !a.compare_exchange_weak(old, v, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
        }
    }

    const uint64_t physicalSize_;
    const uint32_t guestPage_, hostPage_, pages_;
    std::unique_ptr<std::atomic<uint64_t>[]> reader_;  // per guest page: the newest part that reads it
    const uint32_t hotWords_;
    std::unique_ptr<std::atomic<uint64_t>[]> hot_;  // FM4_GUARD_HOT: one bit per host page
    std::atomic<bool> hotOn_{false}, ringAhead_{false};
    std::atomic<uint64_t> ringAheadCount_{0};
    std::atomic<uint64_t> hotCount_{0};
    std::atomic<uint32_t> hotLogPage_[kHotLog] = {};
    std::atomic<uint64_t> hotLogReader_[kHotLog] = {};
    std::atomic<uint64_t> submitted_{0}, completed_{0}, request_{0};
    std::atomic<bool> stranded_{false};
    std::atomic_flag gate_;  // clear (C++20)
    Stats stats_;
};

}  // namespace runtime::video
