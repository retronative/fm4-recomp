// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Adapted from fm4-engine 0.3.2 host/src/input/prompt_strings.cpp (FM4 Recomp 0.3.1's "prompts follow the input device"):
//
//   Copyright (c) 2026, i2kiselev
//   BSD 3-Clause License. Redistribution and use in source and binary forms, with or without modification, are permitted provided
//   that the following conditions are met: 1. Redistributions of source code must retain the above copyright notice, this list of
//   conditions and the following disclaimer. 2. Redistributions in binary form must reproduce the above copyright notice, this list
//   of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
//   3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products
//   derived from this software without specific prior written permission. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
//   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
//   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
//   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
//   OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// The keyboard wording of FM4's English strings that name pad buttons in prose ("Press A to start the game." -> "Press SPACE to
// start"). Pure: no guest memory, no bindings file; the caller resolves each control's key label first (runtime/game/prompt_text.cpp,
// with kernel/controls_menu.h KeyLabel). The string keys are 16-bit and shared by every table: a key's text is rewritten only when it
// contains one of the phrases, so another table's string under the same key stays as it is.
//
// FM4 Recomp changes:
//   - one key set (key_bindings.h's 17 actions) instead of the reference's menu and driving sets; `driving` only chooses how
//     "the LEFT STICK" reads (the steering keys);
//   - our actions have no key for the left stick's up/down or press, nor the right stick: a phrase naming one of those, or a control
//     whose action is unbound (`none`), keeps the game's words (never a guess at a key that does nothing);
//   - labels come resolved (controls_menu.h KeyLabel: "SPACE", "BACKSPACE", "LEFT ARROW", ...), so the reference's Words()/Group() label
//     table is not taken;
//   - one pass over the game's text (KeyboardText), so a key label is never read as a phrase again;
//   - header-only, so tests/run_keyboard_prompt_tests.sh builds it alone.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace runtime::prompts {

// The label of the first key bound to each control a prompt can name; "" when the action has no key.
struct KeyLabels
{
    std::string a, b, x, y, start, back, leftShoulder, rightShoulder, rightTrigger, dpadDown, steerLeft, steerRight;
};

struct PromptKey
{
    uint16_t key;
    bool driving;  // the string is about a race (the steering keys for "the LEFT STICK")
};

// The EN string tables' keys (reference prompt_strings.cpp:22-47; the table and name of each in the comment). UNRESOLVED on our
// disc until checked by tests/check_prompt_string_keys.py (README "String keys").
inline constexpr std::array<PromptKey, 24> kPromptKeys = {{
    {0x3D74, false},  // CarExperience IDS_Explore_Description
    {0x42FB, false},  // CarExperience IDS_Interact_Description1
    {0x437B, false},  // CarExperience IDS_Interact_Description2
    {0xBA58, false},  // CarExperience IDS_Getin_Description
    {0xDE8C, false},  // CarExperience IDS_Move_Description2
    {0xDF0C, false},  // CarExperience IDS_Move_Description1
    {0xEC34, false},  // CarExperience IDS_Look_Description2
    {0xEDB4, false},  // CarExperience IDS_Look_Description1
    {0x4D92, true},   // InGame IDS_Accelerate_Message
    {0x76B3, true},   // InGame IDS_View_Message
    {0xF762, true},   // InGame IDS_Steering_Message
    {0xFC27, false},  // Lobby IDS_OptionBucketNameDesc
    {0x5222, false},  // Main IDS_LB
    {0xD225, false},  // Main IDS_RB
    {0xC0F5, false},  // Main IDS_Rivals_Message
    {0x90AB, false},  // MainMenu IDS_A_Start
    {0x206F, true},   // MyProfile IDS_Rewind_Message
    {0x6877, true},   // MyProfile IDS_Controller_Discription
    {0x63B4, false},  // Storefront IDS_GamertagDefault
    {0xE49E, false},  // Storefront IDS_ManualKeywordDefault
    {0x2ADE, true},   // Telemetry IDS_Press_Dpad_Down
    {0xD422, false},  // Tuning IDS_SetupFileLocked_Remove_Msg
    {0xEC3B, false},  // UpgradeAreas IDS_Description_3
    {0xED3B, false},  // UpgradeAreas IDS_Description_1
}};

inline const PromptKey* FindPromptKey(uint16_t key)
{
    const auto it = std::find_if(kPromptKeys.begin(), kPromptKeys.end(), [key](const PromptKey& p) { return p.key == key; });
    return it == kPromptKeys.end() ? nullptr : &*it;
}

inline bool IsPromptKey(uint16_t key) { return FindPromptKey(key) != nullptr; }

enum class Slot { A, B, X, Y, RB, RT, Back, Start, DpadDown, LeftStick, LeftStickUp, LeftStickPress, RightStick };

struct Phrase
{
    std::u16string_view from, before;
    Slot slot;
    std::u16string_view after;
};

