// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: the in-game KEYBOARD CONTROLS submenu, shown in FM4's own option screen (the one its Pause menu opens for HUD:
// ForzaUI::CHudOptions, scene 300_I_OPT_ASSISTS_DIFFICULTY), so it looks and navigates like the game's Audio and HUD menus and Back
// returns to the menu that opened it (the game's screen stack). The logic is kernel/controls_menu.h (the launcher's rules); this
// file is its guest side, through title hooks (config/title_hooks.toml, kind "delegate"; FM4's addresses and their evidence are
// there, named in title_symbols.h):
//   - OpenKeyboardControls: the reusable entry handler. Any menu that opens the option screens (Pause today; Pre-Race and the
//     split-screen Pause have the same call) asks the screen manager for the HUD screen, as for HUD, with the submenu pending.
//   - the option screen's slot 3 (binds and builds its rows): the game's own rows replaced by one row per action (its key as the
//     value: a CDynamicListDataSource holding every key name and "PRESS A KEY", so a change is "select value N + redraw", the game's
//     own reset path) and RESET TO DEFAULTS; slot 5 (title); slot 0 (list events: focus kept, value changes refused);
//     slot 9 (input, ReCarbon's keys: A sets a key, Y clears it, X restores its default, B cancels a wait or a question, else Back
//     as the game's; the window loop's redraw pulse is an X while the submenu owns the input); slot 4 (closing).
// While the submenu waits for a key or asks a question the game's pad is idle (window_sdl.cpp): the window loop gives the next key
// to the session and sends one X press so the screen redraws on the game's thread.
// Technique: SpeedBreaker runtime/game/quick_race_cops.cpp (fe04285, GPL-3.0-or-later): game rows made with the game's own calls.
//
// FM4 Recomp: the GRAPHICS submenu (the Pause menu's GRAPHICS row; kernel/graphics_menu.h is its logic and its file) is shown the
// same way, in the same option screen: OpenGraphics; two rows whose values are the game's own spinners (left and right change one;
// the list's "a value changed" event reads the rows' options, title_symbols.h kItemValue, and saves at once), X restores the focused
// row's default, B is the game's Back. It never owns the input. One submenu at a time: `shown` says which one `screen` is.
#include "title_symbols.h"
#include "kernel/controls_menu.h"
#include "kernel/graphics_menu.h"
#include "kernel/input_live.h"
#include "kernel/key_bindings.h"
#include "ppc_recomp_shared.h"
#include "kernel/extended_view.h"  // FM4 Recomp: GRAPHICS switches Extended View at once
#include "video/renderer.h"  // FM4 Recomp (FM4_GRAPHICS_LIVE): GRAPHICS applies edge smoothing at once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace fm4::title {
namespace {

using namespace fm4::sym;
namespace controls = runtime::controls;
namespace settings = runtime::settings;

enum class Submenu : uint8_t { None, Keyboard, Graphics };
std::atomic<Submenu> pending{Submenu::None};  // the next option screen built is this submenu
std::atomic<uint32_t> screen{0};    // the option screen showing it, 0 if none
Submenu shown = Submenu::None;      // game thread only: the submenu `screen` shows
std::optional<settings::Menu> graphics;  // game thread only: GRAPHICS while it is shown
std::atomic<bool> freeing{false};   // our own call of the teardown (old rows), not the screen closing
uint32_t focusRow = 0;              // game thread only

std::string BindingsPath()
{
    const char* home = std::getenv("FM4_RECOMP_HOME");
    return home != nullptr && *home != 0 ? std::string(home) + "/keybindings.conf" : std::string();
}

// A scratch frame on the guest stack for one handler: guest calls run below it; the caller's registers come back at the end.
struct Frame
{
    PPCContext& ctx;
    uint8_t* base;
    const PPCContext saved;
    uint32_t next, limit;
    Frame(PPCContext& c, uint8_t* b) : ctx(c), base(b), saved(c)
    {
        const uint32_t top = (c.r1.u32 - 0x1000) & ~15u;
        PPC_STORE_U32(top, saved.r1.u32);  // the back chain
        next = top + 128, limit = top + 0x1000 - 16;  // past the callee argument area
        ctx.r1.u64 = top;
    }
    ~Frame() { ctx = saved; }
    uint32_t Alloc(uint32_t bytes)
    {
        const uint32_t at = next;
        next += (bytes + 7) & ~7u;
        if (next > limit)
        {
            std::fprintf(stderr, "[controls] KEYBOARD CONTROLS: scratch frame exhausted (a defect)\n");
            std::abort();
        }
        return at;
    }
    // Scratch taken after Mark() is reused after Reset(mark) (each label's text and slot live only for its calls).
    uint32_t Mark() const { return next; }
    void Reset(uint32_t mark) { next = mark; }
    void Call(uint32_t function) { PPC_CALL_INDIRECT_FUNC(function); }
    uint32_t VFunction(uint32_t object, uint32_t slot) { return PPC_LOAD_U32(PPC_LOAD_U32(object) + slot); }
    uint32_t New(uint32_t bytes)
    {
        ctx.r3.u64 = bytes;
        Call(kNew);
        return ctx.r3.u32;
    }
    // A slot holding a new CUserString with `text`.
    uint32_t String(const std::string& text)
    {
        const uint32_t chars = Alloc(uint32_t(text.size() + 1) * 2), slot = Alloc(4);
        for (size_t i = 0; i < text.size(); i++)
            PPC_STORE_U16(chars + 2 * i, uint16_t(uint8_t(text[i])));
        PPC_STORE_U16(chars + 2 * text.size(), 0);
        ctx.r3.u64 = slot, ctx.r4.u64 = chars, ctx.r5.s64 = -1;
        Call(kUserString);
        return slot;
    }
    void Release(uint32_t slot)
    {
        if (const uint32_t object = PPC_LOAD_U32(slot))
        {
            ctx.r3.u64 = object;
            Call(VFunction(object, 8));
        }
    }
};

// The row's options: every key name, then "PRESS A KEY", then (an action that has several keys when the submenu opens) its keys.
size_t KeyCount() { return runtime::input::kKeyNameCount; }

size_t ValueOf(const controls::Session::View& view, size_t row)
{
    if (row >= runtime::input::kActionCount)
        return 0;
    if (view.waitingRow == row)
        return KeyCount();
    const std::vector<uint16_t>& keys = view.keys[row];
    if (keys.empty())
        return KeyCount() + 1;
    if (keys.size() > 1)
        return KeyCount() + 2;
    for (size_t i = 0; i < KeyCount(); i++)
        if (!keys.empty() && runtime::input::kKeyNames[i].scancode == keys[0])
            return i;
    return KeyCount();
}

void SetText(Frame& f, uint32_t element, const std::string& text)
{
    const uint32_t mark = f.Mark();
    const uint32_t slot = f.String(text);
    f.ctx.r3.u64 = element, f.ctx.r4.u64 = slot;
    f.Call(kSetText);
    f.Release(slot);
    f.Reset(mark);
}

// One option of a list source.
void AddOption(Frame& f, uint32_t source, uint32_t value, const std::string& label)
{
    const uint32_t mark = f.Mark();
    const uint32_t made = f.String(label), copy = f.Alloc(4);
    f.ctx.r3.u64 = copy, f.ctx.r4.u64 = made;
    f.Call(kStringCopy);
    f.ctx.r3.u64 = source, f.ctx.r4.u64 = value, f.ctx.r5.u64 = copy, f.ctx.r6.u64 = 0, f.ctx.r7.u64 = value, f.ctx.r8.u64 = 0, f.ctx.r9.u64 = 1;
    f.ctx.f1.f64 = -1.0;
    f.Call(kListSourceAdd);
    f.Release(copy);
    f.Release(made);
    f.Reset(mark);
}

void Assign(Frame& f, uint32_t field, const std::string& text)
{
    const uint32_t mark = f.Mark();
    const uint32_t slot = f.String(text);
    f.ctx.r3.u64 = field, f.ctx.r4.u64 = slot;
    f.Call(kStringAssign);
    f.Release(slot);
    f.Reset(mark);
}

// Redraws the rows' values and the prompt from the session.
void Refresh(PPCContext& ctx, uint8_t* base, uint32_t self)
{
    const controls::Session::View view = controls::KeyboardMenu().Snapshot();
    if (view.rows.empty())
        return;
    Frame f(ctx, base);
    const uint32_t collection = PPC_LOAD_U32(self + kHudCollection);
    const uint32_t begin = PPC_LOAD_U32(collection + kCollectionItems), end = PPC_LOAD_U32(collection + kCollectionItems + 4);
    for (uint32_t at = begin, row = 0; at < end && row < runtime::input::kActionCount; at += 4, row++)
    {
        const uint32_t item = PPC_LOAD_U32(at);
        f.ctx.r3.u64 = item, f.ctx.r4.u64 = ValueOf(view, row), f.ctx.r5.u64 = 0;
        f.Call(f.VFunction(item, kItemSetValue));
    }
    const uint32_t binding = PPC_LOAD_U32(self + kHudList) + kListBinding;
    f.ctx.r3.u64 = binding;
    f.Call(f.VFunction(binding, kBindingRefresh));
    SetText(f, self + kHudDescription, view.prompt);
    PPC_STORE_U8(self + kHudChanged, 0);
}

// The pieces of a submenu's rows (both submenus): a new collection; a new row with its label, its description and an empty list
// of options (returned in `source`); the row put in the collection and set to `value`; the collection bound to the list, the
// screen's own rows freed.
uint32_t NewCollection(Frame& f)
{
    const uint32_t collection = f.New(kCollectionSize);
    f.ctx.r3.u64 = collection;
    f.Call(kCollectionNew);
    return f.ctx.r3.u32;
}

uint32_t NewRow(Frame& f, uint32_t row, const std::string& label, const std::string& description, uint32_t& source)
{
    uint8_t* const base = f.base;  // the guest memory the PPC_ macros address
    const uint32_t item = f.New(kItemSize);
    PPC_STORE_U32(item, kItemVtable);
    for (uint32_t o = 4; o < 24; o += 4)
        PPC_STORE_U32(item + o, 0);
    PPC_STORE_U8(item + 24, 0);
    PPC_STORE_U32(item + 12, row);
    Assign(f, item + 16, label);
    Assign(f, item + 20, description);
    source = f.New(kListSourceSize);
    f.ctx.r3.u64 = source, f.ctx.r4.u64 = 2;
    f.Call(kListSourceNew);
    source = f.ctx.r3.u32;
    return item;
}

void AddRow(Frame& f, uint32_t collection, uint32_t item, uint32_t source, uint32_t value)
{
    uint8_t* const base = f.base;  // the guest memory the PPC_ macros address
    PPC_STORE_U32(item + 4, source);
    const uint32_t mark = f.Mark();
    const uint32_t slot = f.Alloc(4);
    PPC_STORE_U32(slot, item);
    f.ctx.r3.u64 = collection + kCollectionItems, f.ctx.r4.u64 = slot;
    f.Call(kCollectionAdd);
    f.ctx.r3.u64 = item, f.ctx.r4.u64 = value, f.ctx.r5.u64 = 0;
    f.Call(f.VFunction(item, kItemSetValue));
    f.Reset(mark);
}

void ShowRows(Frame& f, uint32_t self, uint32_t collection)
{
    uint8_t* const base = f.base;  // the guest memory the PPC_ macros address
    const uint32_t binding = PPC_LOAD_U32(self + kHudList) + kListBinding;
    f.ctx.r3.u64 = binding, f.ctx.r4.u64 = kDisplayNameField;
    f.Call(f.VFunction(binding, kBindingField));
    f.ctx.r3.u64 = binding, f.ctx.r4.u64 = collection;
    f.Call(f.VFunction(binding, kBindingCollection));
    // The screen's own rows: freed by its own teardown, then ours are the screen's.
    freeing = true;
    f.ctx.r3.u64 = self;
    f.Call(kHudTeardown);
    freeing = false;
    PPC_STORE_U32(self + kHudCollection, collection);
    PPC_STORE_U8(self + kHudChanged, 0);
}

// The KEYBOARD CONTROLS rows.
void BuildRows(PPCContext& ctx, uint8_t* base, uint32_t self)
{
    const controls::Session::View view = controls::KeyboardMenu().Snapshot();
    Frame f(ctx, base);
    const uint32_t collection = NewCollection(f);
    for (uint32_t row = 0; row < controls::Menu::kRows; row++)
    {
        const bool action = row < runtime::input::kActionCount;
        uint32_t source = 0;
        const uint32_t item = NewRow(f, row, view.rows[row].label, action ? view.prompt : "RESET ALL KEYS TO THE DEFAULTS. A: RESET.", source);
        if (action)
        {
            for (size_t i = 0; i < KeyCount(); i++)
                AddOption(f, source, uint32_t(i), controls::KeyLabel(runtime::input::kKeyNames[i].scancode));
            AddOption(f, source, uint32_t(KeyCount()), "PRESS A KEY");
            AddOption(f, source, uint32_t(KeyCount() + 1), "NONE");
            if (view.keys[row].size() > 1)
                AddOption(f, source, uint32_t(KeyCount() + 2), controls::KeysLabel(view.keys[row]));
        }
        else
            AddOption(f, source, 0, "");
        AddRow(f, collection, item, source, uint32_t(ValueOf(view, row)));
    }
    ShowRows(f, self, collection);
}

// The GRAPHICS rows: each value is one of the row's options, in the file's order.
// The running game's HDR state (renderer.h HdrOutputNow) into the menu's status and the Settings panel's file.
void PublishHdr()
{
    const runtime::video::HdrOutput now = runtime::video::HdrOutputNow();
    settings::PublishHdrStatus({true, now.active, now.reason});
}

void BuildGraphicsRows(PPCContext& ctx, uint8_t* base, uint32_t self)
{
    Frame f(ctx, base);
    const uint32_t collection = NewCollection(f);
    for (uint32_t row = 0; row < settings::Menu::Rows(); row++)
    {
        uint32_t source = 0;
        const uint32_t item = NewRow(f, row, settings::Menu::Label(row), settings::Menu::Description(row), source);
        for (uint32_t value = 0; value < settings::Menu::Count(row); value++)
            AddOption(f, source, value, settings::Menu::ValueLabel(row, value));
        AddRow(f, collection, item, source, uint32_t(graphics->Value(row)));
    }
    ShowRows(f, self, collection);
}

// GRAPHICS: the rows' spinners as the player left them are the new values (saved at once, graphics_menu.h); then the rows show
// what is saved (a value that could not be saved goes back) and the line under the list says so.
void ApplyGraphics(PPCContext& ctx, uint8_t* base, uint32_t self, bool readRows)
{
    Frame f(ctx, base);
    const uint32_t collection = PPC_LOAD_U32(self + kHudCollection);
    const uint32_t begin = PPC_LOAD_U32(collection + kCollectionItems), end = PPC_LOAD_U32(collection + kCollectionItems + 4);
    for (uint32_t at = begin, row = 0; at < end && row < settings::Menu::Rows(); at += 4, row++)
    {
        const uint32_t item = PPC_LOAD_U32(at);
        if (readRows && graphics->Set(row, PPC_LOAD_U32(item + kItemValue)))
            std::fprintf(stderr, "[graphics] GRAPHICS: %s %s: %s\n", settings::Menu::Label(row), settings::Menu::ValueLabel(row, graphics->Value(row)),
                         graphics->Notice().c_str());
        f.ctx.r3.u64 = item, f.ctx.r4.u64 = graphics->Value(row), f.ctx.r5.u64 = 0;
        f.Call(f.VFunction(item, kItemSetValue));
    }
    const uint32_t binding = PPC_LOAD_U32(self + kHudList) + kListBinding;
    f.ctx.r3.u64 = binding;
    f.Call(f.VFunction(binding, kBindingRefresh));
    if (!graphics->Notice().empty())
        SetText(f, self + kHudDescription, graphics->Notice());
    PPC_STORE_U8(self + kHudChanged, 0);
}

void CloseSession()
{
    if (controls::KeyboardMenu().Close())
    {
        const runtime::input::ReloadResult r = runtime::input::ReloadKeyBindings(BindingsPath());
        std::fprintf(stderr, "[controls] KEYBOARD CONTROLS closed: bindings %s (%s)\n", r.applied ? "reloaded" : "NOT reloaded", r.message.c_str());
    }
    else
        std::fprintf(stderr, "[controls] KEYBOARD CONTROLS closed: no change\n");
}

// The reusable entry: the option screen with the submenu pending, opened for `opener` as it opens its own option screens, then
// the opener's own tail for them (its standings hidden: a byte and the HIDE_SCREEN_INFO event).
void Open(PPCContext& ctx, uint8_t* base, uint32_t opener, uint32_t infoHidden, uint32_t hideEvent, bool once, const char* from,
          Submenu which = Submenu::Keyboard)
{
    if (which == Submenu::Keyboard ? BindingsPath().empty() || controls::KeyboardMenu().IsOpen() : !settings::Available() || screen.load() != 0)
        return;
    pending = which;
    Frame f(ctx, base);
    f.Call(kScreenManager);
    const uint32_t manager = f.ctx.r3.u32;
    f.ctx.r3.u64 = manager, f.ctx.r4.u64 = PPC_LOAD_U32(opener + kScreenOwner), f.ctx.r5.u64 = kHudScreenId, f.ctx.r6.u64 = 0, f.ctx.r7.s64 = -1;
    f.Call(f.VFunction(manager, kManagerOpen));
    if (!once || PPC_LOAD_U8(opener + infoHidden) == 0)
    {
        PPC_STORE_U8(opener + infoHidden, 1);
        f.ctx.r3.u64 = opener, f.ctx.r4.u64 = PPC_LOAD_U32(hideEvent);
        f.Call(kFireSceneEvent);
    }
    std::fprintf(stderr, "[controls] %s opening from the %s (option screen %u)\n", which == Submenu::Keyboard ? "KEYBOARD CONTROLS" : "GRAPHICS", from,
                 unsigned(kHudScreenId));
}

}  // namespace

