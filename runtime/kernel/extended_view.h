// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9, Extended View, off by default): a wider field of view on a window wider than 16:9. The game still renders its
// 1280x720 frame; the main 3D view's aspect ratio is multiplied by a factor (a title hook on the game's viewport aspect getter,
// runtime/game/extended_view.cpp: the projection and the culling frustum both read the camera's aspect, so they change together),
// the picture is stretched horizontally by the same factor when presented, and the draws of the HUD and menus are narrowed by it
// so they stay a centred 16:9 block. The design is ReCarbon's and SpeedBreaker's (runtime/game/ultrawide.h, runtime/video/
// picture_fit.h at 66401ea / ea7311a, GPL-3.0-or-later: the guest frame stays 16:9, Hor+ factor = aspect / (16/9) beyond a small
// margin); nothing of their title hooks is used. Pure: no SDL, no Vulkan, no guest memory (tests/runtime/test_extended_view.cpp).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace runtime::view {

constexpr float kBaseAspect = 16.0f / 9.0f;
constexpr float kMaxFactor = 2.0f;     // 32:9
constexpr float kMinFactor = 0.75f;    // 4:3 (ReCarbon's kTallMinAspect: a narrower window shows a 4:3 picture with bars)
constexpr float kMargin = 1.01f;       // a window within 1 % of 16:9 is 16:9 (a title bar's rounding)

// The factor for a window of `width` x `height` pixels: the picture's aspect over 16:9 (ReCarbon / SpeedBreaker
// runtime/video/picture_fit.h FitPicture's policy). Above 1 beyond the margin: a wider view, the same height (Hor+), at most 2
// (32:9). Below 1 beyond the margin (16:10, 3:2, 4:3): the same width of view with more above and below (Vert+), at least 0.75
// (4:3). 1 within the margin: the 16:9 picture as ever.
inline float FactorForWindow(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return 1.0f;
    const float factor = (float(width) / float(height)) / kBaseAspect;
    return factor > kMargin ? std::min(factor, kMaxFactor) : factor < 1.0f / kMargin ? std::max(factor, kMinFactor) : 1.0f;
}
// The two stretches of a factor: the frame is shown `Wide` times wider or `Tall` times taller than 16:9 (one of them is 1).
inline float Wide(float factor) { return factor > 1.0f ? factor : 1.0f; }
inline float Tall(float factor) { return factor < 1.0f ? 1.0f / factor : 1.0f; }
// The main view for a factor. Its aspect ratio (width / height) is multiplied by the factor; its vertical field of view (radians)
// grows only for a taller picture, tan(fov' / 2) = tan(fov / 2) x Tall, so the horizontal view stays what the game chose
// (projection [0][0] = cot(fov / 2) / aspect is unchanged: cot / Tall over aspect / Tall).
inline float ViewAspect(float aspect, float factor) { return aspect * factor; }
inline float ViewFov(float fov, float factor) { return factor < 1.0f ? 2.0f * std::atan(std::tan(fov * 0.5f) * Tall(factor)) : fov; }

// Where a picture rendered with `factor` goes in a window: the largest centred rectangle of aspect factor x 16:9 (factor 1: the
// letterbox of window_policy.h FitRect).
struct Placement
{
    int32_t x0, y0, x1, y1;
};
inline Placement PlaceStretched(uint32_t windowWidth, uint32_t windowHeight, float factor)
{
    const double aspect = 16.0 / 9.0 * double(std::clamp(factor, kMinFactor, kMaxFactor));
    double w = double(windowWidth), h = w / aspect;
    if (h > double(windowHeight))
        h = double(windowHeight), w = h * aspect;
    const int32_t rw = int32_t(std::lround(w)), rh = int32_t(std::lround(h));
    const int32_t x0 = (int32_t(windowWidth) - rw) / 2, y0 = (int32_t(windowHeight) - rh) / 2;
    return {x0, y0, x0 + rw, y0 + rh};
}

