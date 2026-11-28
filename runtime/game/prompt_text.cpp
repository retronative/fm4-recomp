// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Adapted from fm4-engine 0.3.2 host/src/game_hooks.cpp:253-296 and 1471-1592 (Fm4GraphicsOptionText's prompt part,
// Fm4TextElementUpdate, GuestText), Copyright (c) 2026, i2kiselev, BSD 3-Clause License; its full notice is in
// runtime/kernel/keyboard_prompts.h.
//
// FM4 Recomp (keyboard prompts): the guest side of kernel/keyboard_prompts.h, through two title hooks (config/title_hooks.toml, kind
// "delegate", flag FM4_KEYBOARD_PROMPTS; FM4's addresses and their evidence are there, named in title_symbols.h):
//   - PromptStringLookup, on the string table lookup: for one of the 24 prompt keys, while the keyboard was used last
//     (input_live.h KeyboardLastUsed), the text returned is the keyboard wording, a guest copy kept for the run (one per distinct
//     text). A lookup made by the scene text setup (its return address) records the element it builds, with the game's text and
//     the text shown, whichever device is current;
//   - PromptTextElementUpdate, on a text element's update: a recorded element whose wording no longer matches the device (or the
//     keys, after KEYBOARD CONTROLS changed them) gets the other text through the game's own assign, and lays out again, as the
//     game's SetText does. An element that no longer shows the text it was given has been reused: it is forgotten.
// FM4 Recomp changes from the reference: delegates instead of midasm hooks (the lookup's original is called directly, no nested
// call guard; the element's rewrite runs at the update's entry, before the update copies the text, not at the reference's hook point after
// the copy: drafts README "Entry instead of the mid-function hook"); guest copies from the kernel arena (kernel/arena.h) instead of the system heap; no string
// log; the key labels from our bindings (key_bindings.h, controls_menu.h KeyLabel).
#include "title_symbols.h"
#include "kernel/arena.h"
#include "kernel/controls_menu.h"
#include "kernel/input_live.h"
#include "kernel/key_bindings.h"
#include "kernel/keyboard_prompts.h"
#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace fm4::title {
namespace {

using namespace fm4::sym;
namespace prompts = runtime::prompts;

// Longer than any string of FM4's tables; a longer read is treated as no text (a defect or a reused element).
constexpr size_t kMaxChars = 4096;

// The label of the first key bound to an action ("" when it has none), by its id in key_bindings.h kActions.
std::string FirstKey(const runtime::input::KeyBindings& bindings, const char* id)
{
    for (size_t i = 0; i < runtime::input::kActionCount; i++)
        if (std::strcmp(runtime::input::kActions[i].id, id) == 0)
            return bindings.keys[i].empty() ? std::string() : runtime::controls::KeyLabel(bindings.keys[i][0]);
    std::fprintf(stderr, "[prompts] no keyboard action '%s' (a defect): its prompts keep the pad's words\n", id);
    return {};
}

prompts::KeyLabels LabelsNow()
{
    const auto bindings = runtime::input::ActiveKeyBindings();
    prompts::KeyLabels k;
    k.a = FirstKey(*bindings, "a"), k.b = FirstKey(*bindings, "b"), k.x = FirstKey(*bindings, "x"), k.y = FirstKey(*bindings, "y");
    k.start = FirstKey(*bindings, "start"), k.back = FirstKey(*bindings, "back");
    k.leftShoulder = FirstKey(*bindings, "left_shoulder"), k.rightShoulder = FirstKey(*bindings, "right_shoulder");
    k.rightTrigger = FirstKey(*bindings, "throttle"), k.dpadDown = FirstKey(*bindings, "dpad_down");
    k.steerLeft = FirstKey(*bindings, "steer_left"), k.steerRight = FirstKey(*bindings, "steer_right");
    return k;
}

// The text for the device used last: the keyboard wording, or nullopt (the game's own text).
std::optional<std::u16string> TextNow(uint16_t key, const std::u16string& game)
{
    if (!runtime::input::KeyboardLastUsed())
        return std::nullopt;
    return prompts::KeyboardText(key, game, LabelsNow());
}

// A NUL-terminated UTF-16BE string at `address`; nullopt past kMaxChars.
std::optional<std::u16string> ReadText(uint8_t* base, uint32_t address)
{
    std::u16string text;
    for (uint32_t at = address;; at += 2)
    {
        const char16_t c = char16_t(PPC_LOAD_U16(at));
        if (c == 0)
            return text;
        if (text.size() == kMaxChars)
            return std::nullopt;
        text += c;
    }
}

// The guest copy of `text` (UTF-16BE, terminated), made once per distinct text and kept for the run: the game may hold the pointer
// a lookup returned for as long as it likes. The keys' labels are bounded, so the texts are too (24 keys x the key sets used).
std::mutex copiesLock;
std::unordered_map<std::u16string, uint32_t> copies;

uint32_t GuestCopy(uint8_t* base, const std::u16string& text)
{
    std::lock_guard lock(copiesLock);
    uint32_t& address = copies[text];
    if (address == 0)
    {
        address = runtime::ArenaAlloc(base, uint32_t(2 * (text.size() + 1)));  // zeroed: the terminator is there
        for (size_t i = 0; i < text.size(); i++)
            PPC_STORE_U16(address + 2 * uint32_t(i), uint16_t(text[i]));
        std::fprintf(stderr, "[prompts] keyboard text %zu: %s\n", copies.size(), std::string(text.begin(), text.end()).c_str());
    }
    return address;
}

// Scene text elements built from a prompt key: element -> its key, the game's text, the text it shows.
struct LiveText
{
    uint16_t key;
    std::u16string game, shown;
    // HYPOTHESIS (README): the reference lays the element out once more after a rewrite, because its layout measured the text it
    // replaced; at the entry the copy already holds the new text, so this second layout may be redundant. Kept until a run shows it.
    bool relayout = false;
};
std::mutex liveLock;
std::unordered_map<uint32_t, LiveText> live;

// The element's text now (its std::wstring at kTextElementString), or nullopt when the fields do not describe a text.
std::optional<std::u16string> ElementText(uint8_t* base, uint32_t element)
{
    const uint32_t string = element + kTextElementString, length = PPC_LOAD_U32(element + kTextElementLength),
                   capacity = PPC_LOAD_U32(element + kTextElementCapacity);
    if (length > kMaxChars || length > capacity)
        return std::nullopt;
    const uint32_t chars = capacity < 8 ? string : PPC_LOAD_U32(string);
    std::u16string text;
    for (uint32_t i = 0; i < length; i++)
        text += char16_t(PPC_LOAD_U16(chars + 2 * i));
    return text;
}

}  // namespace

