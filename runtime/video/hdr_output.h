// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SpeedBreaker / ReCarbon contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9, opt-in FM4_HDR=1, off by default): HDR output on a display with extended dynamic range (macOS EDR). The game's
// finished picture (standard range, display-referred after the game's own tone mapping and gamma ramp) is decoded to linear light
// and its highlights are expanded into the display's headroom; nothing below the highlights moves, and with no headroom the
// picture is the standard one.
// Adapted from SpeedBreaker runtime/video/hdr_output.h at ea7311a (GPL-3.0-or-later): PeakForHeadroom, Expand, SrgbToLinear,
// FramePeak, SmoothAverage, LocalScale, Luma709 and ExpandRgb, and the design of its compute pass (a 32x18 grid of cell means,
// the frame-average limiter and the local budget). FM4 Recomp changes: the namespace; one compute shader with three steps over one
// 16-bit float image, in place; the cell field interpolated between cell centres; no UI blending (the game's HUD is part of the
// picture: its white expands like any highlight, a known gap) and no HDR10/PQ (macOS only); the fail-closed switches.
// Pure: no Vulkan call (tests/runtime/test_hdr_output.cpp; the shader goes through glslangValidator and spirv-val there).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace runtime::video::hdr {

constexpr float kStrengthDefault = 2.0f, kStrengthMin = 1.5f, kStrengthMax = 4.0f;
constexpr float kAverageRelease = 0.15f;  // a darker frame eases the limiter back by this share of the gap a frame
constexpr float kLocalBudget = 0.1f;      // the light a neighbourhood may add, in SDR white
constexpr uint32_t kCellsX = 32, kCellsY = 18, kCells = kCellsX * kCellsY;
constexpr uint32_t kGroup = 8;
constexpr uint32_t Groups(uint32_t pixels) { return (pixels + kGroup - 1) / kGroup; }

// FM4_HDR: unset, "" or "off" is off; "auto" (graphics.conf's spelling: HDR where the display and system can show it), "on" or "1" asks
// for it, which is the same thing (it is never forced); anything else is refused (the caller stops).
inline bool Parse(const char* value, bool& on, std::string& error)
{
    on = false;
    if (value == nullptr || *value == '\0' || std::strcmp(value, "off") == 0)
        return true;
    if (std::strcmp(value, "auto") == 0 || std::strcmp(value, "on") == 0 || std::strcmp(value, "1") == 0)
        return on = true;
    error = std::string("FM4_HDR '") + value + "' is not off, auto, on or 1";
    return false;
}

// FM4_HDR_STRENGTH: the most the picture's white may reach, in multiples of SDR white: 1.5 to 4, default 2.
// FM4_HDR_HEADROOM (test only): a headroom assumed instead of the display's, 1 to 16.
inline bool ParseNumber(const char* name, const char* value, float low, float high, float fallback, float& out, std::string& error)
{
    out = fallback;
    if (value == nullptr || *value == '\0')
        return true;
    char* end = nullptr;
    const float v = std::strtof(value, &end);
    if (end == value || *end != '\0' || !(v >= low && v <= high))
    {
        error = std::string(name) + " '" + value + "' is not a number from " + std::to_string(low) + " to " + std::to_string(high);
        return false;
    }
    out = v;
    return true;
}

// The brightest value the picture may use, in multiples of SDR white: the display's current headroom (1: no EDR, the picture is
// left alone), at most the strength.
inline float PeakForHeadroom(float headroom, float strength)
{
    return std::clamp(std::isfinite(headroom) ? headroom : 1.0f, 1.0f, std::clamp(strength, 1.0f, kStrengthMax));
}

// v: an SDR value in linear light (0 black, 1 SDR white). SDR white becomes `peak`; the curve is steep (v^8), so shadows and
// mid-tones stay where the SDR picture put them and only bright highlights reach the headroom.
inline float Expand(float v, float peak)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v + (std::max(peak, 1.0f) - 1.0f) * std::pow(v, 8.0f);
}

