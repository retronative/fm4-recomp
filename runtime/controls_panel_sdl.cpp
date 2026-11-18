// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The window build's Keyboard Controls panel process (controls_panel_sdl.h). The spawn follows disc_setup_sdl.cpp.
#include "controls_panel_sdl.h"
#include "stops.h"
#include "video/window_sdl.h"
#include <cerrno>
#include <chrono>
#include <objc/message.h>
#include <objc/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace runtime::controls {
namespace {

// The panel gets the keyboard: macOS lets the active app (the game) activate another, so the game hands the focus over, as a
// click would (without it, a key typed for a row reached the game, whose pad is idle meanwhile: 2026-11-15). AppKit through the
// Objective-C runtime (NSRunningApplication runningApplicationWithProcessIdentifier: / activateWithOptions:
// NSApplicationActivateAllWindows); false until the panel's process is registered as an application.
bool Activate(pid_t pid)
{
    Class running = objc_getClass("NSRunningApplication");
    if (running == nullptr)
        return false;
    using Lookup = id (*)(Class, SEL, pid_t);
    using ActivateWith = bool (*)(id, SEL, unsigned long);
    id app = reinterpret_cast<Lookup>(objc_msgSend)(running, sel_registerName("runningApplicationWithProcessIdentifier:"), pid);
    return app != nullptr && reinterpret_cast<ActivateWith>(objc_msgSend)(app, sel_registerName("activateWithOptions:"), 1ul);
}

struct Child
{
    std::string panel;
    pid_t pid = 0;
    bool activated = false;
    std::chrono::steady_clock::time_point started;

    bool Start()
    {
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);  // its own group: the game's SIGINT to its group is not the panel's
        posix_spawnattr_setpgroup(&attributes, 0);
        const SDL_Rect frame = runtime::video::window::WindowFrame();
        const std::string frameArg = std::to_string(frame.x) + "," + std::to_string(frame.y) + "," + std::to_string(frame.w) + "," + std::to_string(frame.h);
        const char* const argv[] = {panel.c_str(), "--controls-panel", "--frame", frameArg.c_str(), nullptr};
        // stdout and stderr are the game's (play.sh: game.out, game.log).
        const int spawned = posix_spawn(&pid, panel.c_str(), nullptr, &attributes, const_cast<char* const*>(argv), environ);
        posix_spawnattr_destroy(&attributes);
        if (spawned != 0)
        {
            pid = 0;
            std::fprintf(stderr, "[controls] the Keyboard Controls panel %s could not be started: %s\n", panel.c_str(), std::strerror(spawned));
            return false;
        }
        std::fprintf(stderr, "[controls] panel %s started (pid %d)\n", panel.c_str(), int(pid));
        activated = false, started = std::chrono::steady_clock::now();
        return true;
    }

    bool Finished()
    {
        if (pid <= 0)
            return true;
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == 0 || (r < 0 && errno == EINTR))
        {
            if (!activated && std::chrono::steady_clock::now() - started < std::chrono::seconds(3) && Activate(pid))
            {
                activated = true;
                std::fprintf(stderr, "[controls] panel (pid %d) given the keyboard focus\n", int(pid));
            }
            return false;
        }
        std::fprintf(stderr, "[controls] panel (pid %d) ended: %s %d\n", int(pid), r < 0 ? "waitpid failed, errno" : WIFEXITED(status) ? "exit" : "signal",
                     r < 0 ? errno : WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        pid = 0;
        return true;
    }
};

}  // namespace

Hooks MakeSdlHooks(const std::string& panel, std::function<void(bool)> systemUi, std::function<const char*()> busy)
{
    if (access(panel.c_str(), X_OK) != 0)
        Stop(StopCode::Setup, "--controls-panel %s is not an executable (the kit's launcher, bin/fm4recomp-setup)", panel.c_str());
    // The file the panel writes (app/setup/Core.swift Paths.keyBindingsFile) and play.sh passes (locations.sh loc_game_args).
    const char* home = std::getenv("FM4_RECOMP_HOME");
    if (home == nullptr || *home == 0)
        Stop(StopCode::Setup, "--controls-panel needs FM4_RECOMP_HOME (the player's data folder; play.sh sets it)");
    auto child = std::make_shared<Child>();
    child->panel = panel;
    Hooks hooks;
    hooks.start = [child] { return child->Start(); };
    hooks.finished = [child] { return child->Finished(); };
    hooks.systemUi = std::move(systemUi);
    hooks.busy = std::move(busy);
    hooks.bindingsPath = std::string(home) + "/keybindings.conf";
    return hooks;
}

}  // namespace runtime::controls
