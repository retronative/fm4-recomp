// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Some text inputs below (the shapes of FM4's phrases, e.g. "x PRESS THE BACK BUTTON y", "NUMBER OF PERFECT DRAFT SCORES", the key
// constants) are taken from FM4 Static Recomp 0.3.x host/tests/prompt_strings_test.cpp (Copyright (c) 2026, i2kiselev, BSD 3-Clause
// License; full notice in runtime/kernel/keyboard_prompts.h). The expected outputs are ours, derived as below for our key set.
//
// Keyboard prompts, CPU only: kernel/keyboard_prompts.h (the keyboard wording of the prompt strings) and input_live.h LastDevice
// (the device used last). Expected values are written out by hand from the rules, not taken from the code's output:
//   - the labels are controls_menu.cpp KeyLabel's (its kMac table, else the SDL name in capitals) of key_bindings.cpp's defaults:
//     A Space -> "SPACE", B Backspace -> "BACKSPACE", X Left Shift -> "LEFT SHIFT", Y Left Ctrl -> "LEFT CONTROL", START Return -> "RETURN",
//     BACK Tab -> "TAB", LB Q, RB E, throttle (RT) W, d-pad down Down -> "DOWN ARROW", steer A / D;
//   - the phrases are the reference's table (prompt_strings.cpp:80-101), each replaced by its `before` + label + `after`;
//   - the dead zones are XInput's documented constants (7849 left, 8689 right, trigger threshold 30; "beyond" = strictly greater).
// The game texts below are test inputs shaped like FM4's phrases; the real EN strings are checked separately
// (check_prompt_string_keys.py, then this program with the TSV it writes: README "String keys").
//
//   test_keyboard_prompts [<EN prompts TSV>]
#include "input_live.h"
#include "keyboard_prompts.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace runtime::prompts;
using runtime::input::InputDevice;
using runtime::input::Keys;
using runtime::input::LastDevice;
using runtime::input::LiveState;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", what);
    failures += ok ? 0 : 1;
}

static std::u16string U(const char* s) { return Wide(s); }
static bool Is(const std::optional<std::u16string>& got, const char* want) { return got && *got == U(want); }

static KeyLabels Defaults()
{
    KeyLabels k;
    k.a = "SPACE", k.b = "BACKSPACE", k.x = "LEFT SHIFT", k.y = "LEFT CONTROL", k.start = "RETURN", k.back = "TAB", k.leftShoulder = "Q",
    k.rightShoulder = "E", k.rightTrigger = "W", k.dpadDown = "DOWN ARROW", k.steerLeft = "A", k.steerRight = "D";
    return k;
}

// Keys used below (the reference's table; driving per kPromptKeys).
constexpr uint16_t kTitleStart = 0x90AB, kInteract = 0x42FB, kMove = 0xDF0C, kLook = 0xEDB4, kExplore = 0x3D74, kEngineTip = 0xED3B,
                   kTabLeft = 0x5222, kTabRight = 0xD225, kViewMessage = 0x76B3, kSteering = 0xF762, kTelemetryExit = 0x2ADE,
                   kRewind = 0x206F;

