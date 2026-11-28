// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (diagnostic, keyboard prompts): FM4_DIAG_PROMPTS_KEYBOARD=1 makes the prompts name keyboard keys from the start, as
// if a key had been pressed, so a bounded offscreen run driven by FM4_INPUT_SCRIPT (which feeds the pad, never the keyboard; the
// project's offscreen policy) can show the keyboard wording. Read once.
#pragma once
#include <cstdlib>
#include <cstring>

namespace runtime::diag {

inline bool PromptsKeyboard()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_PROMPTS_KEYBOARD");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

}  // namespace runtime::diag
