// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The in-game Keyboard Controls screen (controls_screen.h). Tested by tests/runtime/test_runtime.cpp.
#include "controls_screen.h"
#include "key_bindings.h"
#include <cstdio>

namespace runtime::controls {

bool Screen::RequestOpen(const char* how)
{
    if (Open())
        return false;  // already up: the panel has the focus, so this is a stray repeat
    if (const char* other = hooks_.busy ? hooks_.busy() : nullptr)
    {
        std::fprintf(stderr, "[controls] Keyboard Controls not opened (%s): %s is up\n", how, other);
        return false;
    }
    if (!hooks_.start())
        return false;
    open_.store(true, std::memory_order_release);  // the pad is idle from the next poll
    hooks_.systemUi(true);
    std::fprintf(stderr, "[controls] Keyboard Controls opened (%s): XN_SYS_UI 1, the game's pad idle\n", how);
    return true;
}

bool Screen::Pump()
{
    if (const char* how = requested_.exchange(nullptr, std::memory_order_acq_rel))
    {
        RequestOpen(how);
        return false;
    }
    if (!Open() || !hooks_.finished())
        return false;
    // The new keys first, so they are in effect when the game resumes.
    const input::ReloadResult reload = input::ReloadKeyBindings(hooks_.bindingsPath);
    if (reload.applied && reload.changed)
        std::fprintf(stderr, "[input] keyboard bindings reloaded (%s): %s\n", reload.message.c_str(),
                     input::DescribeKeyBindings(*input::ActiveKeyBindings()).c_str());
    else if (reload.applied)
        std::fprintf(stderr, "[input] keyboard bindings unchanged (%s)\n", reload.message.c_str());
    else
        std::fprintf(stderr, "[input] keyboard bindings NOT reloaded: %s\n", reload.message.c_str());
    open_.store(false, std::memory_order_release);
    hooks_.systemUi(false);
    std::fprintf(stderr, "[controls] Keyboard Controls closed: XN_SYS_UI 0\n");
    return true;
}

namespace {
std::atomic<Screen*> service{nullptr};
}  // namespace

void SetService(Screen* screen) { service.store(screen); }
Screen* Service() { return service.load(); }

bool InputBlocked()
{
    const Screen* s = service.load();
    return s != nullptr && s->Open();
}

}  // namespace runtime::controls
