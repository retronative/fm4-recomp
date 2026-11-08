// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// After ForzaRecomp runtime/video/presenter.cpp:110-140 and 455-485 (GPL-3.0-or-later, same owner), re-written;
// the changes are listed in window_sdl.h.
#include "window_sdl.h"
#include "renderer.h"
#include "stops.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>

static_assert(int(StopCode::WindowClosed) == 14, "scripts/gpu_run.sh names exit 14 as the window closed by the user");

namespace runtime::video::window {
namespace {

SDL_Window* sdlWindow = nullptr;
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
    if (!SDL_Init(SDL_INIT_VIDEO))
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

void RunWindowLoop()
{
    for (;;)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            Dispatch(ClassifyWindowEvent(event.type), EventName(event.type), CloseStop,
                     [] { WindowEvent(EventAction::Resized, WindowPixels()); },
                     [](bool minimized) { WindowEvent(minimized ? EventAction::Minimized : EventAction::Restored, WindowPixels()); });
        PresentHandedOverFrame(kTakeWaitNs);  // bounded: at most kTakeWaitNs without a frame, then the bounded present
    }
}

}  // namespace runtime::video::window
