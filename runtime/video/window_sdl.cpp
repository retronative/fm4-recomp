// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// After ForzaRecomp runtime/video/presenter.cpp:110-140 and 455-485 (GPL-3.0-or-later, same owner), re-written;
// the changes are listed in window_sdl.h.
#include "window_sdl.h"
#include "renderer.h"
#include "stops.h"
#include "kernel/input_live.h"
#include "kernel/disc_swap.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>

static_assert(int(StopCode::WindowClosed) == 14, "scripts/gpu_run.sh names exit 14 as the window closed by the user");

namespace runtime::video::window {
namespace {

SDL_Window* sdlWindow = nullptr;
SDL_Gamepad* gamepad = nullptr;  // FM4_INPUT_LIVE: the first gamepad SDL reports

// FM4 Recomp (FM4_INPUT_LIVE, input_live.h): the keyboard and the gamepad now, merged, handed to XamInputGetState.
void PollLiveInput()
{
    using namespace runtime::input;
    if (gamepad == nullptr)
    {
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count))
        {
            if (count > 0 && (gamepad = SDL_OpenGamepad(ids[0])) != nullptr)
                std::fprintf(stderr, "[input] gamepad connected: %s\n", SDL_GetGamepadName(gamepad));
            SDL_free(ids);
        }
    }
    LiveState pad;
    if (gamepad != nullptr && !SDL_GamepadConnected(gamepad))
    {
        SDL_CloseGamepad(gamepad), gamepad = nullptr;
        std::fprintf(stderr, "[input] gamepad disconnected\n");
    }
    if (gamepad != nullptr)
    {
        const auto b = [](SDL_GamepadButton which, uint16_t bit) { return SDL_GetGamepadButton(gamepad, which) ? bit : uint16_t(0); };
        pad.buttons = uint16_t(b(SDL_GAMEPAD_BUTTON_DPAD_UP, button::kDpadUp) | b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, button::kDpadDown) |
                               b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, button::kDpadLeft) | b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, button::kDpadRight) |
                               b(SDL_GAMEPAD_BUTTON_START, button::kStart) | b(SDL_GAMEPAD_BUTTON_BACK, button::kBack) |
                               b(SDL_GAMEPAD_BUTTON_LEFT_STICK, button::kLeftThumb) | b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, button::kRightThumb) |
                               b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, button::kLeftShoulder) | b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, button::kRightShoulder) |
                               b(SDL_GAMEPAD_BUTTON_SOUTH, button::kA) | b(SDL_GAMEPAD_BUTTON_EAST, button::kB) | b(SDL_GAMEPAD_BUTTON_WEST, button::kX) |
                               b(SDL_GAMEPAD_BUTTON_NORTH, button::kY));
        pad.leftTrigger = TriggerFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        pad.rightTrigger = TriggerFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        pad.leftX = ThumbFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), false);
        pad.leftY = ThumbFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY), true);
        pad.rightX = ThumbFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX), false);
        pad.rightY = ThumbFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY), true);
    }
    const bool* k = SDL_GetKeyboardState(nullptr);
    Keys keys;
    keys.up = k[SDL_SCANCODE_UP], keys.down = k[SDL_SCANCODE_DOWN], keys.left = k[SDL_SCANCODE_LEFT], keys.right = k[SDL_SCANCODE_RIGHT];
    keys.w = k[SDL_SCANCODE_W], keys.a = k[SDL_SCANCODE_A], keys.s = k[SDL_SCANCODE_S], keys.d = k[SDL_SCANCODE_D];
    keys.enter = k[SDL_SCANCODE_RETURN], keys.backspace = k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_ESCAPE], keys.space = k[SDL_SCANCODE_SPACE];
    keys.tab = k[SDL_SCANCODE_TAB], keys.x = k[SDL_SCANCODE_X], keys.y = k[SDL_SCANCODE_Y], keys.q = k[SDL_SCANCODE_Q], keys.e = k[SDL_SCANCODE_E];
    keys.c = k[SDL_SCANCODE_C];
    SetLiveState(Merge(pad, FromKeys(keys)));
}
SurfaceRequest request;
constexpr uint64_t kTakeWaitNs = 8'000'000;  // the loop's wait for a handed-over frame between event polls (~half a 60 Hz frame)

VkSurfaceKHR CreateSurface(VkInstance instance)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface))
        Stop(StopCode::Setup, "window: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    return surface;
}

void DestroySurface(VkInstance instance, VkSurfaceKHR surface) { SDL_Vulkan_DestroySurface(instance, surface, nullptr); }

