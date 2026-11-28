// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: the in-game KEYBOARD CONTROLS submenu's logic, shared by every game menu that opens it (the Pause menu first;
// config/title_hooks.toml hosts it in one of FM4's own option screens). Pure: no SDL, no guest memory, no stop. The rules are the
// launcher's Keyboard Controls (app/setup/KeyBindings.swift, SetupView.swift), now in one place for the game:
//   - assigning a key replaces the action's keys with it; the key leaves the action that had it, and if that leaves that action
//     with no key it takes the selected action's previous keys (a swap), so every action keeps a key; a key another action has
//     asks first ("S IS USED BY BRAKE");
//   - Reset to Defaults asks first and removes the file (no file: the defaults);
//   - every change is written at once to $FM4_RECOMP_HOME/keybindings.conf, in the format key_bindings.h parses, atomically.
// The new keys apply to the game when the submenu closes (Back to the menu that opened it): Close() reloads them
// (key_bindings.h ReloadKeyBindings), so navigation inside the submenu keeps the keys it started with.
#pragma once
#include "key_bindings.h"
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace runtime::controls {

struct Change
{
    size_t action = 0;
    uint16_t key = 0;
    int from = -1;                  // the action that has the key now, -1 if none
    std::vector<uint16_t> fromGets;  // its keys afterwards
};
Change ChangeAssigning(const input::KeyBindings& bindings, uint16_t key, size_t action);
input::KeyBindings Applying(input::KeyBindings bindings, const Change& change);
// The file: a comment, then one "action = key" line per key, in action order (KeyBindings.swift serialized()).
std::string Serialize(const input::KeyBindings& bindings);
// Writes `bindings` to `path` (a temporary file renamed over it); false with `error`.
bool SaveBindings(const std::string& path, const input::KeyBindings& bindings, std::string& error);
// Removes `path` (absent already: fine); false with `error`.
bool ResetBindings(const std::string& path, std::string& error);

// The game's style: upper case, ASCII (FM4's menu font). "STEER LEFT"; "BACKSPACE" for SDL's "Backspace" (a Mac keyboard's delete key), "UP ARROW" for "Up".
const char* ActionLabel(size_t action);
std::string KeyLabel(uint16_t scancode);
std::string KeysLabel(const std::vector<uint16_t>& keys);  // "BACKSPACE / ESC"

class Menu
{
public:
    static constexpr size_t kResetRow = input::kActionCount;  // the last row
    static constexpr size_t kRows = input::kActionCount + 1;
    enum class Mode { Browse, WaitingForKey, ConfirmReplace, ConfirmReset };
    struct Row
    {
        std::string label, value;
    };

    // `bindings`: the active set when the submenu opens; `path`: the file it writes.
    Menu(input::KeyBindings bindings, std::string path);

    Mode CurrentMode() const { return mode_; }
    const input::KeyBindings& Bindings() const { return bindings_; }
    Row RowText(size_t row) const;
    // The help line under the list: what A/B do now, or the question, or the last result.
    std::string Prompt() const;
    // While not browsing, the menu owns the input: the game's pad is idle and keys come through Key/Confirm/Cancel.
    bool OwnsInput() const { return mode_ != Mode::Browse; }

    // A on a row (the game's list): an action waits for its key; Reset to Defaults asks.
    void Select(size_t row);
    // Y on an action's row: it has no key (saved as `none`). X: its default key back (asks when another action has it now).
    void Clear(size_t row);
    void RestoreDefault(size_t row);
    // A keyboard key (SDL scancode) while waiting: Escape cancels (as in ReCarbon); not bindable -> asked again; another action's
    // key -> asks; else saved.
    void Key(int scancode);
    // A (pad or the key bound to A) on a question: replace / reset.
    void Confirm();
    // B (pad or the key bound to B): leaves a wait or a question; true when it did (false: browsing, so B closes the submenu).
    bool Cancel();
    // The submenu closes: true when the file changed while it was open (the caller reloads the game's bindings).
    bool Changed() const { return changed_; }

private:
    void Save(const input::KeyBindings& next, const std::string& done);
    std::string Hint(size_t action, const char* button) const;

    input::KeyBindings bindings_;
    std::string path_;
    Mode mode_ = Mode::Browse;
    size_t row_ = 0;
    Change pending_;
    std::string notice_;
    bool changed_ = false;
};

// The submenu while a game screen shows it, shared by the game's UI thread (the screen's hooks: Open, Select, Back, View, Close)
// and the window thread (keys and the pad while the submenu owns the input). While it owns the input the game's pad is idle; each
// change it makes asks for one refresh pulse (an X press the window loop sends the game) so the screen redraws on the game's thread.
class Session
{
public:
    // `nowNs`: a steady clock (tests pass their own).
    void Open(input::KeyBindings bindings, std::string path, uint64_t nowNs = 0);
    bool IsOpen() const;
    bool OwnsInput() const;
    // Game thread. A on `row`; Y (clear) and X (default) on it; B (true: it cancelled a wait or a question; false: the screen
    // closes); the rows and the prompt.
    void Select(size_t row);
    void Clear(size_t row);
    void RestoreDefault(size_t row);
    bool Back();
    // Game thread: an A the game delivered may select a row (start a capture or the reset question). Not the press that opened
    // the submenu: false until Confirm (the pad's A and every key bound to A) has been released since Open, and for
    // kOpenSettleNs after it (the game can deliver the opener's A to the new screen on either edge).
    bool AcceptsConfirm(uint64_t nowNs) const;
    static constexpr uint64_t kOpenSettleNs = 250'000'000;
    struct View
    {
        Menu::Mode mode = Menu::Mode::Browse;
        std::vector<Menu::Row> rows;
        std::vector<std::vector<uint16_t>> keys;  // per action, the keys (the row's value)
        size_t waitingRow = Menu::kRows;          // the row showing "PRESS A KEY"
        std::string prompt;
    };
    View Snapshot() const;
    // The screen closed: true when the bindings changed (the caller reloads them). The session ends.
    bool Close();

    // Window thread. A key went down (SDL scancode, no repeat): waiting -> the new key; a question -> the key bound to A answers
    // yes, the key bound to B no. The pad's buttons (X_INPUT_GAMEPAD bits): A answers yes, B cancels a wait or a question.
    // Key capture starts once Confirm is released (the pad's A and every key bound to A up), so the press that started it is
    // never taken as the new key. `confirmHeld`: a key bound to A is down (window loop, every poll).
    void KeyDown(int scancode, const input::KeyBindings& active);
    void Pad(uint16_t buttons, bool confirmHeld = false);
    // One refresh pulse pending (the window loop sends X while true, then clears it).
    bool TakePulse(uint64_t nowNs);
    // Game thread: the X that arrived was that pulse (true once per pulse sent, within 500 ms of it), not the player's X. `nowNs`:
    // a steady clock (tests pass their own).
    bool ConsumePulse(uint64_t nowNs);

private:
    mutable std::mutex mutex_;
    std::optional<Menu> menu_;
    std::atomic<bool> owns_{false}, pulse_{false};
    std::atomic<uint64_t> inFlightUntil_{0};  // a pulse sent, until this steady time (ns); 0 none
    uint16_t lastPad_ = 0;
    bool armed_ = false;  // waiting, and Confirm has been released since
    bool releasedSinceOpen_ = true;  // Confirm released since Open
    uint64_t openedNs_ = 0;
    void After();  // under the lock: owns_ and pulse_ from the menu
};

// The process's session (one submenu at a time).
Session& KeyboardMenu();

}  // namespace runtime::controls