// The entry handlers (a menu row's action; r3 = the menu): the Pause menu (its tail sets the byte once) and the Pre-Race screen
// (every time), as each does for its Audio and HUD rows.
void OpenKeyboardControls(PPCContext& ctx, uint8_t* base)
{
    Open(ctx, base, ctx.r3.u32, kPauseInfoHidden, kHideScreenInfoEvent, true, "Pause menu");
}

void OpenKeyboardControlsPreRace(PPCContext& ctx, uint8_t* base)
{
    Open(ctx, base, ctx.r3.u32, kPreRaceInfoHidden, kPreRaceHideScreenInfoEvent, false, "Pre-Race screen");
}

// The Pause menu's GRAPHICS row.
void OpenGraphics(PPCContext& ctx, uint8_t* base)
{
    Open(ctx, base, ctx.r3.u32, kPauseInfoHidden, kHideScreenInfoEvent, true, "Pause menu", Submenu::Graphics);
}

// The Pre-Race screen's GRAPHICS row.
void OpenGraphicsPreRace(PPCContext& ctx, uint8_t* base)
{
    Open(ctx, base, ctx.r3.u32, kPreRaceInfoHidden, kPreRaceHideScreenInfoEvent, false, "Pre-Race screen", Submenu::Graphics);
}

namespace {

// The help bar: A SET KEY, X DEFAULT, Y CLEAR, B BACK (the HUD screen's SAVE, DISCARD, RESET DEFAULT removed); GRAPHICS: X DEFAULT,
// B BACK (left and right change a value).
void HelpBar(PPCContext& ctx, uint8_t* base, uint32_t self)
{
    Frame f(ctx, base);
    f.ctx.r3.u64 = kHelpBarName;
    f.Call(kNameHash);
    f.ctx.r4.u64 = f.ctx.r3.u64, f.ctx.r3.u64 = self;
    f.Call(kComponent);
    const uint32_t bar = f.ctx.r3.u32;
    if (bar == 0)
        return;
    for (const uint32_t slot : {3u, 1u, 0u})
    {
        f.ctx.r3.u64 = bar, f.ctx.r4.u64 = slot;
        f.Call(kHelpRemove);
    }
    static const struct { uint32_t slot; const char* label; bool graphics; } kButtons[] = {
        {0, "SET KEY", false}, {2, "DEFAULT", true}, {3, "CLEAR", false}, {1, "BACK", true}};
    for (const auto& b : kButtons)
    {
        if (shown == Submenu::Graphics && !b.graphics)
            continue;
        const uint32_t mark = f.Mark();
        const uint32_t made = f.String(b.label), copy = f.Alloc(4);
        f.ctx.r3.u64 = copy, f.ctx.r4.u64 = made;
        f.Call(kStringCopy);
        f.ctx.r3.u64 = bar, f.ctx.r4.u64 = b.slot, f.ctx.r5.u64 = copy;
        f.Call(kHelpAdd);
        f.Release(copy);
        f.Release(made);
        f.Reset(mark);
    }
    const uint32_t binding = bar + kListBinding;
    f.ctx.r3.u64 = binding;
    f.Call(f.VFunction(binding, kBindingRefresh));
}

}  // namespace

