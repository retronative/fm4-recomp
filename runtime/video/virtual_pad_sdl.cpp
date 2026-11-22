// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (test only, opt-in): FM4_TEST_VIRTUAL_PAD=<a named pipe> attaches a virtual SDL gamepad (SDL_AttachVirtualJoystick,
// standard gamepad layout) and presses its buttons from lines written to the pipe ("down A", "up A"; A B X Y START BACK UP DOWN LEFT
// RIGHT LB RB), so a scripted run drives the game through the same SDL gamepad path as a real controller (window_sdl.cpp
// PollLiveInput) on a Mac with none connected. Off (nothing attached) without the variable.
#include "virtual_pad_sdl.h"
#include <SDL3/SDL.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace runtime::video::virtual_pad {
namespace {

int fd = -1;
SDL_Joystick* joystick = nullptr;
std::string pending;

// What the pad receives (as ReCarbon's virtual pad, runtime/hid/input.cpp): one line per SDL_RumbleGamepad call that reaches it.
bool SDLCALL Rumble(void*, Uint16 low, Uint16 high)
{
    std::fprintf(stderr, "[test pad] rumble low %u, high %u\n", unsigned(low), unsigned(high));
    return true;
}

int ButtonOf(const std::string& name)
{
    static const struct { const char* name; SDL_GamepadButton button; } kButtons[] = {
        {"A", SDL_GAMEPAD_BUTTON_SOUTH},        {"B", SDL_GAMEPAD_BUTTON_EAST},          {"X", SDL_GAMEPAD_BUTTON_WEST},
        {"Y", SDL_GAMEPAD_BUTTON_NORTH},        {"START", SDL_GAMEPAD_BUTTON_START},     {"BACK", SDL_GAMEPAD_BUTTON_BACK},
        {"UP", SDL_GAMEPAD_BUTTON_DPAD_UP},     {"DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN},  {"LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {"RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}};
    for (const auto& b : kButtons)
        if (name == b.name)
            return b.button;
    return -1;
}

}  // namespace

void Start()
{
    const char* path = std::getenv("FM4_TEST_VIRTUAL_PAD");
    if (path == nullptr || *path == 0)
        return;
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
    {
        std::fprintf(stderr, "[test pad] FM4_TEST_VIRTUAL_PAD %s: %s (no virtual gamepad)\n", path, std::strerror(errno));
        return;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "FM4 Recomp test pad";
    desc.Rumble = Rumble;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    joystick = id != 0 ? SDL_OpenJoystick(id) : nullptr;
    std::fprintf(stderr, "[test pad] virtual gamepad %s (pipe %s)\n", joystick != nullptr ? "attached" : SDL_GetError(), path);
}

void Poll()
{
    if (fd < 0 || joystick == nullptr)
        return;
    char buffer[256];
    for (ssize_t n; (n = read(fd, buffer, sizeof buffer)) > 0;)
        pending.append(buffer, size_t(n));
    for (size_t end; (end = pending.find('\n')) != std::string::npos;)
    {
        const std::string line = pending.substr(0, end);
        pending.erase(0, end + 1);
        const size_t space = line.find(' ');
        const std::string verb = line.substr(0, space), name = space == std::string::npos ? "" : line.substr(space + 1);
        const int button = ButtonOf(name);
        if ((verb != "down" && verb != "up") || button < 0)
        {
            std::fprintf(stderr, "[test pad] ignored: '%s'\n", line.c_str());
            continue;
        }
        SDL_SetJoystickVirtualButton(joystick, button, verb == "down");
        std::fprintf(stderr, "[test pad] %s %s\n", verb.c_str(), name.c_str());
    }
}

}  // namespace runtime::video::virtual_pad
