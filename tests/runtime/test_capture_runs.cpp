// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (FM4_NO_DRAIN): T0 tests of runtime/video/capture_runs.h, CPU only. The rule (written down before the code, from the
// ShiftGlue SDK's src/graphics/fm4_resolve_readbacks.cpp runs and "extra" rule, and our bound): a run is the resolves to one range
// with gaps of at most 4 frames; a resolve does NOT wait only when it is at least the run's 3rd and this frame's count of resolves to
// the range is not above the count in the run's previous frame. Each expectation is counted by hand in its name. "CONTROL" names a wrong behaviour that must not happen.
//
//   tests/run_capture_runs_tests.sh
#include "capture_runs.h"
#include <cstdio>

using runtime::video::CaptureRuns;

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
}  // namespace

int main()
{
    {  // one resolve a frame
        CaptureRuns r;
        Expect(r.Resolve(0x1000, 0x400, 10), "frame 10: the run's 1st resolve waits");
        Expect(r.Resolve(0x1000, 0x400, 11), "frame 11: the 2nd waits");
        Expect(!r.Resolve(0x1000, 0x400, 12), "frame 12: the 3rd, count 1 = previous 1: no wait");
        Expect(!r.Resolve(0x1000, 0x400, 16), "frame 16: gap 4 (16 = 12 + 4) continues the run: no wait");
        Expect(r.Resolve(0x1000, 0x400, 21), "frame 21: gap 5 (> 16 + 4) starts a new run: waits");
        Expect(r.Resolve(0x1000, 0x400, 22), "frame 22: the new run's 2nd waits");
        CONTROL(r.Resolve(0x1000, 0x400, 23), "frame 23: the new run's 3rd does not wait");
    }
    {  // double-buffered: every other frame
        CaptureRuns r;
        Expect(r.Resolve(0x6000, 0x400, 1) && r.Resolve(0x6000, 0x400, 3), "frames 1 and 3: the 1st and 2nd wait");
        Expect(!r.Resolve(0x6000, 0x400, 5), "frame 5: the 3rd (gap 2 continues the run) does not wait");
        CONTROL(r.Resolve(0x6000, 0x400, 7), "frame 7: still steady");
    }
    {  // two resolves a frame
        CaptureRuns r;
        Expect(r.Resolve(0x2000, 0x100, 1) && r.Resolve(0x2000, 0x100, 1), "frame 1: the 1st and 2nd wait");
        Expect(!r.Resolve(0x2000, 0x100, 2), "frame 2, first: the 3rd, count 1 <= previous 2: no wait");
        Expect(!r.Resolve(0x2000, 0x100, 2), "frame 2, second: count 2 <= previous 2: no wait");
        Expect(r.Resolve(0x2000, 0x100, 2), "frame 2, third: count 3 > previous 2 (an extra capture): waits");
        CONTROL(r.Resolve(0x2000, 0x100, 3), "frame 3, first: count 1 <= previous 3: no wait");
    }
    {  // a capture re-run a few frames after its first (the home-space environment map)
        CaptureRuns r;
        Expect(r.Resolve(0x3000, 0x800, 100), "frame 100: the first map (its clear) waits");
        Expect(r.Resolve(0x3000, 0x800, 103), "frame 103: the loaded map, the run's 2nd, waits");
    }
    {  // ranges are keyed by address and length
        CaptureRuns r;
        for (uint64_t f = 1; f <= 3; f++)
            r.Resolve(0x4000, 0x400, f);
        Expect(r.Resolve(0x4000, 0x800, 3), "the same address with another length is another range: waits");
        Expect(r.Resolve(0x5000, 0x400, 3), "another address with the same length: waits");
        CONTROL(r.Resolve(0x4000, 0x400, 4), "the steady range is not disturbed by the others");
    }
    std::printf("%s: %d failure(s), %d control(s)\n", failures ? "FAILED" : "passed", failures, controls);
    return failures ? 1 : 0;
}
