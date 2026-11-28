// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Adapted from FM4 Static Recomp 0.3.x host/src/input/keycap.cpp and prompt_swap.cpp (FM4 Recomp 0.3.1's keycap prompt icons):
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
// The game's button prompt icons (Textures.zip UI4/<name>.xds) drawn as keycaps of the bound keys while the keyboard was the last
// device used (FM4_KEYCAP_ICONS=1; texture_vk.cpp binds the keycap's view in place of the icon's). Pure: the text comes from a
// TextInk function (keycap_text_mac.cpp: Core Text), the keys from the caller.
//
// FM4 Recomp changes:
//   - icons are found by our ContentHash (texture_upload.h, seed 0) of the base level's guest bytes, i.e. the .xds data after its
//     0x34-byte header (hashes from the disc's Textures.zip, research tool; facts, no game bytes); the reference hashed with XXH3;
//   - one key set (key_bindings.h's 17 actions): the sticks have no keyboard action in every context, so their icons stay the pad's
//     (as the prompt text keeps "the LEFT STICK" outside races); LT is the brake key, RT the throttle key;
//   - the labels' text is drawn by Core Text in Helvetica Neue Medium (the game's help bar uses Swiss 721 Medium, a Helvetica
//     design) instead of the title's vector font, so no Fonts.zip parser; arrows stay drawn triangles;
//   - one RGBA8 image at 2x the icon (64x64 for the 32x32 icons, every level down to 1x1), not the reference's 4x in the icon's
//     guest format: the icon is ~45 texels high at QHD, so 2x is minified less and no BC3 encoder is needed;
//   - the telemetry's D-pad and tab icons (HUD/) are not replaced yet.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace runtime::video::keycap {

// A coverage mask, rows top down; (0, 0) is its top-left pixel.
struct Ink
{
    int width = 0, height = 0;
    std::vector<float> alpha;
    float& at(int x, int y) { return alpha[size_t(y) * width + x]; }
    float at(int x, int y) const { return alpha[size_t(y) * width + x]; }
};

// The ink of `text` at `pixelsPerEm`, trimmed to its inked pixels (empty when nothing is inked).
using TextInk = std::function<Ink(std::string_view text, float pixelsPerEm)>;
Ink CoreTextInk(std::string_view text, float pixelsPerEm);  // keycap_text_mac.cpp

// Labels that are drawn as triangles (the reference's arrow tokens).
inline constexpr std::string_view kArrowUp = "\xE2\x86\x91", kArrowDown = "\xE2\x86\x93", kArrowLeft = "\xE2\x86\x90", kArrowRight = "\xE2\x86\x92";
// The BACKSPACE key's label: drawn as a long left arrow, no text.
inline constexpr std::string_view kBackspace = "\xE2\x9F\xB5";

struct Icon
{
    const char* name;  // UI4/<name>.xds
    uint64_t hash;     // ContentHash(base level, 0)
    std::array<const char*, 4> actions;  // key_bindings.h kActions ids whose first keys the cap shows, in order
    bool back;         // the BACK icon: its circle ends at x 27 of 32 (the home icon follows), so the cap does too
};

// Textures.zip UI4/*_F3.xds and RBumper_I.xds: every one 32x32, fetch format 50, one level, tiled, endian 2; Back_Button_D_F3
// and Back_Button_F3, RBumper_F3 and RBumper_I share their pictures (and hashes). The sticks are absent on purpose (header).
inline constexpr uint32_t kIconWidth = 32, kIconHeight = 32, kIconFormat = 50, kIconBytes = 4096;
inline constexpr std::array<Icon, 11> kIcons = {{
    {"A_Button_F3", 0x190cf394c4dd14d9ull, {"a"}, false},
    {"B_Button_F3", 0x144c69fe319bce3bull, {"b"}, false},
    {"X_Button_F3", 0x3cf278acbbb978b2ull, {"x"}, false},
    {"Y_Button_F3", 0x7bfd13341ea90c8full, {"y"}, false},
    {"Back_Button_F3", 0xab9c70a77e262fdaull, {"back"}, true},
    {"Start_Button_F3", 0x7325a2a003120c46ull, {"start"}, false},
    {"D_Pad_Button_F3", 0xd35ef5a6e044d068ull, {"dpad_up", "dpad_left", "dpad_down", "dpad_right"}, false},
    {"D_Pad_Left_Right_F3", 0x27e4871f6a039103ull, {"dpad_left", "dpad_right"}, false},
    {"LTrigger_F3", 0x9f8030e3660a0098ull, {"brake"}, false},
    {"RTrigger_F3", 0xa2597e19908dae76ull, {"throttle"}, false},
    {"RBumper_F3", 0x01f30dddf859a467ull, {"right_shoulder"}, false},
}};

// The index in kIcons of a texture's base level, or -1. Only a 32x32 format-50 single-level texture can be one (the caller
// checks the shape before hashing).
int FindIcon(uint64_t baseLevelHash);

// What a cap shows for one key label (controls_menu.h KeyLabel: "SPACE", "LEFT ARROW", "FORWARD DELETE", "KEYPAD 7"): the arrows
// as triangle tokens, long names as keyboard legends.
std::string CapLabel(std::string_view keyLabel);

// The cap's labels from each action's key label ("" when unbound); empty when any action has no key (the pad icon stays: never a
// cap that names a key that does nothing). Consecutive equal labels are shown once.
std::vector<std::string> CapLabels(std::span<const std::string> keyLabels);

struct Rgba
{
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;  // RGBA8, straight alpha, rows top down
};

// A light keycap: a rounded grey face over a darker bottom edge, the labels centred in dark text, shrunk to fit; several labels
// share the cap, or the first alone when they would not read. A capWidth below size draws the cap that wide from the left.
Rgba RenderKeycap(std::span<const std::string> labels, uint32_t size, const TextInk& text, uint32_t capWidth = 0);

// The image's levels from `top` down to 1x1, each a 2x2 alpha-weighted average (the reference's Levels).
std::vector<Rgba> Levels(Rgba top);

// The keycap of kIcons[icon] at `scale` times the icon, every level (Levels).
std::vector<Rgba> IconLevels(size_t icon, std::span<const std::string> labels, uint32_t scale, const TextInk& text);

}  // namespace runtime::video::keycap
