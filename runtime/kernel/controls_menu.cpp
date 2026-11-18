// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The in-game KEYBOARD CONTROLS submenu's logic (controls_menu.h). Tested by tests/runtime/test_runtime.cpp.
#include "controls_menu.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unistd.h>

namespace runtime::controls {

namespace {
// KeyBindings.swift keyActions' labels, upper case.
const char* const kLabels[input::kActionCount] = {"STEER LEFT", "STEER RIGHT", "THROTTLE", "BRAKE", "D-PAD UP", "D-PAD DOWN",
                                                  "D-PAD LEFT", "D-PAD RIGHT", "A (CONFIRM)", "B (BACK)", "X", "Y", "START",
                                                  "BACK", "LEFT SHOULDER", "RIGHT SHOULDER", "CAMERA"};

bool Has(const std::vector<uint16_t>& keys, uint16_t key) { return std::find(keys.begin(), keys.end(), key) != keys.end(); }
}  // namespace

Change ChangeAssigning(const input::KeyBindings& bindings, uint16_t key, size_t action)
{
    Change c;
    c.action = action, c.key = key;
    for (size_t a = 0; a < input::kActionCount; a++)
        if (a != action && Has(bindings.keys[a], key))
            c.from = int(a);
    if (c.from < 0)
        return c;
    std::vector<uint16_t> rest, previous;
    for (const uint16_t k : bindings.keys[size_t(c.from)])
        if (k != key)
            rest.push_back(k);
    for (const uint16_t k : bindings.keys[action])
        if (k != key)
            previous.push_back(k);
    c.fromGets = rest.empty() ? previous : rest;
    return c;
}

input::KeyBindings Applying(input::KeyBindings bindings, const Change& change)
{
    bindings.keys[change.action] = {change.key};
    if (change.from >= 0)
        bindings.keys[size_t(change.from)] = change.fromGets;
    return bindings;
}

std::string Serialize(const input::KeyBindings& bindings)
{
    std::string text = "# FM4 Recomp keyboard bindings, written by FM4 Recomp's Keyboard Controls. Delete this file for the defaults.\n";
    for (size_t a = 0; a < input::kActionCount; a++)
    {
        if (bindings.keys[a].empty())
            text += std::string(input::kActions[a].id) + " = none\n";
        for (const uint16_t k : bindings.keys[a])
            if (const char* name = input::NameOfScancode(k))
                text += std::string(input::kActions[a].id) + " = " + name + "\n";
    }
    return text;
}

bool SaveBindings(const std::string& path, const input::KeyBindings& bindings, std::string& error)
{
    const std::string temporary = path + ".writing";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << Serialize(bindings);
        out.flush();
        if (!out)
        {
            error = "cannot write " + temporary;
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        error = "cannot replace " + path + ": " + std::strerror(errno);
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

bool ResetBindings(const std::string& path, std::string& error)
{
    if (unlink(path.c_str()) == 0 || errno == ENOENT)
        return true;
    error = "cannot remove " + path + ": " + std::strerror(errno);
    return false;
}

const char* ActionLabel(size_t action) { return action < input::kActionCount ? kLabels[action] : "?"; }

std::string KeyLabel(uint16_t scancode)
{
    const char* name = input::NameOfScancode(scancode);
    if (name == nullptr)
        return "?";
    static const struct { const char* sdl; const char* label; } kMac[] = {
        {"Backspace", "DELETE"}, {"Delete", "FORWARD DELETE"}, {"Escape", "ESC"}, {"Up", "UP ARROW"}, {"Down", "DOWN ARROW"},
        {"Left", "LEFT ARROW"}, {"Right", "RIGHT ARROW"}, {"Left Ctrl", "LEFT CONTROL"}, {"Right Ctrl", "RIGHT CONTROL"}};
    for (const auto& m : kMac)
        if (std::strcmp(name, m.sdl) == 0)
            return m.label;
    std::string label = name;
    for (char& ch : label)
        if (ch >= 'a' && ch <= 'z')
            ch = char(ch - 'a' + 'A');
    return label;
}

std::string KeysLabel(const std::vector<uint16_t>& keys)
{
    if (keys.empty())
        return "NONE";
    std::string text;
    for (const uint16_t k : keys)
        text += (text.empty() ? "" : " / ") + KeyLabel(k);
    return text;
}

// A button's prompt with its keyboard equivalent (the keys bound to it now): "A (SPACE)"; just "A" when it has no key.
std::string Menu::Hint(size_t action, const char* button) const
{
    return bindings_.keys[action].empty() ? std::string(button) : std::string(button) + " (" + KeysLabel(bindings_.keys[action]) + ")";
}

Menu::Menu(input::KeyBindings bindings, std::string path) : bindings_(std::move(bindings)), path_(std::move(path)) {}

Menu::Row Menu::RowText(size_t row) const
{
    if (row == kResetRow)
        return {"RESET TO DEFAULTS", ""};
    if (row >= input::kActionCount)
        return {"", ""};
    if (row == row_ && mode_ == Mode::WaitingForKey)
        return {kLabels[row], "PRESS A KEY"};
    return {kLabels[row], KeysLabel(bindings_.keys[row])};
}

std::string Menu::Prompt() const
{
    switch (mode_)
    {
    case Mode::WaitingForKey:
        return notice_.empty() ? std::string("PRESS THE NEW KEY FOR ") + kLabels[row_] + ". ESC OR B CANCELS." : notice_;
    case Mode::ConfirmReplace:
        return KeyLabel(pending_.key) + " IS USED BY " + kLabels[pending_.from] + ". " + Hint(8, "A") + ": REPLACE (" +
               kLabels[pending_.from] + " WILL USE " + KeysLabel(pending_.fromGets) + "). " + Hint(9, "B") + ": CANCEL.";
    case Mode::ConfirmReset:
        return "RESET ALL KEYS TO THE DEFAULTS? " + Hint(8, "A") + ": RESET. " + Hint(9, "B") + ": CANCEL.";
    case Mode::Browse:
        break;
    }
    return notice_.empty() ? Hint(8, "A") + ": SET KEY   " + Hint(11, "Y") + ": CLEAR   " + Hint(10, "X") + ": DEFAULT   " + Hint(9, "B") +
                                 ": BACK. THE CONTROLLER LAYOUT IS FIXED."
                           : notice_;
}

void Menu::Select(size_t row)
{
    if (mode_ != Mode::Browse || row >= kRows)
        return;
    notice_.clear();
    row_ = row;
    mode_ = row == kResetRow ? Mode::ConfirmReset : Mode::WaitingForKey;
}

void Menu::Clear(size_t row)
{
    if (mode_ != Mode::Browse || row >= input::kActionCount)
        return;
    input::KeyBindings next = bindings_;
    next.keys[row].clear();
    row_ = row;
    Save(next, std::string(kLabels[row]) + " HAS NO KEY NOW. X: ITS DEFAULT KEY BACK.");
}

void Menu::RestoreDefault(size_t row)
{
    if (mode_ != Mode::Browse || row >= input::kActionCount)
        return;
    row_ = row;
    notice_.clear();
    const std::vector<uint16_t> defaults = input::DefaultKeyBindings().keys[row];
    if (defaults.empty() || bindings_.keys[row] == defaults)
    {
        notice_ = std::string(kLabels[row]) + " ALREADY HAS ITS DEFAULT KEY.";
        return;
    }
    pending_ = ChangeAssigning(bindings_, defaults[0], row);
    if (pending_.from >= 0)
        mode_ = Mode::ConfirmReplace;
    else
        Save(Applying(bindings_, pending_), std::string(kLabels[row]) + " HAS ITS DEFAULT KEY AGAIN.");
}

void Menu::Key(int scancode)
{
    if (mode_ != Mode::WaitingForKey)
        return;
    if (scancode == 41)  // Escape (SDL_SCANCODE_ESCAPE): cancels the wait, as in ReCarbon (settings_menu.cpp:1329-1333 at 66401ea)
    {
        Cancel();
        return;
    }
    if (input::NameOfScancode(scancode) == nullptr)
    {
        notice_ = "THAT KEY CANNOT BE USED IN THE GAME. PRESS ANOTHER KEY, OR B: CANCEL.";
        return;
    }
    notice_.clear();
    pending_ = ChangeAssigning(bindings_, uint16_t(scancode), row_);
    if (pending_.from >= 0)
        mode_ = Mode::ConfirmReplace;
    else
        Save(Applying(bindings_, pending_), "SAVED. THE NEW KEYS APPLY WHEN YOU RETURN TO THE GAME.");
}

void Menu::Confirm()
{
    if (mode_ == Mode::ConfirmReplace)
        Save(Applying(bindings_, pending_), "SAVED. THE NEW KEYS APPLY WHEN YOU RETURN TO THE GAME.");
    else if (mode_ == Mode::ConfirmReset)
    {
        std::string error;
        mode_ = Mode::Browse;
        if (!ResetBindings(path_, error))
            notice_ = "COULD NOT RESET: " + error;
        else
        {
            changed_ = changed_ || !(bindings_ == input::DefaultKeyBindings());
            bindings_ = input::DefaultKeyBindings();
            notice_ = "RESET TO THE DEFAULTS. THE KEYS APPLY WHEN YOU RETURN TO THE GAME.";
        }
    }
}

bool Menu::Cancel()
{
    if (mode_ == Mode::Browse)
        return false;
    mode_ = Mode::Browse;
    notice_.clear();
    return true;
}

void Menu::Save(const input::KeyBindings& next, const std::string& done)
{
    std::string error;
    mode_ = Mode::Browse;
    if (!SaveBindings(path_, next, error))
    {
        notice_ = "COULD NOT SAVE: " + error;
        return;
    }
    changed_ = changed_ || !(next == bindings_);
    bindings_ = next;
    notice_ = done;
}

void Session::Open(input::KeyBindings bindings, std::string path, uint64_t nowNs)
{
    const std::lock_guard lock(mutex_);
    menu_.emplace(std::move(bindings), std::move(path));
    releasedSinceOpen_ = false, openedNs_ = nowNs;
    inFlightUntil_.store(0, std::memory_order_release);
    lastPad_ = 0xFFFF;  // a button already down when it opens is not a new press
    After();
}

bool Session::IsOpen() const
{
    const std::lock_guard lock(mutex_);
    return menu_.has_value();
}

bool Session::OwnsInput() const { return owns_.load(std::memory_order_acquire); }

bool Session::AcceptsConfirm(uint64_t nowNs) const
{
    const std::lock_guard lock(mutex_);
    return releasedSinceOpen_ && nowNs >= openedNs_ + kOpenSettleNs;
}

void Session::Select(size_t row)
{
    const std::lock_guard lock(mutex_);
    if (menu_)
        menu_->Select(row);
    armed_ = false;  // capture starts once Confirm is released (Pad)
    After();
}

void Session::Clear(size_t row)
{
    const std::lock_guard lock(mutex_);
    if (menu_)
        menu_->Clear(row);
    After();
}

void Session::RestoreDefault(size_t row)
{
    const std::lock_guard lock(mutex_);
    if (menu_)
        menu_->RestoreDefault(row);
    After();
}

bool Session::Back()
{
    const std::lock_guard lock(mutex_);
    const bool cancelled = menu_ && menu_->Cancel();
    After();
    return cancelled;
}

Session::View Session::Snapshot() const
{
    const std::lock_guard lock(mutex_);
    View v;
    if (!menu_)
        return v;
    v.mode = menu_->CurrentMode();
    for (size_t r = 0; r < Menu::kRows; r++)
        v.rows.push_back(menu_->RowText(r));
    for (size_t a = 0; a < input::kActionCount; a++)
        v.keys.push_back(menu_->Bindings().keys[a]);
    for (size_t r = 0; r < input::kActionCount; r++)
        if (v.rows[r].value == "PRESS A KEY")
            v.waitingRow = r;
    v.prompt = menu_->Prompt();
    return v;
}

bool Session::Close()
{
    const std::lock_guard lock(mutex_);
    const bool changed = menu_ && menu_->Changed();
    menu_.reset();
    owns_.store(false, std::memory_order_release);
    pulse_.store(false, std::memory_order_release);
    inFlightUntil_.store(0, std::memory_order_release);
    return changed;
}

void Session::KeyDown(int scancode, const input::KeyBindings& active)
{
    const std::lock_guard lock(mutex_);
    if (!menu_ || !menu_->OwnsInput())
        return;
    const bool isA = Has(active.keys[8], uint16_t(scancode)), isB = Has(active.keys[9], uint16_t(scancode));
    switch (menu_->CurrentMode())
    {
    case Menu::Mode::WaitingForKey:
        if (!armed_)
            return;  // Confirm not released yet
        menu_->Key(scancode);
        break;
    case Menu::Mode::ConfirmReplace:
    case Menu::Mode::ConfirmReset:
        if (isA)
            menu_->Confirm();
        else if (isB)
            menu_->Cancel();
        else
            return;
        break;
    case Menu::Mode::Browse:
        return;
    }
    pulse_.store(true, std::memory_order_release);
    After();
}

void Session::Pad(uint16_t buttons, bool confirmHeld)
{
    const std::lock_guard lock(mutex_);
    const uint16_t pressed = uint16_t(buttons & ~lastPad_);
    lastPad_ = buttons;
    if (!(buttons & input::button::kA) && !confirmHeld)
        releasedSinceOpen_ = true;
    if (menu_ && menu_->CurrentMode() == Menu::Mode::WaitingForKey && !armed_ && !(buttons & input::button::kA) && !confirmHeld)
        armed_ = true;
    if (!menu_ || !menu_->OwnsInput())
        return;
    if (pressed & input::button::kB)
        menu_->Cancel();
    else if ((pressed & input::button::kA) && menu_->CurrentMode() != Menu::Mode::WaitingForKey)
        menu_->Confirm();
    else
        return;
    pulse_.store(true, std::memory_order_release);
    After();
}

bool Session::TakePulse(uint64_t nowNs)
{
    const bool pulse = pulse_.exchange(false, std::memory_order_acq_rel);
    if (pulse)
        inFlightUntil_.store(nowNs + 500'000'000, std::memory_order_release);
    return pulse;
}

bool Session::ConsumePulse(uint64_t nowNs)
{
    const uint64_t until = inFlightUntil_.exchange(0, std::memory_order_acq_rel);
    return until != 0 && nowNs <= until;
}

void Session::After() { owns_.store(menu_ && menu_->OwnsInput(), std::memory_order_release); }

Session& KeyboardMenu()
{
    static Session session;
    return session;
}

}  // namespace runtime::controls
