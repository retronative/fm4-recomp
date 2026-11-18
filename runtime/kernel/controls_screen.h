// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: the in-game Keyboard Controls screen's life in the runtime. The player pauses the race (the game's own pause
// menu, START) and presses Command-comma: the launcher's Keyboard Controls (FM4 Recomp --controls-panel, app/setup/ControlsPanel.swift)
// opens over the game window, with the same rebinding, conflict question and Reset to Defaults as before, and writes
// $FM4_RECOMP_HOME/keybindings.conf. While it is up:
//   - the game is told a system screen is up (XN_SYS_UI 1, as for the Guide; keyboard_ui.h BeginSystemUi, so XamIsUIActive is
//     true too) -- ReCarbon does the same for its settings menu (runtime/ui/ui.cpp:1101-1128, kernel/xam.h:17-19 at 66401ea);
//   - the game's pad is idle (InputBlocked; input_live.h InputGate latches what is still held when it closes).
// When the screen ends, the bindings are re-read (key_bindings.h ReloadKeyBindings) before XN_SYS_UI 0, so the new keys are in
// effect the moment gameplay resumes. Window thread only, except Open()/InputBlocked() (any thread). Pure: the process and the
// notification are hooks (controls_panel_sdl.cpp in the window build; fakes in tests/runtime/test_runtime.cpp).
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <utility>

namespace runtime::controls {

struct Hooks
{
    std::function<bool()> start;          // shows the screen (starts the panel process); false: it could not start (logged there)
    std::function<bool()> finished;       // polled every loop turn while open: the screen has ended
    std::function<void(bool)> systemUi;   // true: XN_SYS_UI 1 (the game pauses); false: XN_SYS_UI 0 (it resumes)
    std::function<const char*()> busy;    // the other system screen that is up (a keyboard dialog, the Disc 2 setup), or nullptr
    std::string bindingsPath;             // $FM4_RECOMP_HOME/keybindings.conf: the file the screen writes, re-read on close
};

class Screen
{
public:
    explicit Screen(Hooks hooks) : hooks_(std::move(hooks)) {}
    // The shortcut. False (logged) when the screen is already up, another system screen is, or it could not start.
    bool RequestOpen(const char* how);
    // Any thread (the game's Pause menu row, config/title_hooks.toml menu_row_action): the window loop opens the screen at its next
    // Pump, as for the shortcut.
    void RequestFromGame(const char* how) { requested_.store(how, std::memory_order_release); }
    // Every loop turn. A request from the game opens the screen (RequestOpen). Once the screen has ended: the bindings re-read, then XN_SYS_UI 0. True on that turn only (the window
    // loop then resets SDL's keyboard state, so a key that went down or up while the panel had the focus reads as up).
    bool Pump();
    bool Open() const { return open_.load(std::memory_order_acquire); }

private:
    Hooks hooks_;
    std::atomic<bool> open_{false};
    std::atomic<const char*> requested_{nullptr};
};

// The process's screen (main.cpp: --controls-panel with --renderer window and FM4_INPUT_LIVE=1); null otherwise.
void SetService(Screen* screen);
Screen* Service();
// Any thread: the screen is up, so the game's pad is idle.
bool InputBlocked();

}  // namespace runtime::controls
