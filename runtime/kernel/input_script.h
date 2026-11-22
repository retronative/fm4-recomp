// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// A scripted gamepad for bounded headless runs (test only, opt-in: FM4_INPUT_SCRIPT=<file>, loaded by main.cpp).
// The script is a text file, one event per line, `<vblank> <button>...`: from that vblank on, user 0's pad reports
// exactly those buttons held (an empty list releases everything) until the next line; `#` starts a comment. Names:
// A B X Y START BACK LB RB DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT LS RS (X_INPUT_GAMEPAD button bits, Xenia
// xenia-canary 67f16c4e315f src/xenia/hid/input.h:31-47) and LT RT (the analogue triggers, pulled fully: 0xFF;
// input.h:59-60). Thumbsticks stay centred (0) unless a line names a direction: LS_LEFT LS_RIGHT LS_UP LS_DOWN RS_LEFT RS_RIGHT
// RS_UP RS_DOWN push that stick fully along the axis (-32768 / 32767, +Y up; input.h:57-72). Vblanks must increase strictly from
// line to line.
//
// Without a script nothing here is reached and XamInputGetState keeps reporting no controller (imports_xam.cpp).
// This layer is pure (no guest memory, no stop): parsing reports an error message, the import file turns it into
// a Setup stop at start-up and polls the active event with the vblank counter (kernel/graphics.h VblankCount).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace runtime::input {

// X_INPUT_GAMEPAD button bits (Xenia hid/input.h:31-47).
enum Button : uint16_t
{
    kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
    kLeftThumb = 0x0040, kRightThumb = 0x0080, kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kA = 0x1000, kB = 0x2000,
    kX = 0x4000, kY = 0x8000,
};

struct Event
{
    uint64_t vblank = 0;
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;  // 0 or 0xFF (LT / RT named)
    int16_t thumbs[4] = {};                     // LX, LY, RX, RY: 0, -32768 or 32767 (LS_* / RS_* named)
};

// FM4 Recomp (M7 A/B harness): a line `@race` starts a second list whose vblanks count from the race start (Poller::RaceStarted;
// renderer.cpp reports it), so the race inputs are the same whatever the load time. Before it the first list applies; from the
// race start only the race list does (nothing held before its first event). Without `@race` RaceStarted changes nothing.
// FM4 Recomp (M7 stability harness): `@title` instead anchors the second list on the title screen's acceptance (the game's storage
// device prompt, XamShowDeviceSelectorUI: Poller::TitleAccepted), so the menu route keeps its timing whatever the boot time; the first
// list can then press A repeatedly until the title takes it. One anchor a script.
struct Script
{
    std::vector<Event> events;  // in vblank order
    std::vector<Event> race;    // in vblank order, relative to the anchor (the race start, or the title's acceptance with `@title`)
    bool hasRace = false;       // a second list is present
    bool anchorTitle = false;   // it is anchored on the title (`@title`), not the race start (`@race`)
};

// Parses `text`; false with `error` ("line N: ...") on a malformed line, an unknown name or a vblank not above the
// previous line's. A script with no events is valid (a connected pad with nothing pressed).
bool ParseScript(std::string_view text, Script& out, std::string& error);

// The script as the pad reports it: the index of the newest event whose vblank is <= `vblank`, or -1 before the
// first. Pure.
long ActiveEvent(const Script& script, uint64_t vblank);

// What one XamInputGetState poll reports (X_INPUT_STATE).
struct Report
{
    uint32_t packet = 0;
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t thumbs[4] = {};  // LX, LY, RX, RY
    long event = -1;  // the active event, -1 before the first
};

// A poller with the packet-number rule of Xenia's SDL driver (sdl_input_driver.cc:190-198, :477-478): the number
// starts at 1 on the first poll and goes up by exactly one on a poll that sees a state change since the previous
// poll, however many events passed between the two; a poll with no change repeats the number. Not thread-safe:
// the import file serializes its polls.
class Poller
{
public:
    explicit Poller(Script script) : script_(std::move(script)) {}
    const Script& script() const { return script_; }
    Report Poll(uint64_t vblank);
    // The race start at `vblank` (the first call counts; false when the script has no `@race` section or it was already set).
    bool RaceStarted(uint64_t vblank);
    // The title screen took input at `vblank` (the first call counts; false unless the script's second list is `@title`).
    bool TitleAccepted(uint64_t vblank);

private:
    Script script_;
    bool raced_ = false;
    uint64_t raceStart_ = 0;
    uint32_t packet_ = 0;  // 0: never polled
    long event_ = -1;
};

// FM4 Recomp: Poller::RaceStarted on the loaded script, if any (imports_input.cpp; logged once). Any thread.
void NotifyRaceStart(uint64_t vblank);
// FM4 Recomp: each swap's draw count (renderer.cpp); a race scene draws 1,000 or more (the @race heuristic): Command-comma pauses
// only there (window_sdl.cpp). Any thread.
void NoteSwapDraws(uint64_t draws);
bool InRaceScene();
// FM4 Recomp: Poller::TitleAccepted on the loaded script, if any (imports_content.cpp XamShowDeviceSelectorUI; logged once).
void NotifyTitleAccepted(uint64_t vblank);

// The names of the buttons held in a report, space-separated ("" when none), for the log.
std::string Describe(uint16_t buttons, uint8_t leftTrigger, uint8_t rightTrigger);

}  // namespace runtime::input
