// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7): runtime/video/submit_policy.h DecideSplit, CPU only. Expected values by hand from the header's rules.
//   tests/run_submit_policy_tests.sh
#include "submit_policy.h"
#include <cstdio>
using runtime::video::DecideSplit;
using runtime::video::SplitDecision;
namespace {
int failures = 0, controls = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}
void Control(bool wrong, const char* name)
{
    controls++;
    Expect(!wrong, name);
}
}  // namespace

int main()
{
    int asked = 0;
    const auto empty = [&] { asked++; return true; };
    const auto busy = [&] { asked++; return false; };
    // Split 128 at pass end, idle 16 (the race stack + FM4_SUBMIT_IF_IDLE=16).
    SplitDecision d = DecideSplit(128, 128, true, false, 16, busy);
    Expect(d.bySize && !d.byIdle && asked == 0, "128 draws outside a pass: by size, the queue not asked");
    d = DecideSplit(300, 128, true, true, 16, busy);
    Expect(!d.Split(), "300 draws inside a pass, GPU busy: no split (pass end needs 512, idle needs an empty queue)");
    d = DecideSplit(512, 128, true, true, 16, busy);
    Expect(d.bySize, "512 draws inside a pass: by size (4 x 128)");
    asked = 0;
    d = DecideSplit(16, 128, true, false, 16, empty);
    Expect(!d.bySize && d.byIdle && asked == 1, "16 draws outside a pass, GPU idle: by idle");
    d = DecideSplit(16, 128, true, false, 16, busy);
    Control(d.Split(), "control: 16 draws, GPU busy: no split");
    asked = 0;
    d = DecideSplit(15, 128, true, false, 16, empty);
    Expect(!d.Split() && asked == 0, "15 draws: below idle's 16, the queue not asked");
    d = DecideSplit(63, 128, true, true, 16, empty);
    Control(d.Split(), "control: 63 draws inside a pass, GPU idle: no split (4 x 16 = 64)");
    d = DecideSplit(64, 128, true, true, 16, empty);
    Expect(d.byIdle, "64 draws inside a pass, GPU idle: by idle");
    // FM4_SUBMIT_IF_IDLE_IN_PASS: factor 1: 16 draws inside a pass, GPU idle: by idle; 15 not; factor 2: 31 not, 32 yes.
    d = DecideSplit(16, 128, true, true, 16, empty, 1);
    Expect(d.byIdle, "factor 1: 16 draws inside a pass, GPU idle: by idle");
    d = DecideSplit(31, 128, true, true, 16, empty, 2);
    Control(d.Split(), "control: factor 2: 31 draws inside a pass: no split (2 x 16 = 32)");
    d = DecideSplit(32, 128, true, true, 16, empty, 2);
    Expect(d.byIdle, "factor 2: 32 draws inside a pass, GPU idle: by idle");
    d = DecideSplit(100, 128, true, false, 0, empty);
    Control(d.Split(), "control: idle off (0): only by size");
    d = DecideSplit(1000, 0, true, false, 16, empty);
    Control(d.Split(), "control: FM4_SUBMIT_SPLIT off: nothing splits");
    d = DecideSplit(200, 128, false, true, 16, busy);
    Expect(d.bySize, "200 draws inside a pass without FM4_SPLIT_AT_PASS_END: by size");
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures ? 1 : 0;
}
