// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (FM4_LOAD_WAITS_RELEASES): T0 tests of runtime/game/release_wait.h, CPU only, on a synthetic manager (no title
// address; the layout numbers here are the test's own). Expected values are counted by hand in the comments. The scenario is the
// race-start failure: a loader's allocation fails while the main thread still has releases queued; it must be retried until
// they have run, and never wait on its own queue or wait twice for a thread that is not served. "CONTROL" names a wrong
// behaviour that must not happen.
//
//   tests/run_release_wait_tests.sh
#include "release_wait.h"
#include <cstdio>
#include <map>

using namespace fm4::title;

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

// A manager at 0x1000 with 3 thread entries at +0x100 and queues from entry index + 5, 16 bytes each (0x1050, 0x1060, 0x1070).
const RequestQueues kLayout{3, 0x100, 5, 16};
constexpr uint32_t kManager = 0x1000;
struct Memory
{
    std::map<uint32_t, uint32_t> words;
    uint32_t operator()(uint32_t address) const
    {
        const auto it = words.find(address);
        return it == words.end() ? 0 : it->second;
    }
    void Thread(uint32_t index, uint32_t id, uint32_t begin, uint32_t end)
    {
        words[kManager + 0x100 + 4 * index] = id;
        words[kManager + (index + 5) * 16] = begin, words[kManager + (index + 5) * 16 + 4] = end;
    }
};

void Pending()
{
    Memory m;
    m.Thread(0, 0xA1, 0x8000, 0x8000 + 72);  // the main thread: three 24-byte requests
    m.Thread(1, 0xB2, 0x9000, 0x9000 + 24);  // a loader: one
    m.Thread(2, 0, 0x7000, 0x7000 + 240);    // an unused entry (thread id 0): its words do not count
    Expect(PendingForOthers(kLayout, kManager, 0xB2, m) == 72, "the loader sees the main thread's 72 bytes");
    Expect(PendingForOthers(kLayout, kManager, 0xA1, m) == 24, "the main thread sees the loader's 24 bytes");
    Expect(PendingForOthers(kLayout, kManager, 0xC3, m) == 96, "an unregistered thread sees both (72 + 24)");
    CONTROL(PendingForOthers(kLayout, kManager, 0xB2, m) == 96, "a thread's own queue is not counted");
    CONTROL(PendingForOthers(kLayout, kManager, 0xC3, m) == 336, "an unused entry is not counted");
    m.Thread(0, 0xA1, 0x8048, 0x8048);       // drained: begin == end
    Expect(PendingForOthers(kLayout, kManager, 0xB2, m) == 0, "a drained queue counts nothing");
    m.Thread(0, 0xA1, 0x8048, 0x8000);       // end below begin (read between two updates): nothing, not a huge number
    Expect(PendingForOthers(kLayout, kManager, 0xB2, m) == 0, "an inconsistent pair counts nothing");
}

struct Run
{
    uint32_t pending, releasePerSleep, succeedBelow;  // the allocation succeeds once pending < succeedBelow
    int retries = 0, sleeps = 0;
    bool gaveUp = false;
    uint32_t waited = 0;
    bool Go(uint32_t limit)
    {
        return WaitForReleases([&] { return pending; }, [&] { retries++; return pending < succeedBelow; },
                               [&] { sleeps++; pending -= pending < releasePerSleep ? pending : releasePerSleep; }, gaveUp, limit, waited);
    }
};

void Waits()
{
    {
        // 72 bytes queued, 24 released per millisecond, room once 24 or fewer remain: sleeps at 72 -> 48 (retry fails), 48 -> 24
        // (retry succeeds). 2 sleeps, 2 retries.
        Run r{72, 24, 25};
        Expect(r.Go(100) && r.sleeps == 2 && r.retries == 2 && r.waited == 2 && !r.gaveUp, "waits until the releases make room");
        CONTROL(r.sleeps == 0, "a failure with releases queued does not return at once");
    }
    {
        // Nothing queued: one retry (the last release may have landed after the failure), no sleep, the failure stands.
        Run r{0, 0, 0};
        Expect(!r.Go(100) && r.sleeps == 0 && r.retries == 1 && r.waited == 0 && !r.gaveUp, "with nothing queued the failure stands after one retry");
    }
    {
        // Room appears only when everything is released: 48 -> 24 (fail), 24 -> 0 (fail), then the retry with nothing queued succeeds.
        Run r{48, 24, 0};
        r.succeedBelow = 1;
        Expect(r.Go(100) && r.sleeps == 2 && r.retries == 2, "the retry after the last release is made");
    }
    {
        // The other thread never runs (it waits for this one): after the limit of 5 sleeps the failure stands and is remembered;
        // the next failure returns without a sleep; once the queues are seen empty a later failure waits again.
        Run r{72, 0, 0};
        Expect(!r.Go(5) && r.sleeps == 5 && r.retries == 5 && r.gaveUp, "a wait that is never served stops at the limit");
        r.sleeps = r.retries = 0;
        Expect(!r.Go(5) && r.sleeps == 0 && r.retries == 0 && r.gaveUp, "the next failure does not wait again");
        CONTROL(r.sleeps != 0, "an unserved thread does not pay the limit per allocation");
        r.pending = 0;
        Expect(!r.Go(5) && !r.gaveUp, "empty queues clear the memory of the unserved wait");
        r.pending = 24, r.releasePerSleep = 24, r.succeedBelow = 1, r.sleeps = 0;
        Expect(r.Go(5) && r.sleeps == 1, "and the next failure waits again");
    }
}
}  // namespace

int main()
{
    Pending();
    Waits();
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures != 0;
}