[[noreturn]] void CloseStop(const char* what)
{
    // A normal-context stop: Stop drains the command processor's submissions in flight (bounded; this thread has none
    // of its own: the presenter retired its submission before returning to the loop), then _exit.
    Stop(StopCode::WindowClosed, "the window was closed by the user (%s)", what);
}

const char* EventName(uint32_t type)
{
    return type == SDL_EVENT_QUIT ? "SDL_EVENT_QUIT" : type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ? "SDL_EVENT_WINDOW_CLOSE_REQUESTED" : "event";
}

}  // namespace

const SurfaceRequest* OpenWindow()
{
    // SIGTERM and SIGINT stay the runtime's and the launcher's (scripts/gpu_run.sh: TERM, then KILL of the process group):
    // SDL's handlers would turn them into SDL_EVENT_QUIT, which this loop names as a user close. audio_sdl.cpp sets the
    // same hint, but only in audible mode.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | (runtime::input::LiveEnabled() ? SDL_INIT_GAMEPAD : 0)))  // FM4_INPUT_LIVE: the gamepad subsystem too
        Stop(StopCode::Setup, "window: SDL_Init(VIDEO): %s", SDL_GetError());
    // SDL_WINDOW_VULKAN loads SDL's Vulkan loader (SDL_Vulkan_LoadLibrary(NULL); SDL_VULKAN_LIBRARY, which
    // scripts/gpu_run.sh sets to the loader fm4recomp links, decides which).
    sdlWindow = SDL_CreateWindow("FM4 Recomp", int(kWindowWidth), int(kWindowHeight), SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (sdlWindow == nullptr)
        Stop(StopCode::Setup, "window: SDL_CreateWindow: %s", SDL_GetError());
    // One loader only: SDL_Vulkan_CreateSurface calls vkCreateMetalSurfaceEXT through SDL's vkGetInstanceProcAddr on our
    // instance, which must therefore come from the same loader as every other call (a second loader's dispatch on our
    // instance would be invalid). Fail closed.
    const SDL_FunctionPointer sdlProc = SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (sdlProc != reinterpret_cast<SDL_FunctionPointer>(&vkGetInstanceProcAddr))
        Stop(StopCode::Setup, "window: SDL loaded another Vulkan loader than fm4recomp's (SDL vkGetInstanceProcAddr %p, ours %p): set SDL_VULKAN_LIBRARY "
             "to the linked libvulkan (scripts/gpu_run.sh)", reinterpret_cast<void*>(sdlProc), reinterpret_cast<void*>(&vkGetInstanceProcAddr));
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (names == nullptr)
        Stop(StopCode::Setup, "window: SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
    request.instanceExtensions.assign(names, names + count);
    request.create = CreateSurface;
    request.destroy = DestroySurface;
    const VkExtent2D pixels = WindowPixels();
    std::fprintf(stderr, "[window] opened: %ux%u points, %ux%u pixels; instance extensions from SDL:", kWindowWidth, kWindowHeight, pixels.width, pixels.height);
    for (const char* name : request.instanceExtensions)
        std::fprintf(stderr, " %s", name);
    std::fprintf(stderr, "\n");
    return &request;
}

VkExtent2D WindowPixels()
{
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(sdlWindow, &w, &h))
        Stop(StopCode::Setup, "window: SDL_GetWindowSizeInPixels: %s", SDL_GetError());
    return {uint32_t(w < 0 ? 0 : w), uint32_t(h < 0 ? 0 : h)};
}

void SetWindowTitle(const char* title)
{
    SDL_SetWindowTitle(sdlWindow, title);
}

SDL_Rect WindowFrame()
{
    SDL_Rect r{};
    SDL_GetWindowPosition(sdlWindow, &r.x, &r.y);
    SDL_GetWindowSize(sdlWindow, &r.w, &r.h);
    return r;
}

void RunWindowLoop()
{
    for (;;)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            Dispatch(ClassifyWindowEvent(event.type), EventName(event.type), CloseStop,
                     [] { WindowEvent(EventAction::Resized, WindowPixels()); },
                     [](bool minimized) { WindowEvent(minimized ? EventAction::Minimized : EventAction::Restored, WindowPixels()); });
        if (runtime::input::LiveEnabled())
            PollLiveInput();
        runtime::disc::PumpMainThread();  // FM4 Recomp (Disc 2): a pending swap's dialogs (they may wait for the player; frames are dropped meanwhile)
        PresentHandedOverFrame(kTakeWaitNs);  // bounded: at most kTakeWaitNs without a frame, then the bounded present
    }
}

}  // namespace runtime::video::window
