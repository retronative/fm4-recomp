// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The window of fm4recomp --renderer window (unit 4b): SDL3 on the main
// thread, which macOS requires for windows and events (AppKit), while the guest and the command processor run on their
// own threads. The main thread opens the window before the renderer starts (the surface is created inside
// CreateVulkanDevice, called from renderer Initialize on the main thread), then, once the guest thread runs, loops:
// events, then the frame the command processor handed over (renderer.h PresentHandedOverFrame). It never returns: the
// run ends with a stop (closing the window is StopCode::WindowClosed, 14; the deadline thread's stop and every other stop
// end the process from their own threads with _exit, so nothing waits for this loop).
// After ForzaRecomp runtime/video/presenter.cpp:110-140 and 455-485 (SDL_Init, the window, the instance
// extensions, the event loop; GPL-3.0-or-later, same owner), re-written. FM4 Recomp changes: SDL's signal handlers off
// (SIGTERM stays the launcher's, never an SDL_EVENT_QUIT); the Vulkan loader SDL uses must be the one fm4recomp links
// (checked); a close request is a named stop; presentation stays bounded (window_policy.h).
#pragma once
#include "vulkan_device.h"
#include "window_policy.h"
#include <SDL3/SDL_events.h>

namespace runtime::video::window {

// SDL's event types -> what the loop does (tested against SDL's constants in tests/runtime/test_window.cpp).
inline EventAction ClassifyWindowEvent(uint32_t type)
{
    switch (type)
    {
    case SDL_EVENT_QUIT:  // the last window closed, or Cmd-Q (SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE default)
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return EventAction::Close;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: return EventAction::Resized;
    case SDL_EVENT_WINDOW_MINIMIZED: return EventAction::Minimized;
    case SDL_EVENT_WINDOW_RESTORED: return EventAction::Restored;
    default: return EventAction::None;
    }
}

// One event's effect. `close` must not return (window_sdl.cpp: Stop(StopCode::WindowClosed)); the others update the
// presenter's SwapchainPlanner on this (the main) thread.
template <typename Close, typename Resized, typename Minimized>
void Dispatch(EventAction action, const char* what, Close close, Resized resized, Minimized minimized)
{
    switch (action)
    {
    case EventAction::Close: close(what); break;
    case EventAction::Resized: resized(); break;
    case EventAction::Minimized: minimized(true); break;
    case EventAction::Restored: minimized(false); break;
    case EventAction::None: break;
    }
}

// Main thread, under the deadline, before renderer Initialize: SDL video (signal handlers off), a 1280x720 resizable
// Vulkan window, the loader identity check, the instance extensions. Stops (9) on any failure. The request lives for
// the run.
const SurfaceRequest* OpenWindow();
// The window's size in pixels now (SDL_GetWindowSizeInPixels; main thread).
VkExtent2D WindowPixels();
// Main thread, after the guest thread started. Never returns. FM4 Recomp (Disc 2): each turn also pumps a pending disc
// swap's dialogs (kernel/disc_swap.h PumpMainThread).
[[noreturn]] void RunWindowLoop();
// Main thread: the window's title.
void SetWindowTitle(const char* title);
// Main thread: the window's frame in points, top-left origin on the main display (the Disc setup panel opens over it).
SDL_Rect WindowFrame();

}  // namespace runtime::video::window
