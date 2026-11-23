// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Controller rumble: what the window loop sends to the SDL gamepad for the game's XamInputSetState requests. The approach is
// ReCarbon's (runtime/hid/input.cpp at 66401ea, GPL-3.0-or-later: XamInputSetState stores the two motor speeds, the main loop sends
// them with SDL_RumbleGamepad, Suspend sends 0 and Resume sends the last request again), re-written as a pure planner so it is
// tested without SDL. Our changes: the planner; a short hold (1 s, sent again every 0.5 s while the motors run) instead of 5 s
// sent once, so a process that ends or hangs without a last word leaves the motors on for at most a second; a pad that
// refuses a command is tried again after a pause (what a pad reports about its motors is not trusted: it can be wrong after a reconnect).
// The game's own vibration setting is respected by forwarding only what the game asks for (HYPOTHESIS, not yet observed: with it
// off the game asks for nothing).
// X_INPUT_VIBRATION (Xenia hid/input.h:74-78 at 67f16c4): left motor speed u16, right motor speed u16, big-endian in the guest. The
// left motor is the low-frequency one and the right the high-frequency one, SDL_RumbleGamepad's two arguments in that order
// (Xenia sdl_input_driver.cc:208-234).
#pragma once
#include <cstdint>

namespace runtime::input {

struct RumbleRequest
{
    uint16_t left = 0, right = 0;
    bool operator==(const RumbleRequest&) const = default;
};

// One SDL_RumbleGamepad call: (low, high, duration); duration 0 with 0 speeds stops the motors.
struct RumbleCommand
{
    bool send = false;
    uint16_t low = 0, high = 0;
    uint32_t durationMs = 0;
};

class RumblePlanner
{
public:
    static constexpr uint32_t kHoldMs = 1000, kRefreshMs = 500;

    // Once per window-loop iteration. `request`: the game's last XamInputSetState. `pad`: a gamepad is open and can rumble.
    // `active`: the motors may run now (the window has the focus and the game is not paused behind a host panel).
    RumbleCommand Step(RumbleRequest request, bool pad, bool active, uint64_t nowMs)
    {
        if (!pad)
        {
            sent_ = {};  // a pad that left takes its motors with it; the next one starts still
            return {};
        }
        const RumbleRequest target = active ? request : RumbleRequest{};
        const bool running = target.left != 0 || target.right != 0;
        if (target == sent_ && !(running && nowMs - sentAtMs_ >= kRefreshMs))
            return {};
        sent_ = target, sentAtMs_ = nowMs;
        return {true, target.left, target.right, running ? kHoldMs : 0};
    }

    // The pad refused the command (SDL_RumbleGamepad false): nothing is running as far as we know.
    void Refused() { sent_ = {}; }
    // Before the pad is closed or the process ends: whether a stop is worth sending (the motors may be running).
    bool Running() const { return sent_.left != 0 || sent_.right != 0; }

private:
    RumbleRequest sent_;
    uint64_t sentAtMs_ = 0;
};

// The guest's X_INPUT_VIBRATION as one big-endian word (left in the high half).
inline RumbleRequest RumbleFromGuestWord(uint32_t word) { return {uint16_t(word >> 16), uint16_t(word)}; }

// imports_input.cpp: the game's last request for user 0 (0, 0 until it asks).
RumbleRequest GameRumble();

}  // namespace runtime::input