// Slot 3: binds and builds. The submenu's rows replace the screen's.
void KeyboardHudBind(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const uint32_t self = ctx.r3.u32;
    original(ctx, base);
    const Submenu which = pending.exchange(Submenu::None);
    if (which == Submenu::None)
        return;
    screen = self, shown = which, focusRow = 0;
    if (which == Submenu::Graphics)
    {
        graphics.emplace(settings::GraphicsPath(), [](size_t row, size_t value) {
            if (row == settings::Menu::kHdrRow)  // the HDR preference, from the next presented swap; then what is active now
            {
                runtime::video::SetHdrPreference(value != 0);
                PublishHdr();
                return true;
            }
            if (row == settings::Menu::kExtendedRow)  // Extended View, from the next frame (kernel/extended_view.h); only with its title hook
            {
                static const bool armed = [] { const char* v = std::getenv("FM4_EXTENDED_VIEW_HOOK"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
                runtime::view::Global().SetPreference(value != 0);
                return armed;
            }
            if (row == settings::Menu::kResolutionRow)  // FM4 Recomp (live resolution): asked for now, made at the next frame boundary the renderer finds safe
                return runtime::video::RequestInternalRes(settings::kResolutions[value].file);
            return row == settings::Menu::kSmoothingRow && runtime::video::SetPostAaLive(settings::kSmoothings[value].file);
        });
        PublishHdr();  // what is active now, for the HDR row's description
        BuildGraphicsRows(ctx, base, self);
        std::fprintf(stderr, "[graphics] GRAPHICS shown: %s, %s, HDR %s, EXTENDED VIEW %s (%s); %s\n", settings::Menu::ValueLabel(0, graphics->Value(0)),
                     settings::Menu::ValueLabel(1, graphics->Value(1)), settings::Menu::ValueLabel(2, graphics->Value(2)),
                     settings::Menu::ValueLabel(3, graphics->Value(3)), settings::GraphicsPath().c_str(),
                     settings::HdrStatusLine(settings::GetHdrStatus()).c_str());
        return;
    }
    controls::KeyboardMenu().Open(*runtime::input::ActiveKeyBindings(), BindingsPath(),
                                  uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    BuildRows(ctx, base, self);
    std::fprintf(stderr, "[controls] KEYBOARD CONTROLS shown: %u rows\n", unsigned(controls::Menu::kRows));
}

// Slot 5: the title.
void KeyboardHudTitle(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const uint32_t self = ctx.r3.u32;
    original(ctx, base);
    if (self != screen.load())
        return;
    {
        Frame f(ctx, base);
        SetText(f, self + kHudScreenName, shown == Submenu::Graphics ? "GRAPHICS" : "KEYBOARD CONTROLS");
    }
    HelpBar(ctx, base, self);
}

// Slot 0: list events (this, type, sender, row). Focus: kept, then the game shows the row's description. A value change (left and
// right): refused (the row's value is its key).
void KeyboardHudList(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const uint32_t self = ctx.r3.u32;
    if (self == screen.load() && ctx.r4.u32 == 1)
    {
        if (ctx.r5.u32 == PPC_LOAD_U32(self + kHudList) + kListSender)
            focusRow = ctx.r6.u32;
        else if (shown == Submenu::Graphics)
        {
            ApplyGraphics(ctx, base, self, true);  // GRAPHICS: left or right on a row is the new value
            return;
        }
        else
        {
            Refresh(ctx, base, self);
            return;
        }
    }
    original(ctx, base);
}

// Slot 9: input (this, input).
void KeyboardHudInput(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const uint32_t self = ctx.r3.u32;
    if (self != screen.load())
    {
        original(ctx, base);
        return;
    }
    uint32_t code = 0;
    {
        Frame f(ctx, base);
        const uint32_t codes = f.Alloc(16);
        PPC_STORE_U32(codes, kInputA);
        PPC_STORE_U32(codes + 4, kInputB);
        PPC_STORE_U32(codes + 8, kInputX);
        PPC_STORE_U32(codes + 12, kInputY);
        f.ctx.r3.u64 = self, f.ctx.r4.u64 = f.saved.r4.u64, f.ctx.r5.u64 = codes, f.ctx.r6.u64 = 4;
        f.Call(f.VFunction(self, kScreenMatchInput));
        if (f.ctx.r3.u32 != 0)
            code = PPC_LOAD_U32(f.ctx.r3.u32);
    }
    if (shown == Submenu::Graphics)
    {
        // GRAPHICS: B is the game's Back (nothing to ask: each change was saved as it was made); X the focused row's default; A and
        // Y do nothing (the HUD screen's save and reset are not this submenu's).
        if (code == kInputB)
        {
            std::fprintf(stderr, "[graphics] GRAPHICS closed: %s, %s, HDR %s, EXTENDED VIEW %s\n", settings::Menu::ValueLabel(0, graphics->Value(0)),
                         settings::Menu::ValueLabel(1, graphics->Value(1)), settings::Menu::ValueLabel(2, graphics->Value(2)),
                         settings::Menu::ValueLabel(3, graphics->Value(3)));
            screen = 0, shown = Submenu::None;
            graphics.reset();
            PPC_STORE_U8(self + kHudChanged, 0);
            original(ctx, base);
        }
        else if (code == kInputX)
        {
            if (focusRow < settings::Menu::Rows() && graphics->Default(focusRow))
                std::fprintf(stderr, "[graphics] GRAPHICS: %s back to its default, saved\n", settings::Menu::Label(focusRow));
            ApplyGraphics(ctx, base, self, false);
        }
        else if (code != kInputA && code != kInputY)
            original(ctx, base);
        return;
    }
    controls::Session& session = controls::KeyboardMenu();
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (code == kInputA && !session.AcceptsConfirm(now))
        ;  // the A that opened the submenu (still held, or delivered right after the opening): browsing starts on STEER LEFT
    else if (code == kInputA)
    {
        session.Select(focusRow);
        Refresh(ctx, base, self);
    }
    else if (code == kInputB)
    {
        if (session.Back())
            Refresh(ctx, base, self);
        else
        {
            CloseSession();
            screen = 0, shown = Submenu::None;
            PPC_STORE_U8(self + kHudChanged, 0);  // no "save your changes?" question: the keys were saved as they changed
            original(ctx, base);                  // the game's Back: to the menu that opened it
        }
    }
    else if (code == kInputX)
    {
        // X: the focused action's default key (ReCarbon: X restores the row); also the window loop's redraw pulse, which comes
        // only while the submenu owns the input (then not a press of the player's).
        if (!session.ConsumePulse(now) && !session.OwnsInput() && focusRow < runtime::input::kActionCount)
            session.RestoreDefault(focusRow);
        Refresh(ctx, base, self);
    }
    else if (code == kInputY)
    {
        // Y: the focused action has no key (ReCarbon: Y clears); on RESET TO DEFAULTS, the reset question.
        if (focusRow == controls::Menu::kResetRow)
            session.Select(controls::Menu::kResetRow);
        else
            session.Clear(focusRow);
        Refresh(ctx, base, self);
    }
    else
        original(ctx, base);
}

// Slot 4: the screen closes (or our own call frees its old rows).
void KeyboardHudTeardown(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    if (!freeing && ctx.r3.u32 == screen.load())
    {
        screen = 0, shown = Submenu::None;
        graphics.reset();
        if (controls::KeyboardMenu().IsOpen())
            CloseSession();
    }
    original(ctx, base);
}

}  // namespace fm4::title
