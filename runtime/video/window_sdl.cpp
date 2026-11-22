// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// After ForzaRecomp runtime/video/presenter.cpp:110-140 and 455-485 (GPL-3.0-or-later, same owner), re-written;
// the changes are listed in window_sdl.h.
#include "window_sdl.h"
#include "virtual_pad_sdl.h"
#include "renderer.h"
#include "stops.h"
#include "kernel/input_live.h"
#include "kernel/key_bindings.h"
#include "kernel/disc_swap.h"
#include "kernel/controls_screen.h"
#include "kernel/controls_menu.h"
#include "kernel/input_script.h"
#include "kernel/rumble.h"
#include "kernel/graphics_menu.h"
#include "kernel/extended_view.h"
#include <atomic>
#include <chrono>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <cstring>
#include <objc/message.h>
#include <objc/runtime.h>

static_assert(int(StopCode::WindowClosed) == 14, "scripts/gpu_run.sh names exit 14 as the window closed by the user");

namespace runtime::video::window {
namespace {

SDL_Window* sdlWindow = nullptr;
SDL_Gamepad* gamepad = nullptr;  // FM4_INPUT_LIVE: the first gamepad SDL reports

// FM4 Recomp (FM4_INPUT_LIVE, input_live.h): the keyboard and the gamepad now, merged, handed to XamInputGetState.
std::atomic<bool> pausePressed{false};  // Command-comma in a race (the event loop) -> one START press (PollLiveInput)

// FM4 Recomp (kernel/rumble.h; ReCarbon's SDL approach): the game's motor speeds to the gamepad. Main thread only.
runtime::input::RumblePlanner rumble;
bool rumbleCapable = false;  // the open gamepad says it can rumble and has not refused
uint64_t rumbleSends = 0;

// The motors off now (the pad is about to be closed, or the process to end). A pad that is gone ignores it.
void StopRumble()
{
    if (gamepad != nullptr && rumbleCapable && rumble.Running())
        SDL_RumbleGamepad(gamepad, 0, 0, 0);
    rumble.Refused();  // nothing running from here
}

void UpdateRumble()
{
    // Not while the window is in the background or the game waits behind the Settings panel: the motors stop, and the game's
    // request is sent again when the game is back in front.
    const bool active = (SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_INPUT_FOCUS) != 0 && !runtime::controls::InputBlocked();
    const runtime::input::RumbleCommand c = rumble.Step(runtime::input::GameRumble(), gamepad != nullptr && rumbleCapable, active, SDL_GetTicks());
    if (!c.send)
        return;
    if (!SDL_RumbleGamepad(gamepad, c.low, c.high, c.durationMs))
    {
        std::fprintf(stderr, "[input] rumble: the gamepad refused it (%s): no rumble on this controller\n", SDL_GetError());
        rumbleCapable = false, rumble.Refused();
    }
    else if (rumbleSends++ < 8)
        std::fprintf(stderr, "[input] rumble: low %u, high %u to the gamepad%s\n", unsigned(c.low), unsigned(c.high), rumbleSends == 8 ? " (no further lines)" : "");
}

void PollLiveInput()
{
    using namespace runtime::input;
    if (gamepad == nullptr)
    {
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count))
        {
            if (count > 0 && (gamepad = SDL_OpenGamepad(ids[0])) != nullptr)
            {
                // What it can do (as ReCarbon's hid/input.cpp): a pad without motors is never sent a rumble.
                rumbleCapable = SDL_GetBooleanProperty(SDL_GetGamepadProperties(gamepad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
                std::fprintf(stderr, "[input] gamepad connected: %s (rumble %s)\n", SDL_GetGamepadName(gamepad), rumbleCapable ? "yes" : "no: not supported by this controller");
            }
            SDL_free(ids);
        }
    }
    LiveState pad;
    if (gamepad != nullptr && !SDL_GamepadConnected(gamepad))
    {
        StopRumble();  // best effort; the planner starts the next pad still
        SDL_CloseGamepad(gamepad), gamepad = nullptr, rumbleCapable = false;
        std::fprintf(stderr, "[input] gamepad disconnected\n");
    }
    if (gamepad != nullptr)
    {
        const auto b = [](SDL_GamepadButton which) { return SDL_GetGamepadButton(gamepad, which); };
        PadButtons p;
        p.south = b(SDL_GAMEPAD_BUTTON_SOUTH), p.east = b(SDL_GAMEPAD_BUTTON_EAST), p.west = b(SDL_GAMEPAD_BUTTON_WEST),
        p.north = b(SDL_GAMEPAD_BUTTON_NORTH), p.back = b(SDL_GAMEPAD_BUTTON_BACK), p.start = b(SDL_GAMEPAD_BUTTON_START),
        p.leftStick = b(SDL_GAMEPAD_BUTTON_LEFT_STICK), p.rightStick = b(SDL_GAMEPAD_BUTTON_RIGHT_STICK),
        p.leftShoulder = b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), p.rightShoulder = b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
        p.dpadUp = b(SDL_GAMEPAD_BUTTON_DPAD_UP), p.dpadDown = b(SDL_GAMEPAD_BUTTON_DPAD_DOWN), p.dpadLeft = b(SDL_GAMEPAD_BUTTON_DPAD_LEFT),
        p.dpadRight = b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        pad.buttons = ButtonsFromPad(p);
        pad.leftTrigger = TriggerFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        pad.rightTrigger = TriggerFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        pad.leftX = ThumbFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), false);
        pad.leftY = ThumbFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY), true);
        pad.rightX = ThumbFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX), false);
        pad.rightY = ThumbFromSdl(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY), true);
    }
    // FM4 Recomp (kernel/key_bindings.h): the keyboard through its bindings (the defaults, --keybindings' file, or the file the
    // in-game Keyboard Controls screen wrote, reloaded when it closed).
    int keyCount = 0;
    const bool* k = SDL_GetKeyboardState(&keyCount);
    const Keys keys = KeysFromState(*ActiveKeyBindings(), k, size_t(keyCount));
    // FM4 Recomp (kernel/controls_screen.h, controls_menu.h): an idle pad while the host screen is up or the in-game KEYBOARD
    // CONTROLS submenu waits for a key or asks a question (the pad's A and B answer it); after it, held buttons stay hidden until
    // released. Each change the submenu makes sends the game one X press (~100 ms down, then up) so its screen redraws on the game's
    // thread (runtime/game/keyboard_screen.cpp).
    runtime::controls::Session& menu = runtime::controls::KeyboardMenu();
    bool confirmHeld = false;  // a key bound to A is down: the submenu's key capture waits for its release
    for (const uint16_t sc : ActiveKeyBindings()->keys[8])
        confirmHeld = confirmHeld || (sc < size_t(keyCount) && k[sc]);
    menu.Pad(pad.buttons, confirmHeld);
    static InputGate gate;
    LiveState state = gate.Filter(Merge(pad, FromKeys(keys)), runtime::controls::InputBlocked() || menu.OwnsInput());
    static uint64_t pulseUntil = 0, pauseUntil = 0;
    const uint64_t now = SDL_GetTicksNS();
    if (menu.TakePulse(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())))
        pulseUntil = now + 100'000'000;
    if (pausePressed.exchange(false))
        pauseUntil = now + 100'000'000;
    if (now < pulseUntil)
        state = LiveState{}, state.buttons = button::kX;
    else if (now < pauseUntil)
        state.buttons = uint16_t(state.buttons | button::kStart);  // Command-comma in a race: START, FM4's own Pause menu
    SetLiveState(state);
    UpdateRumble();
}

