// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XAM input imports over the scripted gamepad (input_script.h; FM4_INPUT_SCRIPT, loaded by main.cpp). Semantics:
// Xenia xenia-canary 67f16c4e315f src/xenia/kernel/xam/xam_input.cc, hid/input.h
// (layouts), hid/input_system.cc and hid/sdl/sdl_input_driver.cc (what a connected pad reports), cited per import.
// Without a script no pad is connected: XamInputGetState answers as before (the state zeroed, ERROR_DEVICE_NOT_CONNECTED)
// and the other input imports keep their strict stub (PpcImportStop, exit 3, the same line as the generated stub).
// With a script, user 0 has one wired gamepad; users 1-3 stay not connected. FM4 imports XamInputGetCapabilities,
// XamInputGetCapabilitiesEx, XamInputGetState, XamInputRawState and XamInputSetState (the local capture link (not distributed))
// and resolves XamInputGetUserVibrationLevel and the XInputdFF* ordinals at run time (exports.cpp): RawState (no Xenia
// implementation) and the ordinal-resolved ones stay strict stops.
#include "imports.h"
#include "graphics.h"
#include "input_script.h"
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace {

constexpr uint32_t kErrorNotSupported = 0x32, kErrorBadArguments = 0xA0, kErrorDeviceNotConnected = 0x48F;  // Xenia xbox.h:94,98,103
constexpr uint32_t kXInputFlagGamepad = 0x01, kXInputFlagAnyDevice = 0xFF, kXInputFlagAnyUser = 1u << 30;   // xam_input.cc:28-32

std::mutex lock;
std::unique_ptr<runtime::input::Poller> poller;  // null: no script, no pad
std::string scriptPath;
long logged = -1;       // the last event reported in the log
uint64_t rumbles = 0;

// Xenia xam_input.cc:107-110 (GetState), :67-70 (GetCapabilitiesEx), :156-159 (GetKeystroke): a query that names
// device types (low byte) without the gamepad bit is for another device: not connected.
bool OtherDevice(uint32_t flags) { return (flags & kXInputFlagAnyDevice) != 0 && (flags & kXInputFlagGamepad) == 0; }

}  // namespace

namespace runtime::input {

void LoadScript(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        Stop(StopCode::Setup, "FM4_INPUT_SCRIPT %s: cannot be read", path);
    std::stringstream text;
    text << file.rdbuf();
    Script script;
    std::string error;
    if (!ParseScript(text.str(), script, error))
        Stop(StopCode::Setup, "FM4_INPUT_SCRIPT %s %s", path, error.c_str());
    std::lock_guard guard(lock);
    const size_t events = script.events.size();
    poller = std::make_unique<Poller>(std::move(script));
    scriptPath = path;
    logged = -1;
    rumbles = 0;
    std::fprintf(stderr, "[input] script %s: %zu events\n", path, events);
}

void NotifyRaceStart(uint64_t vblank)
{
    std::lock_guard guard(lock);
    if (poller && poller->RaceStarted(vblank))
        std::fprintf(stderr, "[input] race start at vblank %llu: the script's @race section (%zu events) runs from here\n",
                     (unsigned long long)vblank, poller->script().race.size());
}

bool ScriptLoaded()
{
    std::lock_guard guard(lock);
    return poller != nullptr;
}

}  // namespace runtime::input

GUEST_IMPORT(XamInputGetState, "REFERENCE_DERIVED: Xenia xam_input.cc:94-121; a scripted pad on user 0 (input_script.h), else no controller")
{
    // (user index, flags, X_INPUT_STATE*): the 16-byte state (packet number u32, buttons u16, triggers u8 u8, four s16
    // thumbs; hid/input.h:57-72) is zeroed if given; an index of 4 or more or a query for another device type is
    // ERROR_DEVICE_NOT_CONNECTED (0x48F), as is any user without a pad: every user without a script, users 1-3 with one.
    // (Xenia pins an index whose low byte is FF to user 0 after its >= 4 check, so FF is never connected there either:
    // :111-116.) User 0 with a script: the active event's buttons and triggers and the packet number (input_script.h
    // Poller), 0. A null state pointer with a pad stops (7): Xenia's driver would copy through it (:105 calls it a
    // query), so what the console answers is UNKNOWN and nothing is invented.
    const uint32_t index = ctx.r3.u32, flags = ctx.r4.u32, state = ctx.r5.u32;
    if (state)
        std::memset(base + state, 0, 16);
    std::lock_guard guard(lock);
    if (poller == nullptr || index != 0 || OtherDevice(flags))
        return (void)(ctx.r3.u64 = kErrorDeviceNotConnected);
    if (state == 0)
        Stop(StopCode::Unimplemented, "XamInputGetState(user 0, flags %u) without a state pointer while a pad is connected is not modelled", flags);
    {
        const runtime::input::Report report = poller->Poll(runtime::gpu::VblankCount());
        // FM4 Recomp: indices past the first list are the @race list's (input_script.h Poller::Poll); its "nothing held yet" state
        // (index events + race) is logged by NotifyRaceStart, and the jump to the race skips the first list's unreached events.
        const auto& script = poller->script();
        if (report.event >= long(script.events.size()) && logged < long(script.events.size()) - 1)
            logged = long(script.events.size()) - 1;
        for (; logged < report.event; logged++)
        {
            const size_t next = size_t(logged + 1), first = script.events.size();
            if (next >= first + script.race.size())
                continue;
            const runtime::input::Event& e = next < first ? script.events[next] : script.race[next - first];
            const std::string held = runtime::input::Describe(e.buttons, e.leftTrigger, e.rightTrigger);
            std::fprintf(stderr, "[input] event %ld at %svblank %llu: %s\n", logged + 1, next < first ? "" : "race ", (unsigned long long)e.vblank,
                         held.empty() ? "released" : held.c_str());
        }
        runtime::Store32(base, state, report.packet);
        runtime::Store16(base, state + 4, report.buttons);
        base[state + 6] = report.leftTrigger;
        base[state + 7] = report.rightTrigger;
    }
    ctx.r3.u64 = 0;
}