// IEC 61966-2-1: the display image's values as the standard-range path shows them (a UNORM swapchain in SRGB_NONLINEAR).
inline float SrgbToLinear(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// The peak for a frame whose SDR luma averages `average` (0..1): peak * average stays at SDR white, so a full-white frame (a
// loading screen, a white menu) cannot become a full-screen flash. Darker frames keep `base`.
inline float FramePeak(float base, float average)
{
    base = std::max(base, 1.0f);
    average = std::clamp(average, 0.0f, 1.0f);
    if (average <= 0.0f)
        return base;
    return std::clamp(std::min(base, 1.0f / average), 1.0f, base);
}

// The limiter's frame-to-frame state: a brighter frame takes effect at once, a darker one eases back, so the peak does not pump.
inline float SmoothAverage(float previous, float average)
{
    return average > previous ? average : previous + (average - previous) * kAverageRelease;
}

// The local limit: Expand adds (peak - 1) * v^8; where a neighbourhood's mean v^8 would add more than `budget`, a pixel's added
// light is scaled down so a large near-white area (sky, smoke, a white wall) does not become a flat glowing blob. Small highlights
// in dark surroundings keep the full peak.
inline float LocalScale(float peak, float neighbourhoodV8, float budget)
{
    const float added = (std::max(peak, 1.0f) - 1.0f) * std::max(neighbourhoodV8, 0.0f);
    if (budget <= 0.0f || added <= budget)
        return 1.0f;
    return budget / added;
}

// The HUD under HDR (there is no mask of the HUD in the finished picture, so its white would be expanded to the peak like any
// highlight; OBSERVED in play: the HUD far too white). The renderer knows which draws are the HUD
// (kernel/extended_view.h IsOverlayDraw) and scales their colour by this gain: the encoded value whose linear light L satisfies
// L + (peak - 1) L^8 = 1, so a white HUD pixel is exactly standard white after Expand. 1 at a peak of 1.
inline float HudGain(float peak)
{
    peak = std::clamp(peak, 1.0f, kStrengthMax);
    float low = 0.0f, high = 1.0f;
    for (int i = 0; i < 40; i++)
    {
        const float mid = 0.5f * (low + high);
        (Expand(mid, peak) < 1.0f ? low : high) = mid;
    }
    const float linear = 0.5f * (low + high);
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;  // IEC 61966-2-1 encode
}
// The peak the gain is taken for: the base peak in steps of a quarter (a shader variant per step, not per frame).
inline float HudGainPeak(float peak) { return std::round(std::clamp(peak, 1.0f, kStrengthMax) * 4.0f) / 4.0f; }

inline float Luma709(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// Expands a linear RGB triplet by its Rec.709 luma (the ratio between the channels stays: hue and saturation do not move), the
// added light scaled by `local` (LocalScale).
inline void ExpandRgb(float& r, float& g, float& b, float peak, float local)
{
    r = std::clamp(r, 0.0f, 1.0f), g = std::clamp(g, 0.0f, 1.0f), b = std::clamp(b, 0.0f, 1.0f);
    const float luma = Luma709(r, g, b);
    if (luma <= 0.0f)
        return;
    const float scale = (luma + (Expand(luma, peak) - luma) * local) / luma;
    r *= scale, g *= scale, b *= scale;
}

// The pass's push constants. step 0: the cells (dispatch Groups(kCellsX) x Groups(kCellsY)); step 1: the frame's peak and the
// neighbourhood field (dispatch 1 x 1); step 2: the picture (dispatch Groups(width) x Groups(height)).
struct Push
{
    int32_t width, height, step;
    float peak, budget;
};

// One image (rgba16f, binding 0): on entry the display image's encoded values (blitted in), on exit linear light with 1.0 = SDR
// white. One buffer (binding 1): the limiter's state and the cell field; its first float is zero before the first frame.
inline std::string Glsl()
{
    return R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba16f) uniform image2D img;
layout(set = 0, binding = 1, std430) buffer State
{
    float average;        // the limiter's smoothed frame average (SmoothAverage)
    float framePeak;      // this frame's peak (FramePeak)
    float pad0, pad1;
    vec2 cell[576];       // per cell: mean linear luma, mean luma^8
    float field[576];     // per cell: the mean luma^8 of the 3x3 cells around it
} s;
layout(push_constant) uniform P { ivec2 size; int step; float peak; float budget; } p;
const ivec2 kCells = ivec2(32, 18);

float decode(float v) { v = clamp(v, 0.0, 1.0); return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
vec3 linear(ivec2 at) { vec3 c = imageLoad(img, clamp(at, ivec2(0), p.size - 1)).rgb; return vec3(decode(c.r), decode(c.g), decode(c.b)); }
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float fieldAt(ivec2 c) { c = clamp(c, ivec2(0), kCells - 1); return s.field[c.y * kCells.x + c.x]; }

void main()
{
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (p.step == 0)
    {
        // A cell's means from an 8x8 grid of its pixels.
        if (id.x >= kCells.x || id.y >= kCells.y)
            return;
        vec2 origin = vec2(id) * vec2(p.size) / vec2(kCells), extent = vec2(p.size) / vec2(kCells);
        float sum = 0.0, sum8 = 0.0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
            {
                float l = luma(linear(ivec2(origin + (vec2(x, y) + 0.5) * extent / 8.0)));
                float l2 = l * l, l4 = l2 * l2;
                sum += l, sum8 += l4 * l4;
            }
        s.cell[id.y * kCells.x + id.x] = vec2(sum, sum8) / 64.0;
    }
    else if (p.step == 1)
    {
        if (id != ivec2(0))
            return;
        float sum = 0.0;
        for (int i = 0; i < 576; i++)
            sum += s.cell[i].x;
        float average = clamp(sum / 576.0, 0.0, 1.0);
        float previous = clamp(s.average, 0.0, 1.0);
        average = average > previous ? average : previous + (average - previous) * 0.15;
        s.average = average;
        float base = max(p.peak, 1.0);
        s.framePeak = average <= 0.0 ? base : clamp(min(base, 1.0 / average), 1.0, base);
        for (int cy = 0; cy < kCells.y; cy++)
            for (int cx = 0; cx < kCells.x; cx++)
            {
                float v8 = 0.0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        ivec2 n = clamp(ivec2(cx + dx, cy + dy), ivec2(0), kCells - 1);
                        v8 += s.cell[n.y * kCells.x + n.x].y;
                    }
                s.field[cy * kCells.x + cx] = v8 / 9.0;
            }
    }
    else
    {
        if (id.x >= p.size.x || id.y >= p.size.y)
            return;
        vec3 c = linear(id);
        float l = luma(c);
        if (l > 0.0)
        {
            // The neighbourhood field between the four nearest cell centres.
            vec2 at = (vec2(id) + 0.5) * vec2(kCells) / vec2(p.size) - 0.5;
            ivec2 c0 = ivec2(floor(at));
            vec2 f = at - vec2(c0);
            float v8 = mix(mix(fieldAt(c0), fieldAt(c0 + ivec2(1, 0)), f.x), mix(fieldAt(c0 + ivec2(0, 1)), fieldAt(c0 + ivec2(1, 1)), f.x), f.y);
            float peak = max(s.framePeak, 1.0);
            float neighbourhood = (peak - 1.0) * max(v8, 0.0);
            float local = p.budget <= 0.0 || neighbourhood <= p.budget ? 1.0 : p.budget / neighbourhood;
            float l2 = l * l, l4 = l2 * l2;
            c *= (l + (peak - 1.0) * l4 * l4 * local) / l;
        }
        imageStore(img, id, vec4(c, 1.0));
    }
}
)";
}

}  // namespace runtime::video::hdr
