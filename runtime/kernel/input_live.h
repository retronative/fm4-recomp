// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
// InputGate: adapted from ReCarbon (SpeedBreaker runtime), GPL-3.0-or-later; see the class.
//
// FM4 Recomp: a live gamepad on user 0, opt-in FM4_INPUT_LIVE=1 with
// --renderer window. The window's main loop (window_sdl.cpp) reads an SDL gamepad and the keyboard every iteration and hands
// the merged state to XamInputGetState (imports_input.cpp). Layout and button bits: X_INPUT_GAMEPAD, Xenia hid/input.h:57-72
// (buttons u16, triggers u8 u8, thumbs s16 LX LY RX RY with +Y up). The pure mapping is here so it is tested without SDL.
#pragma once
#include <algorithm>
#include <cstdint>

namespace runtime::input {

namespace button {  // X_INPUT_GAMEPAD_* (Xenia hid/input.h:30-45)
constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
                   kLeftThumb = 0x0040, kRightThumb = 0x0080, kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kA = 0x1000, kB = 0x2000,
                   kX = 0x4000, kY = 0x8000;
}

struct LiveState
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    bool operator==(const LiveState&) const = default;
};

// The keyboard's part. Driving: A/D steer (left stick X), W throttle (RT), S brake (LT), arrow keys the d-pad (menus),
// Enter A, Backspace/Escape B, X X, Y Y, Space START, Tab BACK, Q/E the shoulders, C the right thumb (camera): the Keys fields keep
// those names; which keys press them is key_bindings.h (defaults: Space A, Backspace B, Left Shift X, Left Ctrl Y, Return START).
struct Keys
{
    bool up = false, down = false, left = false, right = false, w = false, a = false, s = false, d = false, enter = false, backspace = false,
         space = false, tab = false, x = false, y = false, q = false, e = false, c = false;
};

inline LiveState FromKeys(const Keys& k)
{
    LiveState s;
    s.buttons = uint16_t((k.up ? button::kDpadUp : 0) | (k.down ? button::kDpadDown : 0) | (k.left ? button::kDpadLeft : 0) |
                         (k.right ? button::kDpadRight : 0) | (k.enter ? button::kA : 0) | (k.backspace ? button::kB : 0) | (k.x ? button::kX : 0) |
                         (k.y ? button::kY : 0) | (k.space ? button::kStart : 0) | (k.tab ? button::kBack : 0) | (k.q ? button::kLeftShoulder : 0) |
                         (k.e ? button::kRightShoulder : 0) | (k.c ? button::kRightThumb : 0));
    s.rightTrigger = k.w ? 255 : 0;
    s.leftTrigger = k.s ? 255 : 0;
    s.leftX = int16_t(k.a == k.d ? 0 : k.a ? -32768 : 32767);
    return s;
}

// A gamepad's buttons by position (SDL_GamepadButton: south, east, west, north are the face buttons, an Xbox pad's A, B, X, Y), as
// X_INPUT_GAMEPAD button bits.
struct PadButtons
{
    bool south = false, east = false, west = false, north = false, back = false, start = false, leftStick = false, rightStick = false,
         leftShoulder = false, rightShoulder = false, dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
};

inline uint16_t ButtonsFromPad(const PadButtons& p)
{
    return uint16_t((p.dpadUp ? button::kDpadUp : 0) | (p.dpadDown ? button::kDpadDown : 0) | (p.dpadLeft ? button::kDpadLeft : 0) |
                    (p.dpadRight ? button::kDpadRight : 0) | (p.start ? button::kStart : 0) | (p.back ? button::kBack : 0) |
                    (p.leftStick ? button::kLeftThumb : 0) | (p.rightStick ? button::kRightThumb : 0) |
                    (p.leftShoulder ? button::kLeftShoulder : 0) | (p.rightShoulder ? button::kRightShoulder : 0) | (p.south ? button::kA : 0) |
                    (p.east ? button::kB : 0) | (p.west ? button::kX : 0) | (p.north ? button::kY : 0));
}

// An SDL gamepad axis (-32768..32767, +Y down) as an XInput thumb (+Y up); a trigger axis (0..32767) as 0..255.
inline int16_t ThumbFromSdl(int16_t axis, bool vertical) { return vertical ? int16_t(axis == -32768 ? 32767 : -axis) : axis; }
inline uint8_t TriggerFromSdl(int16_t axis) { return uint8_t(std::clamp<int>(axis, 0, 32767) * 255 / 32767); }

// The pad and the keyboard together: buttons ORed, the larger trigger, the keyboard's stick when it steers, else the pad's.
inline LiveState Merge(const LiveState& pad, const LiveState& keys)
{
    LiveState s = pad;
    s.buttons = uint16_t(pad.buttons | keys.buttons);
    s.leftTrigger = std::max(pad.leftTrigger, keys.leftTrigger);
    s.rightTrigger = std::max(pad.rightTrigger, keys.rightTrigger);
    if (keys.leftX != 0)
        s.leftX = keys.leftX;
    return s;
}

// While a host screen has the input (the Keyboard Controls screen, controls_screen.h) the game sees an idle pad; after it closes,
// each button still held stays hidden until released, so the press that closed the screen does not reach the game. Adapted from
// ReCarbon runtime/hid/input.cpp:731-745 at 66401ea (SpeedBreaker runtime, GPL-3.0-or-later). FM4 Recomp changes: a pure class
// over LiveState (tested without SDL); no Back-button replay state (FM4 has no scripted Back handling).
class InputGate
{
public:
    LiveState Filter(const LiveState& in, bool blocked)
    {
        if (wasBlocked_ && !blocked)
            latched_ = in.buttons;
        wasBlocked_ = blocked;
        latched_ = uint16_t(latched_ & in.buttons);
        if (blocked)
            return LiveState{};
        LiveState out = in;
        out.buttons = uint16_t(in.buttons & ~latched_);
        return out;
    }

private:
    bool wasBlocked_ = false;
    uint16_t latched_ = 0;
};

// imports_input.cpp: FM4_INPUT_LIVE connects the live pad (main.cpp, window mode only); the window loop updates it.
void EnableLive();
bool LiveEnabled();
void SetLiveState(const LiveState& state);
// FM4 Recomp (title hooks, e.g. skipping the pre-race video with A): how many times the state
// returned to user 0 (script or live pad) went from A up to A down, so a hook can react to a new press, not a held button.
uint64_t APresses();

}  // namespace runtime::input