// FM4 Recomp (kernel/controls_screen.h): Command-comma, the Mac's Settings shortcut (ReCarbon's too, runtime/ui/ui.cpp:665-673 at
// 66401ea). Command is never a game key (key_bindings.h), so the shortcut cannot shadow a binding.
bool IsControlsShortcut(const SDL_Event& e)
{
    return e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_COMMA && (e.key.mod & SDL_KMOD_GUI) != 0;
}

// FM4 Recomp: Command-Q quits as the window's close button does (stop 14, a normal end). The game has no menu bar of its own when
// the app started it (SDL_MAC_BACKGROUND_APP, below), so the shortcut is the event loop's.
bool IsQuitShortcut(const SDL_Event& e)
{
    return e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_Q && (e.key.mod & SDL_KMOD_GUI) != 0;
}

// FM4 Recomp: started by FM4 Recomp.app (it sets SDL_MAC_BACKGROUND_APP=1 for play.sh), the game has no Dock icon of its own (the
// app's branded icon stands for it; a Dock click there brings the game forward, app/setup/Main.swift). SDL leaves the activation
// policy unset and does not activate the app in that mode, so: accessory (windows, focus, no Dock icon), then to the front.
void BecomeAccessoryApp()
{
    if (!SDL_GetHintBoolean(SDL_HINT_MAC_BACKGROUND_APP, false))
        return;
    using Send = id (*)(id, SEL);
    const id app = reinterpret_cast<Send>(objc_msgSend)(reinterpret_cast<id>(objc_getClass("NSApplication")), sel_registerName("sharedApplication"));
    reinterpret_cast<bool (*)(id, SEL, long)>(objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 1L);  // NSApplicationActivationPolicyAccessory
    reinterpret_cast<void (*)(id, SEL, bool)>(objc_msgSend)(app, sel_registerName("activateIgnoringOtherApps:"), true);
    std::fprintf(stderr, "[window] started by FM4 Recomp.app: no Dock icon of its own (accessory)\n");
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
    StopRumble();  // FM4 Recomp: the motors do not outlive the game (any other end: the 1 s hold runs out, kernel/rumble.h)
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
    if (runtime::input::LiveEnabled())
        virtual_pad::Start();  // test only: FM4_TEST_VIRTUAL_PAD
    // FM4 Recomp (kernel/key_bindings.h): the binding table names each scancode as the linked SDL does, both ways. Fail closed.
    if (runtime::input::LiveEnabled())
        for (size_t i = 0; i < runtime::input::kKeyNameCount; i++)
        {
            const runtime::input::KeyName& key = runtime::input::kKeyNames[i];
            const char* sdlName = SDL_GetScancodeName(SDL_Scancode(key.scancode));
            if (SDL_GetScancodeFromName(key.name) != SDL_Scancode(key.scancode) || std::strcmp(sdlName, key.name) != 0)
                Stop(StopCode::Setup, "window: the key binding table names scancode %u '%s', SDL names it '%s' (kernel/key_bindings.cpp kKeyNames)",
                     unsigned(key.scancode), key.name, sdlName);
        }
    // SDL_WINDOW_VULKAN loads SDL's Vulkan loader (SDL_Vulkan_LoadLibrary(NULL); SDL_VULKAN_LIBRARY, which
    // scripts/gpu_run.sh sets to the loader fm4recomp links, decides which).
    sdlWindow = SDL_CreateWindow("FM4 Recomp", int(kWindowWidth), int(kWindowHeight), SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (sdlWindow == nullptr)
        Stop(StopCode::Setup, "window: SDL_CreateWindow: %s", SDL_GetError());
    BecomeAccessoryApp();
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

float HdrHeadroom(float* potential)
{
    if (potential != nullptr)
        *potential = 1.0f;
    void* window = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (window == nullptr)
        return 1.0f;
    const id screen = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(reinterpret_cast<id>(window), sel_registerName("screen"));
    if (screen == nullptr)  // the window is on no screen (off every display)
        return 1.0f;
    using Value = double (*)(id, SEL);  // CGFloat
    if (potential != nullptr)
        *potential = float(reinterpret_cast<Value>(objc_msgSend)(screen, sel_registerName("maximumPotentialExtendedDynamicRangeColorComponentValue")));
    return float(reinterpret_cast<Value>(objc_msgSend)(screen, sel_registerName("maximumExtendedDynamicRangeColorComponentValue")));
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
        {
            if (IsControlsShortcut(event) && runtime::input::LiveEnabled() && runtime::input::InRaceScene())
            {
                // FM4 Recomp: Command-comma during a race opens FM4's own Pause menu (its KEYBOARD CONTROLS row is there), as START on
                // a controller does, so no controller is needed.
                if (!runtime::controls::KeyboardMenu().IsOpen())
                {
                    pausePressed = true;
                    std::fprintf(stderr, "[input] Command-comma: START (the Pause menu)\n");
                }
            }
            // Anywhere else (the menus): the Settings panel (--controls-panel: Graphics, and the keys), over the paused game.
            else if (runtime::controls::Screen* controls = runtime::controls::Service(); controls != nullptr && IsControlsShortcut(event))
                controls->RequestOpen("Command-comma");
            if (IsQuitShortcut(event))
                CloseStop("Command-Q");
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)  // the in-game submenu's key, when it waits for one
                runtime::controls::KeyboardMenu().KeyDown(int(event.key.scancode), *runtime::input::ActiveKeyBindings());
            Dispatch(ClassifyWindowEvent(event.type), EventName(event.type), CloseStop,
                     [] { WindowEvent(EventAction::Resized, WindowPixels()); },
                     [](bool minimized) { WindowEvent(minimized ? EventAction::Minimized : EventAction::Restored, WindowPixels()); });
        }
        // FM4 Recomp (kernel/controls_screen.h): the screen ended: bindings reloaded, the game resumed. Keys that went down or up
        // while the panel had the focus read as up from here.
        if (runtime::controls::Screen* controls = runtime::controls::Service(); controls != nullptr && controls->Pump())
        {
            SDL_ResetKeyboard();
            if (runtime::settings::Available())  // FM4 Recomp (FM4_HDR): the panel's HDR choice, saved in graphics.conf, applies now
            {
                std::string why;
                const runtime::settings::Graphics saved = runtime::settings::Load(runtime::settings::GraphicsPath(), why);
                // FM4 Recomp (live resolution): the panel's resolution and edge smoothing too (each false when this run cannot switch it)
                const bool resolution = RequestInternalRes(runtime::settings::kResolutions[saved.resolution].file);
                const bool smoothing = SetPostAaLive(runtime::settings::kSmoothings[saved.smoothing].file);
                std::fprintf(stderr, "[graphics] Settings closed: RESOLUTION %s (%s), EDGE SMOOTHING %s (%s)\n", runtime::settings::kResolutions[saved.resolution].label,
                             resolution ? "asked for now" : "at the next start", runtime::settings::kSmoothings[saved.smoothing].label, smoothing ? "now" : "at the next start");
                SetHdrPreference(saved.hdr != 0);
                if (runtime::settings::ExtendedOffered())
                    runtime::view::Global().SetPreference(saved.extended != 0);  // Extended View: from the next frame
                const HdrOutput now = HdrOutputNow();
                runtime::settings::PublishHdrStatus({true, now.active, now.reason});
                std::fprintf(stderr, "[graphics] Settings closed: EXTENDED VIEW %s, HDR %s; %s\n", saved.extended != 0 ? "ON" : "OFF", saved.hdr != 0 ? "AUTO" : "OFF",
                             runtime::settings::HdrStatusLine(runtime::settings::GetHdrStatus()).c_str());
            }
        }
        if (runtime::input::LiveEnabled())
        {
            virtual_pad::Poll();
            PollLiveInput();
        }
        {  // FM4 Recomp (FM4_HDR): about once a second, whether the display the window is on now can show HDR (the window may have
           // moved to another display), and what the game shows as a result (the GRAPHICS menu's line and the Settings panel's file)
            static uint64_t next = 0;
            static int said = -1;
            if (const uint64_t now = SDL_GetTicks(); now >= next)
            {
                next = now + 1000;
                const VkExtent2D pixels = WindowPixels();  // Extended View: the window's shape now (a resize, another display)
                runtime::view::Global().SetWindow(pixels.width, pixels.height);
                float potential = 1.0f;
                HdrHeadroom(&potential);
                const bool supported = potential > 1.01f;
                SetHdrDisplay(supported);
                if (said != int(supported))
                    said = int(supported), std::fprintf(stderr, "[window] the display %s HDR (at most %.2fx standard white)\n",
                                                        supported ? "supports" : "does not support", potential);
                const HdrOutput output = HdrOutputNow();
                runtime::settings::PublishHdrStatus({true, output.active, output.reason});
            }
        }
        runtime::disc::PumpMainThread();  // FM4 Recomp (Disc 2): a pending swap's dialogs (they may wait for the player; frames are dropped meanwhile)
        PresentHandedOverFrame(kTakeWaitNs);  // bounded: at most kTakeWaitNs without a frame, then the bounded present
    }
}

}  // namespace runtime::video::window
