// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XamShowKeyboardUI: the system's text-entry dialog (FM4's Photo Mode asks for the photo's name with it). REFERENCE_DERIVED
// from Xenia xenia-canary 67f16c4e315f (kernel/xam/xam_ui.cc:468-523 XamShowKeyboardUI, :113-153 xeXamDispatchDialogEx and
// xeXamDispatchHeadless, :207 xeXamIsUIActive; base/string_util.h copy_and_swap_truncating; kernel_state.cc:811-878
// CompleteOverlappedDeferredEx), re-derived:
// - no buffer: X_ERROR_INVALID_PARAMETER (0x57), nothing shown;
// - with an XOVERLAPPED: its result becomes ERROR_IO_PENDING and its event is reset, the import returns ERROR_IO_PENDING, and
//   a host thread broadcasts XN_SYS_UI (0x9) 1, shows the dialog, broadcasts XN_SYS_UI 0, then completes the XOVERLAPPED;
//   without one, the same steps run on the calling thread and the result is returned;
// - OK: the text, UTF-16 big-endian, truncated to buffer_length - 1 characters and terminated; result 0, extended error 0,
//   length 0. Cancel: the buffer untouched; result 0, extended error X_ERROR_CANCELLED (0x4C7), length 0;
// - headless (no dialog host, Xenia's cvars::headless): the default text is the answer (none: the buffer zeroed).
// Differences, by design: Xenia's post step sleeps 100 ms before XN_SYS_UI 0 (a delay, not a semantic); not modelled.
// The dialog host: the window build shows a native macOS dialog (runtime/kernel/keyboard_ui.cpp, /usr/bin/osascript); a test
// run (opt-in, FM4_KEYBOARD_UI_ANSWERS) answers from a list instead, so a scripted run exercises OK and Cancel.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace runtime::keyboard_ui {

constexpr uint32_t kErrorInvalidParameter = 0x57, kErrorCancelled = 0x4C7, kNotificationSystemUi = 0x9;

struct Request
{
    std::u16string title, description, defaultText;
    uint32_t bufferLength = 0;  // in characters, the terminator included
};

struct Answer
{
    bool cancelled = false;
    std::u16string text;  // OK's text
};

// Shows the dialog and waits for the player (any thread but the window's).
using Host = std::function<Answer(const Request&)>;

// The host the import uses; none set: headless (the default text). Set once at start-up.
void SetHost(Host host);
// The window build's host: a native macOS text dialog (osascript); its Cancel is the answer's cancel. The typed text is the
// program's standard output only; its standard error (where macOS writes its own warnings) only tells a Cancel (-128) apart from a
// failure. `program`: osascript, or a stand-in in the tests.
Host NativeDialogHost(const std::string& program = "/usr/bin/osascript");

// FM4_KEYBOARD_UI_ANSWERS (test only): `ok:<text>` or `cancel`, separated by `;`, used in order, one per dialog. False with
// `error` on a malformed entry. A dialog past the end of the list stops (Setup): a scripted run never waits for a person.
bool ParseAnswers(const std::string& text, std::vector<Answer>& out, std::string& error);
Host ScriptedHost(std::vector<Answer> answers);

// Writes `answer` into the guest buffer as Xenia's close callback does; returns the extended error (0 or kErrorCancelled).
uint32_t WriteAnswer(uint8_t* base, uint32_t buffer, uint32_t bufferLength, const Answer& answer);

// A dialog is shown (xeXamIsUIActive: xam_dialogs_shown_ > 0).
bool Active();

// FM4 Recomp: a system screen's bracket (xeXamDispatchDialogEx's pre / post steps, as above): XN_SYS_UI 1 and the shown count
// up; then the count down and XN_SYS_UI 0. The keyboard dialog and the in-game Keyboard Controls screen (controls_screen.h) use
// it, so XamIsUIActive is true while either is up. Any thread; each Begin has exactly one End.
void BeginSystemUi(uint8_t* base);
void EndSystemUi(uint8_t* base);

}  // namespace runtime::keyboard_ui