void PromptStringLookup(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const uint16_t key = uint16_t(ctx.r4.u32);
    const uint32_t caller = uint32_t(ctx.lr), element = ctx.r31.u32;  // the caller's r31 (the element, from the scene text setup)
    original(ctx, base);
    if (!prompts::IsPromptKey(key) || ctx.r3.u32 == 0)
        return;
    const std::optional<std::u16string> game = ReadText(base, ctx.r3.u32);
    if (!game)
        return;
    std::u16string shown = *game;
    if (const auto text = TextNow(key, *game))
    {
        ctx.r3.u64 = GuestCopy(base, *text);
        shown = *text;
    }
    if (caller == kTextSetup + kTextSetupGetTextReturn)
    {
        std::lock_guard lock(liveLock);
        live[element] = LiveText{key, *game, std::move(shown), false};
    }
}

void PromptTextElementUpdate(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const uint32_t element = ctx.r4.u32;
    std::optional<LiveText> found;
    {
        std::lock_guard lock(liveLock);
        if (const auto it = live.find(element); it != live.end())
            found = it->second;
    }
    if (found)
    {
        const std::u16string wanted = TextNow(found->key, found->game).value_or(found->game);
        if (wanted == found->shown)
        {
            if (found->relayout)
            {
                PPC_STORE_U8(element + kTextElementLayout, 1);
                std::lock_guard lock(liveLock);
                live[element].relayout = false;
            }
        }
        else if (ElementText(base, element) != found->shown)
        {
            std::lock_guard lock(liveLock);
            live.erase(element);  // another text now: the element was reused
        }
        else
        {
            // The game's own assign (string, text, length), then "lay out again", as its SetText does. Called at the update's entry,
            // as a call the update's caller could make: the registers come back before the update runs.
            const uint32_t text = GuestCopy(base, wanted);
            const PPCContext saved = ctx;
            ctx.r3.u64 = element + kTextElementString, ctx.r4.u64 = text, ctx.r5.u64 = uint32_t(wanted.size());
            PPC_CALL_INDIRECT_FUNC(kTextAssign);
            ctx = saved;
            PPC_STORE_U8(element + kTextElementLayout, 1);
            std::lock_guard lock(liveLock);
            LiveText& entry = live[element];
            entry.shown = wanted, entry.relayout = true;
        }
    }
    original(ctx, base);
}

}  // namespace fm4::title