static void TextRules()
{
    const KeyLabels k = Defaults();
    Expect(Is(KeyboardText(kTitleStart, U("Press A to start the game."), k), "Press SPACE to start"),
           "title screen: 'Press A to start the game.' -> 'Press SPACE to start' (the reference's short form)");
    Expect(Is(KeyboardText(kTitleStart, U("Press A to continue"), k), "Press SPACE to continue"), "'Press A to' -> 'Press SPACE to' (the release's example)");
    Expect(Is(KeyboardText(kInteract, U("x press the A button or pull the RIGHT TRIGGER."), k), "x press SPACE or W."),
           "two phrases in one text: A and the right trigger (throttle W)");
    Expect(Is(KeyboardText(kViewMessage, U("x PRESS THE BACK BUTTON y"), k), "x PRESS TAB y"), "BACK -> TAB");
    Expect(Is(KeyboardText(kTelemetryExit, U("PRESS D-PAD DOWN TO EXIT"), k), "PRESS DOWN ARROW TO EXIT"), "d-pad down -> DOWN ARROW");
    Expect(Is(KeyboardText(kExplore, U("x, press START to bring up y"), k), "x, press RETURN to bring up y"), "START -> RETURN");
    Expect(Is(KeyboardText(kEngineTip, U("Tip: Press the RB button to go"), k), "Tip: Press E to go"), "RB -> E");
    Expect(Is(KeyboardText(kRewind, U("the Rewind button"), k), "the Rewind key (LEFT CONTROL)"), "Rewind button -> Y's key in brackets");
    Expect(Is(KeyboardText(kTitleStart, U("Press the A button to go"), k), "Press SPACE to go"),
           "the longer phrase wins: 'Press the A button' before 'Press A to'");
    Expect(Is(KeyboardText(kTabLeft, U("LB"), k), "Q") && Is(KeyboardText(kTabRight, U("RB"), k), "E"), "the tab strip's LB / RB whole -> Q / E");
    Expect(!KeyboardText(kTabRight, U("NUMBER OF PERFECT DRAFT SCORES"), k), "another table's text under a prompt key stays (no phrase; 'RB' only whole)");
    Expect(Is(KeyboardText(kSteering, U("USE THE LEFT STICK TO STEER"), k), "USE A/D TO STEER"), "a race's left stick -> the steering keys A/D");
    KeyLabels arrows = k;
    arrows.steerLeft = "LEFT ARROW", arrows.steerRight = "RIGHT ARROW";
    Expect(Is(KeyboardText(kSteering, U("USE THE LEFT STICK"), arrows), "USE LEFT/RIGHT ARROW"), "steering on the arrow keys -> LEFT/RIGHT ARROW");
    Expect(!KeyboardText(kMove, U("Use the LEFT STICK to move"), k), "outside a race the left stick has no keys: unchanged (CONTROL for the race case)");
    Expect(!KeyboardText(kLook, U("Use the RIGHT STICK to look"), k), "the right stick has no keys: unchanged");
    Expect(!KeyboardText(0x1234, U("PRESS THE BACK BUTTON"), k), "a key that is not a prompt key: unchanged (CONTROL)");
    KeyLabels rebound = k;
    rebound.back = "C";
    Expect(Is(KeyboardText(kViewMessage, U("PRESS THE BACK BUTTON TO"), rebound), "PRESS C TO"), "a rebind changes the wording");
    KeyLabels rbOnA = k;
    rbOnA.rightShoulder = "A";
    Expect(Is(KeyboardText(kEngineTip, U("Tip: Press the RB button to go"), rbOnA), "Tip: Press A to go"),
           "one pass: RB on the A key reads 'Press A to go', not read again as 'Press A to' -> 'Press SPACE to' (the reference's flaw)");
    KeyLabels driving = k;
    Expect(!KeyboardText(kMove, U("Push forward on the LEFT STICK"), driving), "a phrase with no key is kept whole: the shorter 'the LEFT STICK' inside it is not taken");
    KeyLabels unbound = k;
    unbound.back.clear();
    Expect(!KeyboardText(kViewMessage, U("PRESS THE BACK BUTTON TO"), unbound), "an action bound to none keeps the game's words");
    KeyLabels self = k;
    self.a = "A";
    Expect(Is(KeyboardText(kTitleStart, U("Press A to x. Press A to y."), self), "Press A to x. Press A to y."),
           "a label equal to the button's name ends (no endless replacement) and replaces every occurrence");
    Expect(IsPromptKey(kTitleStart) && !IsPromptKey(0x1234) && kPromptKeys.size() == 24, "24 prompt keys; others are not");
}

static LiveState Pad(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry)
{
    LiveState s;
    s.buttons = buttons, s.leftTrigger = lt, s.rightTrigger = rt, s.leftX = lx, s.leftY = ly, s.rightX = rx, s.rightY = ry;
    return s;
}

