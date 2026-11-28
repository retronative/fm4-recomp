// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: the GRAPHICS submenu's Y: Preview (runtime/game/keyboard_screen.cpp). While it is on, the renderer turns the pause
// screen's blur into a copy and draws the menus only inside the help bar's corner, so the paused scene shows sharp with the
// previewed graphics settings and only "B: BACK TO GRAPHICS". Pure: the renderer's draw entry asks Classify, then flattens the
// blur's constants with FlattenBlur or clips a menu draw with ClipToHint.
//
// OBSERVED (FM4_DIAG_DRAW_LIST at swap 12300, a paused race): the final 1280-wide
// target gets the tone-mapped scene (ps e9cd98c0331323d0, an unblended rectangle list), then two unblended whole-frame rectangle
// lists, a separable blur in two passes (ps 362b1c7e698a2e23, then ps f9daba5e30b16f65; absent from race frames), then the menus
// (blended triangle lists: extended_view.h IsOverlayDraw). Each blur pass samples the previous picture's copy at seven taps, the
// centre texture coordinate plus the offsets in pixel constants c1-c7, weighted by c0 (the dumped GLSL of both shaders), and
// writes every pixel: leaving the passes out leaves a black frame (a capture before this
// rule). With c1-c7 zero every tap reads the centre texel, so a pass copies the picture (scaled by its weights' sum).
#pragma once
#include <algorithm>
#include <cstdint>

namespace runtime::preview {

constexpr uint64_t kPauseBlurPixelShaders[] = {0x362b1c7e698a2e23ull, 0xf9daba5e30b16f65ull};

// The help bar's corner of the 1280x720 frame (guest pixels): the bottom right, where FM4 lists its buttons. OBSERVED on the
// GRAPHICS screen (captures at 4K): B: BACK TO GRAPHICS at about x
// 1037-1194, the bar's top line at y 653; the description panel's divider at x 995 reaches y 653, so the corner starts at x 1000.
struct Rect
{
    uint32_t x, y, width, height;
};
constexpr Rect kHint{1000, 632, 280, 88};

enum class Action
{
    Draw,     // as the game asks
    Skip,     // left out (nothing recorded)
    Clip,     // drawn inside kHint only
    Flatten,  // a pause blur pass, drawn with FlattenBlur's constants (a copy)
};

constexpr uint32_t kBlurOffsetFirstWord = 4, kBlurOffsetEndWord = 32;  // c1.x .. c7.w of the pixel shader's float constants

// `psWords`: the pixel shader's float constants (c0.x first). The tap offsets c1-c7 set to 0.0f (the word 0).
constexpr void FlattenBlur(uint32_t* psWords)
{
    for (uint32_t w = kBlurOffsetFirstWord; w < kBlurOffsetEndWord; w++)
        psWords[w] = 0;
}

constexpr bool IsPauseBlur(uint64_t pixelShader)
{
    for (const uint64_t h : kPauseBlurPixelShaders)
        if (h == pixelShader)
            return true;
    return false;
}

// `overlay`: extended_view.h IsOverlayDraw for the draw. A menu draw whose scissor misses the corner is skipped.
constexpr Action Classify(bool previewing, uint64_t pixelShader, bool overlay, uint32_t scissorX, uint32_t scissorY, uint32_t scissorWidth, uint32_t scissorHeight)
{
    if (!previewing)
        return Action::Draw;
    if (IsPauseBlur(pixelShader))
        return Action::Flatten;
    if (!overlay)
        return Action::Draw;
    const bool misses = scissorX >= kHint.x + kHint.width || scissorX + scissorWidth <= kHint.x || scissorY >= kHint.y + kHint.height ||
                        scissorY + scissorHeight <= kHint.y || scissorWidth == 0 || scissorHeight == 0;
    return misses ? Action::Skip : Action::Clip;
}

// The scissor narrowed to kHint (Classify returned Clip, so they overlap).
constexpr void ClipToHint(uint32_t& x, uint32_t& y, uint32_t& width, uint32_t& height)
{
    const uint32_t left = std::max(x, kHint.x), top = std::max(y, kHint.y);
    const uint32_t right = std::min(x + width, kHint.x + kHint.width), bottom = std::min(y + height, kHint.y + kHint.height);
    x = left, y = top, width = right > left ? right - left : 0, height = bottom > top ? bottom - top : 0;
}

}  // namespace runtime::preview