// Reference prompt_strings.cpp:79-101, unchanged. Longest first, so a phrase inside a longer one does not take it apart.
inline constexpr Phrase kPhrases[] = {
    // The title screen's lines hug this text: a key name longer than A must not widen it (reference comment).
    {u"Press A to start the game.", u"Press ", Slot::A, u" to start"},
    {u"or pull the RIGHT TRIGGER", u"or ", Slot::RT, u""},
    {u"Push forward on the LEFT STICK", u"Press ", Slot::LeftStickUp, u""},
    {u"press the LEFT STICK (lean out)", u"press ", Slot::LeftStickPress, u" (lean out)"},
    {u"HOLD DOWN THE RIGHT TRIGGER", u"HOLD DOWN ", Slot::RT, u""},
    {u"PRESS THE BACK BUTTON", u"PRESS ", Slot::Back, u""},
    {u"USE THE LEFT STICK", u"USE ", Slot::LeftStick, u""},
    {u"PRESS D-PAD DOWN", u"PRESS ", Slot::DpadDown, u""},
    {u"press the 'X' button", u"press ", Slot::X, u""},
    {u"Press the RB button", u"Press ", Slot::RB, u""},
    {u"press the B button", u"press ", Slot::B, u""},
    {u"Press the A button", u"Press ", Slot::A, u""},
    {u"press the A button", u"press ", Slot::A, u""},
    {u"PRESS X BUTTON", u"PRESS ", Slot::X, u""},
    {u"press START to", u"press ", Slot::Start, u" to"},
    {u"Rewind button", u"Rewind key (", Slot::Y, u")"},
    {u"the LEFT STICK", u"", Slot::LeftStick, u""},
    {u"the RIGHT STICK", u"", Slot::RightStick, u""},
    {u"The RIGHT STICK", u"", Slot::RightStick, u""},
    {u"Press A to", u"Press ", Slot::A, u" to"},
};

inline std::u16string Wide(std::string_view text)
{
    std::u16string out;
    for (const char c : text)
        out += char16_t(uint8_t(c));
    return out;
}

// The words for a slot; "" when the keyboard has no key for it (the phrase then stays as the game wrote it).
inline std::string SlotText(Slot slot, const KeyLabels& k, bool driving)
{
    switch (slot)
    {
    case Slot::A: return k.a;
    case Slot::B: return k.b;
    case Slot::X: return k.x;
    case Slot::Y: return k.y;
    case Slot::RB: return k.rightShoulder;
    case Slot::RT: return k.rightTrigger;
    case Slot::Back: return k.back;
    case Slot::Start: return k.start;
    case Slot::DpadDown: return k.dpadDown;
    case Slot::LeftStick:
        // FM4 Recomp: only the race's steering has keys (A/D by default); outside a race the left stick's four directions have none.
        if (!driving || k.steerLeft.empty() || k.steerRight.empty())
            return "";
        if (k.steerLeft == "LEFT ARROW" && k.steerRight == "RIGHT ARROW")
            return "LEFT/RIGHT ARROW";
        return k.steerLeft + "/" + k.steerRight;
    case Slot::LeftStickUp:
    case Slot::LeftStickPress:
    case Slot::RightStick:
        return "";  // FM4 Recomp: no keyboard action (key_bindings.h kActions)
    }
    return "";
}

// The text for keyboard players, or nullopt when `key` is not a prompt key or its text names no button the keyboard has.
inline std::optional<std::u16string> KeyboardText(uint16_t key, std::u16string_view text, const KeyLabels& k)
{
    const PromptKey* found = FindPromptKey(key);
    if (found == nullptr)
        return std::nullopt;
    // Strings that are a button name and nothing else (the tab strips' LB / RB), matched whole: as phrases they would take apart
    // words in the other strings under these keys (reference prompt_strings.cpp:107-114).
    if (text == u"LB" || text == u"RB")
    {
        const std::string& label = text == u"LB" ? k.leftShoulder : k.rightShoulder;
        if (label.empty())
            return std::nullopt;
        return Wide(label);
    }
    // FM4 Recomp: one pass over the game's text, left to right; at each place the first phrase of the table that starts there (the
    // longest) is taken whole. The reference applies each phrase to the text already rewritten, so a key label could be read as a
    // phrase again (RB bound to the A key: "Press the RB button to" -> "Press A to" -> "Press SPACE to"). A phrase whose control
    // has no key is kept as the game wrote it, whole (no shorter phrase inside it is rewritten).
    std::u16string out;
    bool changed = false;
    for (size_t at = 0; at < text.size();)
    {
        const Phrase* phrase = nullptr;
        for (const Phrase& p : kPhrases)
            if (text.compare(at, p.from.size(), p.from) == 0)
            {
                phrase = &p;
                break;
            }
        if (phrase == nullptr)
        {
            out += text[at++];
            continue;
        }
        const std::string words = SlotText(phrase->slot, k, found->driving);
        if (words.empty())
            out += phrase->from;
        else
        {
            out += std::u16string(phrase->before) + Wide(words) + std::u16string(phrase->after);
            changed = true;
        }
        at += phrase->from.size();
    }
    if (!changed)
        return std::nullopt;
    return out;
}

}  // namespace runtime::prompts