static void DeviceRules()
{
    using runtime::input::KeysInUse;
    using runtime::input::PadInUse;
    Expect(!PadInUse(LiveState{}) && PadInUse(Pad(0x1000, 0, 0, 0, 0, 0, 0)), "a button is pad input; an idle pad is not");
    Expect(!PadInUse(Pad(0, 30, 30, 0, 0, 0, 0)) && PadInUse(Pad(0, 31, 0, 0, 0, 0, 0)) && PadInUse(Pad(0, 0, 31, 0, 0, 0, 0)),
           "triggers: 30 is the threshold (not input), 31 is input");
    Expect(!PadInUse(Pad(0, 0, 0, 7849, -7849, 0, 0)) && PadInUse(Pad(0, 0, 0, 7850, 0, 0, 0)) && PadInUse(Pad(0, 0, 0, 0, -7850, 0, 0)),
           "left stick: 7849 inside the dead zone, 7850 beyond (either sign)");
    Expect(!PadInUse(Pad(0, 0, 0, 0, 0, 8689, -8689)) && PadInUse(Pad(0, 0, 0, 0, 0, -8690, 0)) && PadInUse(Pad(0, 0, 0, 0, 0, 0, 8690)),
           "right stick: 8689 inside, 8690 beyond");
    Expect(PadInUse(Pad(0, 0, 0, -32768, 0, 0, 0)), "the stick's most negative value is beyond (no overflow)");
    Keys none, w;
    w.w = true;
    Expect(!KeysInUse(none) && KeysInUse(w), "a key held is keyboard input");

    LastDevice d;
    Expect(d.Current() == InputDevice::Pad && d.Update(false, false) == InputDevice::Pad, "until any input: the pad");
    Expect(d.Update(false, true) == InputDevice::Keyboard, "a key press: the keyboard");
    Expect(d.Update(true, true) == InputDevice::Pad, "a pad press while the key is still held: the pad (its start is new)");
    Expect(d.Update(false, true) == InputDevice::Pad, "the key still held, the pad released: still the pad (no new start)");
    Expect(d.Update(false, false) == InputDevice::Pad && d.Update(false, true) == InputDevice::Keyboard, "the key released and pressed again: the keyboard");
    Expect(d.Update(true, true) == InputDevice::Pad && d.Update(false, false) == InputDevice::Pad, "and back to the pad");
    LastDevice tie;
    (void)tie.Update(false, true);
    Expect(tie.Update(false, false) == InputDevice::Keyboard && tie.Update(true, true) == InputDevice::Keyboard, "both start in one poll: unchanged");
    LastDevice stuck;
    Expect(stuck.Update(true, false) == InputDevice::Pad && stuck.Update(true, true) == InputDevice::Keyboard && stuck.Update(true, false) == InputDevice::Keyboard &&
               stuck.Update(true, false) == InputDevice::Keyboard,
           "a pad that stays off centre does not take the prompts back (an edge, not a level; CONTROL for a level rule)");
    // Connection (as 0.3.x): a start without a pad is the keyboard; connecting gives the pad, disconnecting the keyboard.
    LastDevice plug;
    Expect(plug.Update(false, false, false) == InputDevice::Keyboard, "no pad at the start: the keyboard");
    Expect(plug.Update(false, false, false) == InputDevice::Keyboard, "still no pad: the keyboard");
    Expect(plug.Update(false, false, true) == InputDevice::Pad, "a pad connects, untouched: the pad");
    Expect(plug.Update(false, true, true) == InputDevice::Keyboard, "then a key: the keyboard");
    Expect(plug.Update(false, true, true) == InputDevice::Keyboard, "the pad still connected, no new edge: the keyboard");
    Expect(plug.Update(false, false, false) == InputDevice::Keyboard, "disconnected while the keyboard was last: the keyboard");
    Expect(plug.Update(false, false, true) == InputDevice::Pad && plug.Update(false, false, false) == InputDevice::Keyboard,
           "connected again, then disconnected while the pad was last: the keyboard");
    LastDevice padStart;
    Expect(padStart.Update(false, false, true) == InputDevice::Pad, "a pad at the start: the pad");
}

// The EN prompt strings (check_prompt_string_keys.py: table \t key \t name \t text, text UTF-8 ASCII): which keys the keyboard
// rewords with the default keys. A report for the reviewer, compared with the README's expectation, not a pass/fail.
static void Report(const char* path)
{
    std::ifstream file(path);
    if (!file)
    {
        Expect(false, "the TSV opens");
        return;
    }
    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream fields(line);
        std::string table, key, name, text;
        std::getline(fields, table, '\t'), std::getline(fields, key, '\t'), std::getline(fields, name, '\t'), std::getline(fields, text);
        const auto out = KeyboardText(uint16_t(std::stoul(key, nullptr, 16)), U(text.c_str()), Defaults());
        std::printf("report %s %s %s: %s\n", table.c_str(), key.c_str(), name.c_str(), out ? std::string(out->begin(), out->end()).c_str() : "(unchanged)");
    }
}

int main(int argc, char** argv)
{
    TextRules();
    DeviceRules();
    if (argc > 1)
        Report(argv[1]);
    std::printf("%s\n", failures == 0 ? "all keyboard prompt checks pass" : "keyboard prompt checks FAILED");
    return failures == 0 ? 0 : 1;
}
