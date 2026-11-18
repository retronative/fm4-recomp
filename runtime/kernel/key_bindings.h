// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: the live pad's keyboard bindings (FM4_INPUT_LIVE, input_live.h). Each of the 17 keyboard actions (the Keys fields)
// has one or more keys; a key belongs to at most one action. Without --keybindings the defaults apply (the map window_sdl.cpp
// hard-coded before: A/D steer, W throttle, S brake, arrows d-pad, Enter A, Backspace and Escape B, X, Y, Space START, Tab BACK,
// Q/E shoulders, C camera). With --keybindings <file> ($FM4_RECOMP_HOME/keybindings.conf, written by the in-game Keyboard Controls
// screen, controls_screen.h; play.sh passes it when it exists, locations.sh loc_game_args) the file replaces them, every action named:
//
//   # comment
//   throttle = W
//   b = Backspace
//   b = Escape          (one key per line; an action with several keys repeats its id; SDL's comma key is named ",")
//   camera = none       (FM4 Recomp: the action has no key; cleared in the game's KEYBOARD CONTROLS)
//
// Key names and numbers are SDL 3's scancode names and values (SDL_GetScancodeName / SDL_GetScancodeFromName,
// SDL3/SDL_scancode.h: the USB HID keyboard usage IDs), restricted to the keys a Mac keyboard reports that SDL maps without
// layout-dependent swaps (no ` or the ISO section key, no Caps Lock, no Command). window_sdl.cpp checks this table against the
// linked SDL at start-up. This layer is pure (no SDL, no stop): parsing reports "line N: ..."; LoadKeyBindings turns it into a
// Setup stop naming the file.
#pragma once
#include "input_live.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace runtime::input {

constexpr size_t kActionCount = 17;

struct Action
{
    const char* id;        // the file's name (stable, lowercase)
    bool Keys::*field;     // the Keys field it presses
};
extern const Action kActions[kActionCount];

struct KeyName
{
    const char* name;  // SDL_GetScancodeName
    uint16_t scancode;  // SDL_Scancode
};
extern const KeyName kKeyNames[];
extern const size_t kKeyNameCount;

// The scancode of an SDL key name in kKeyNames (exact, case-sensitive), or -1; the name of a scancode, or nullptr.
int ScancodeFromName(std::string_view name);
const char* NameOfScancode(int scancode);

struct KeyBindings
{
    std::vector<uint16_t> keys[kActionCount];  // per kActions entry, in file order
    bool operator==(const KeyBindings&) const = default;
};

KeyBindings DefaultKeyBindings();

// Parses `text`; false with `error` ("line N: ...", or "no key for '<action>'") on a line without '=', an unknown action, an
// unknown key name, a key bound twice (naming both actions), or an action not named at all (`none` names it without a key).
bool ParseKeyBindings(std::string_view text, KeyBindings& out, std::string& error);

// The Keys pressed: an action is down when any of its keys is (state: SDL_GetKeyboardState's array of `count` entries).
Keys KeysFromState(const KeyBindings& bindings, const bool* state, size_t count);

// "steer_left A, steer_right D, ..., b Backspace/Escape, ..." for the log.
std::string DescribeKeyBindings(const KeyBindings& bindings);

// The bindings the window loop reads: the defaults until LoadKeyBindings (main.cpp, before the window opens). LoadKeyBindings
// stops (9) on an unreadable or invalid file, naming it and the line.
void LoadKeyBindings(const char* path);
// FM4 Recomp (in-game Keyboard Controls, controls_screen.h): the active set is swapped whole under a lock, so any thread may hold
// a snapshot while another thread reloads; a snapshot never changes.
std::shared_ptr<const KeyBindings> ActiveKeyBindings();
// The file loaded, or "" (the defaults).
std::string KeyBindingsPath();

// FM4 Recomp: re-reads `path` while the game runs (the Keyboard Controls screen closed; never stops). No file: the defaults (its
// Reset to Defaults removes the file). A valid file: its bindings. An unreadable or invalid file: the active bindings stay as they
// are and `message` says why (the screen validates with the same rules before it writes, so this is a defect, logged; the next
// launch's LoadKeyBindings stops on that file).
struct ReloadResult
{
    bool applied = false;  // the active set is now the file's (or the defaults)
    bool changed = false;  // ... and differs from the set before
    std::string message;   // for the [input] log line
};
ReloadResult ReloadKeyBindings(const std::string& path);

}  // namespace runtime::input
