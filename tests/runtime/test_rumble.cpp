// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// runtime/kernel/rumble.h (controller rumble; ReCarbon's SDL approach as a pure planner): which SDL_RumbleGamepad calls the window
// loop makes for the game's requests. Expected commands are written out from the rules (send on change; 1000 ms hold, sent again
// every 500 ms while a motor runs; 0, 0 with duration 0 stops; nothing to a pad that is absent or cannot rumble; stopped while the
// window is in the background), not taken from the code's output. X_INPUT_VIBRATION: left u16 then right u16, big-endian
// (Xenia hid/input.h:74-78), left = low frequency, right = high frequency.
#include "rumble.h"
#include <cstdio>

using namespace runtime::input;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", what);
    failures += ok ? 0 : 1;
}
static bool Is(const RumbleCommand& c, bool send, uint16_t low, uint16_t high, uint32_t ms)
{
    return c.send == send && (!send || (c.low == low && c.high == high && c.durationMs == ms));
}

int main()
{
    // The guest bytes FF FF 80 00 are left FFFF, right 8000.
    const RumbleRequest word = RumbleFromGuestWord(0xFFFF8000u), swapped = RumbleFromGuestWord(0x4000FFFFu);
    Expect(word.left == 0xFFFF && word.right == 0x8000 && swapped.left == 0x4000 && swapped.right == 0xFFFF && RumbleFromGuestWord(0) == RumbleRequest{},
           "guest word: left motor in the high half, right in the low half (CONTROL: the halves exchanged)");

    {
        RumblePlanner p;
        const bool idle = Is(p.Step({}, true, true, 0), false, 0, 0, 0) && !p.Running();
        const bool start = Is(p.Step({0x4000, 0x1000}, true, true, 10), true, 0x4000, 0x1000, 1000) && p.Running();
        const bool same = Is(p.Step({0x4000, 0x1000}, true, true, 509), false, 0, 0, 0);
        const bool again = Is(p.Step({0x4000, 0x1000}, true, true, 510), true, 0x4000, 0x1000, 1000);
        const bool change = Is(p.Step({0x4000, 0x2000}, true, true, 520), true, 0x4000, 0x2000, 1000);
        const bool stop = Is(p.Step({}, true, true, 530), true, 0, 0, 0) && !p.Running();
        const bool quiet = Is(p.Step({}, true, true, 5000), false, 0, 0, 0);
        Expect(idle && start && same, "no request sends nothing; a request is sent once with a 1000 ms hold; the same request 499 ms later is not sent again");
        Expect(again && change, "a running request is sent again after 500 ms; a changed speed at once (left -> low, right -> high)");
        Expect(stop && quiet, "the game's 0, 0 stops the motors (duration 0) once; still motors are never refreshed (CONTROL)");
    }
    {
        // Focus loss (or the Settings panel over the paused game): stop, nothing while away, the game's request again on return.
        RumblePlanner p;
        (void)p.Step({0x8000, 0}, true, true, 0);
        const bool lost = Is(p.Step({0x8000, 0}, true, false, 100), true, 0, 0, 0) && !p.Running();
        const bool away = Is(p.Step({0x8000, 0}, true, false, 2000), false, 0, 0, 0) && Is(p.Step({0xFFFF, 0xFFFF}, true, false, 2100), false, 0, 0, 0);
        const bool back = Is(p.Step({0xFFFF, 0xFFFF}, true, true, 2200), true, 0xFFFF, 0xFFFF, 1000);
        Expect(lost && away && back, "background: the motors stop at once, a request made meanwhile is held back, and the last request is sent on return");
    }
    {
        // No pad, or one that cannot rumble: nothing is ever sent; a pad that arrives while the game asks gets the request.
        RumblePlanner p;
        const bool none = Is(p.Step({0x8000, 0x8000}, false, true, 0), false, 0, 0, 0) && Is(p.Step({0x8000, 0x8000}, false, true, 900), false, 0, 0, 0) && !p.Running();
        const bool arrives = Is(p.Step({0x8000, 0x8000}, true, true, 1000), true, 0x8000, 0x8000, 1000);
        // Disconnect while running: nothing to send to it; the next pad is sent the request afresh (not skipped as "already sent").
        const bool gone = Is(p.Step({0x8000, 0x8000}, false, true, 1100), false, 0, 0, 0) && !p.Running();
        const bool next = Is(p.Step({0x8000, 0x8000}, true, true, 1150), true, 0x8000, 0x8000, 1000);
        // ... and a pad that comes back after the game stopped asking is sent nothing.
        (void)p.Step({}, false, true, 1200);
        const bool still = Is(p.Step({}, true, true, 1300), false, 0, 0, 0);
        Expect(none && arrives, "no pad, or a pad without motors: nothing sent; a pad that connects during a request gets it");
        Expect(gone && next && still, "disconnect: the planner forgets what it sent, so a reconnected pad is sent the running request, or nothing if the game asks for none");
    }
    {
        // A refusal (SDL_RumbleGamepad false): nothing counts as running, so no stop is owed at quit.
        RumblePlanner p;
        (void)p.Step({0x1000, 0x1000}, true, true, 0);
        const bool before = p.Running();
        p.Refused();
        Expect(before && !p.Running(), "a refused command leaves nothing running (no stop owed when the pad closes or the game quits)");
    }
    std::printf("rumble tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