// A HUD vertex's coordinate along a stretched axis, in the frame's own space (-1 one edge, 1 the other): narrowed about the centre
// by that axis's stretch, except a vertex at an edge, which stays there: a full-screen dim, fade or bar still covers the whole
// picture once it is stretched (OBSERVED: narrowed as a whole, the Pause menu's dim left the side strips bright,
// p1_pause_sheet.png). The vertex shader's XE_OVERLAY_PIN variant applies the same rule to x by Wide and to y by Tall, for the
// draws whose viewport is the whole frame (runtime/shader/shader_glsl.cpp); a draw in a viewport of its own (the track map) is
// moved and narrowed as a whole (NarrowAbout).
constexpr float kEdge = 0.998f;
inline float PinNarrow(float v, float stretch)
{
    return std::fabs(v) >= kEdge ? (v < 0.0f ? -1.0f : 1.0f) : v / stretch;
}
// A small viewport's clip-space scale and offset along an axis whose frame centre is `centre`: narrowed about it.
inline void NarrowAbout(float& scale, float& offset, float centre, float stretch)
{
    scale /= stretch, offset = centre + (offset - centre) / stretch;
}
// A HUD draw's scissor along an axis, the frame covering [frameStart, frameStart + frameSize) of the target, by the same rule: an
// edge at the frame's edge (or beyond) stays, another is narrowed about the frame's centre (outward rounding).
inline void NarrowScissor(uint32_t& start, uint32_t& size, uint32_t frameStart, uint32_t frameSize, float stretch)
{
    const double centre = double(frameStart) + double(frameSize) / 2.0, end = double(frameStart + frameSize);
    const double low = start <= frameStart ? double(start) : centre + (double(start) - centre) / double(stretch);
    const double high = double(start + size) >= end ? double(start + size) : centre + (double(start + size) - centre) / double(stretch);
    const uint32_t a = uint32_t(std::max(std::floor(low), 0.0)), b = uint32_t(std::max(std::ceil(high), 0.0));
    start = a, size = b > a ? b - a : 0;
}

// Which draws are the HUD and the menus. OBSERVED in FM4's frames (research/m9/ultrawide/RESULTS.md: a race frame, a menu with a
// 3D scene and a 2D screen): the 3D scene is drawn into a floating-point target and reaches the final picture, a 1280-wide
// k_2_10_10_10 target (colour format 2), as rectangle lists without blending (the tone-mapped copy, one per tile); everything
// else drawn into that target is the game's 2D UI (triangle lists, strips and fans with blending: text, gauges, the track map in
// its own small viewport, panels). So: a draw into such a target that is not an unblended rectangle list.
constexpr uint32_t kFinalColourFormat = 2, kRectangleList = 8, kFrameWidth = 1280;
constexpr bool IsOverlayDraw(uint32_t colourFormat, uint32_t targetWidth, uint32_t primitiveType, bool blending)  // targetWidth: the surface's pitch
{
    return colourFormat == kFinalColourFormat && targetWidth == kFrameWidth && !(primitiveType == kRectangleList && !blending);
}

// The shared state. The preference (the GRAPHICS menu, the Settings panel) and the window's size (the window's thread) give the
// wanted factor; the command processor latches it at each presented swap (Latch), and from then the game's hook, the HUD
// narrowing and the frame's presentation use the latched value, so a change takes effect between frames.
class State
{
public:
    void SetPreference(bool on) { wanted_.store(on, std::memory_order_relaxed); }
    bool Preference() const { return wanted_.load(std::memory_order_relaxed); }
    void SetWindow(uint32_t width, uint32_t height) { window_.store(FactorForWindow(width, height), std::memory_order_relaxed); }
    // FM4_DIAG_ASPECT_FACTOR (diagnostic): a factor without a window.
    void Force(float factor) { forced_.store(std::clamp(factor, kMinFactor, kMaxFactor), std::memory_order_relaxed); }
    float Wanted() const
    {
        const float forced = forced_.load(std::memory_order_relaxed);
        if (forced != 0.0f)
            return forced;
        return wanted_.load(std::memory_order_relaxed) ? window_.load(std::memory_order_relaxed) : 1.0f;
    }
    // The command processor, once per presented swap. True when the factor changed.
    bool Latch()
    {
        const float next = Wanted();
        return latched_.exchange(next, std::memory_order_relaxed) != next;
    }
    float Factor() const { return latched_.load(std::memory_order_relaxed); }

    // The game's hook widened a main view at `vblank` (the 60 Hz count): frames without one for half a second (movies, loading,
    // menus without a 3D scene) are 16:9, as the other FM4 port does (hl360-forza4 doc/wide-screens.md at d6597d7; the idea only).
    void NoteWidened(uint64_t vblank) { widenedAt_.store(vblank, std::memory_order_relaxed); }
    bool Widening(uint64_t vblank) const
    {
        const uint64_t at = widenedAt_.load(std::memory_order_relaxed);
        return at != 0 && vblank - at <= 30;
    }
    // The factor a frame is drawn and shown with now: the latched one while the game widens its main view, 1 otherwise.
    float Effective(uint64_t vblank) const { return Widening(vblank) ? Factor() : 1.0f; }

private:
    std::atomic<bool> wanted_{false};
    std::atomic<float> window_{1.0f}, forced_{0.0f}, latched_{1.0f};
    std::atomic<uint64_t> widenedAt_{0};
};

inline State& Global()
{
    static State state;
    return state;
}

}  // namespace runtime::view