namespace {

// XamInputGetCapabilitiesEx (Xenia xam_input.cc:46-82; the non-Ex form is Ex(1, ...) :85-91): unk above 1 is
// ERROR_NOT_SUPPORTED (0x32); no caps pointer ERROR_BAD_ARGUMENTS (0xA0); the 20-byte X_INPUT_CAPABILITIES (type u8,
// sub-type u8, flags u16, gamepad 12, vibration 4; hid/input.h:80-88) is zeroed; another device type is
// ERROR_DEVICE_NOT_CONNECTED (0x48F); an index whose low byte is FF, or the any-user flag (1 << 30), means user 0. Then
// the pad: only user 0 has one (users 1-3: 0x48F; Xenia's SDL driver answers an index of 4 or more with
// ERROR_BAD_ARGUMENTS, which its input system turns into ERROR_EMPTY (input_system.cc:43-60): not mirrored, 0x48F).
// The capabilities are those Xenia's SDL driver reports for any pad (sdl_input_driver.cc:685-699): type 1
// (XINPUT_DEVTYPE_GAMEPAD), sub-type 1 (XINPUT_DEVSUBTYPE_GAMEPAD), flags 0 (wired, every navigation button bound),
// buttons F3FF (every bit but the guide button, which the script cannot name; Xenia adds 0400 because its guide_button
// cvar defaults to true, hid_flags.cc:12), both triggers FF, every thumb FFFF, both motors FFFF.
void Capabilities(PPCContext& ctx, uint8_t* base, uint32_t unk, uint32_t index, uint32_t flags, uint32_t caps)
{
    if (unk > 1)
        return (void)(ctx.r3.u64 = kErrorNotSupported);
    if (caps == 0)
        return (void)(ctx.r3.u64 = kErrorBadArguments);
    std::memset(base + caps, 0, 20);
    if (OtherDevice(flags))
        return (void)(ctx.r3.u64 = kErrorDeviceNotConnected);
    if ((index & 0xFF) == 0xFF || (flags & kXInputFlagAnyUser))
        index = 0;
    if (index != 0)
        return (void)(ctx.r3.u64 = kErrorDeviceNotConnected);
    base[caps] = 1, base[caps + 1] = 1;
    runtime::Store16(base, caps + 4, 0xF3FF);
    std::memset(base + caps + 6, 0xFF, 14);  // triggers, thumbs, motors
    ctx.r3.u64 = 0;
}

}  // namespace

GUEST_IMPORT(XamInputGetCapabilities, "REFERENCE_DERIVED: Xenia xam_input.cc:85-91 (Ex with 1), sdl_input_driver.cc:685-699 (a pad's capabilities); strict stub without a script")
{
    // (user index, flags, X_INPUT_CAPABILITIES*): see Capabilities. Without a script this is the generated strict stub.
    std::lock_guard guard(lock);
    if (poller == nullptr)
        PpcImportStop("XamInputGetCapabilities", ctx);
    Capabilities(ctx, base, 1, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}

GUEST_IMPORT(XamInputGetCapabilitiesEx, "REFERENCE_DERIVED: Xenia xam_input.cc:46-82, sdl_input_driver.cc:685-699 (a pad's capabilities); strict stub without a script")
{
    // (unknown, user index, flags, X_INPUT_CAPABILITIES*): see Capabilities. Without a script this is the generated strict stub.
    std::lock_guard guard(lock);
    if (poller == nullptr)
        PpcImportStop("XamInputGetCapabilitiesEx", ctx);
    Capabilities(ctx, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

GUEST_IMPORT(XamInputSetState, "REFERENCE_DERIVED: Xenia xam_input.cc:126-146 (arguments), sdl_input_driver.cc:208-234 (a pad accepts the rumble); strict stub without a script")
{
    // (user index, flags (unused in Xenia), X_INPUT_VIBRATION*): an index of 4 or more is X_E_DEVICE_NOT_CONNECTED (the
    // HRESULT, unlike XamInputGetState's Win32 code); no vibration pointer ERROR_BAD_ARGUMENTS (0xA0); users 1-3 have no
    // pad (0x48F); user 0's scripted pad accepts the two motor speeds (u16 u16, hid/input.h:74-78) and answers 0, as
    // Xenia's SDL driver does when its rumble call succeeds. The scripted pad has no motors: the speeds are discarded,
    // which the log says once (never silently). Without a script this is the generated strict stub.
    const uint32_t index = ctx.r3.u32, vibration = ctx.r5.u32;
    std::lock_guard guard(lock);
    if (poller == nullptr)
        PpcImportStop("XamInputSetState", ctx);
    if (index >= 4)
        return (void)(ctx.r3.u64 = (1u << 31) | (7u << 16) | kErrorDeviceNotConnected);
    if (vibration == 0)
        return (void)(ctx.r3.u64 = kErrorBadArguments);
    if (index != 0)
        return (void)(ctx.r3.u64 = kErrorDeviceNotConnected);
    if (rumbles++ == 0)
        std::fprintf(stderr, "[input] XamInputSetState: rumble (left %04X, right %04X) accepted and discarded: the scripted pad has no motors (reported once)\n",
                     runtime::Load32(base, vibration) >> 16, runtime::Load32(base, vibration) & 0xFFFF);
    ctx.r3.u64 = 0;
}
