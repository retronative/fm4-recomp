// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9, opt-in FM4_POST_AA=on; the app's Graphics panel, "Extra edge smoothing"): a post-process anti-aliasing pass over the
// display image, after the game's own MSAA resolve, at the display image's size (1280x720, or the internal resolution).
//
// Adapted from ReCarbon (GPL-3.0-or-later), runtime/video/presenter.cpp kAaCs at commit 66401ea: its FXAA-style compute pass
// (edge-directed, with a sub-pixel blend; the contrast thresholds, the edge walk's strides and the sub-pixel weight 0.5 are its).
// Our changes:
//  - the destination is the display image's own 10-bit format (rgb10_a2, not rgba8), and a texel's alpha is kept;
//  - the 3x3 neighbourhood is read with texelFetch (exact texels, clamped to the image), and a texel the pass does not move is
//    stored as fetched: what is not an edge stays bit for bit what the game drew (ReCarbon reads everything through the linear
//    sampler). Only the edge walk and the final blended read use the sampler;
//  - ReCarbon's 16:9 band clamp (its ultrawide strips) and its downscale gate are left out: FM4's picture is the whole image;
//  - #version 450 (the runtime compiles for Vulkan 1.0 / SPIR-V 1.0, shader_cache.cpp CompileCompute);
//  - a strength (FM4_POST_AA=soft | on | strong; the Graphics panel's Soft / Standard / Strong): the two contrast thresholds and the
//    sub-pixel weight are push constants. Standard (on) is ReCarbon's three values, unchanged. Soft and Strong are the neighbouring
//    tuning points of FXAA 3.11's quality settings (T. Lottes, Fxaa3_11.h: edge threshold 0.166 / 0.125 / 0.063, threshold minimum
//    0.0625 / 0.0312, sub-pixel 0.25 / 0.50 / 0.75; from the developer's memory of that header, which was not re-read for this
//    change: OBSERVED, not VERIFIED).
// The pass never touches EDRAM, the resolves or guest memory: the game's MSAA is as before, and with the switch off no object of
// this unit exists.
//
// Pure (no Vulkan): tests/runtime/test_post_aa.cpp.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace runtime::video::postaa {

constexpr uint32_t kGroup = 8;  // the compute shader's local size, both axes

// The pass's strength: no AA on contrast below max(edgeMin, edgeRelative * the local maximum luma); the sub-pixel blend's weight.
struct Tuning
{
    const char* name;
    float edgeMin, edgeRelative, subpixel;
};
constexpr Tuning kSoft{"soft", 0.0625f, 0.166f, 0.25f};        // fewer edges, thin lines and small text kept sharper
constexpr Tuning kStandard{"standard", 0.0312f, 0.125f, 0.5f};  // ReCarbon's
constexpr Tuning kStrong{"strong", 0.0312f, 0.063f, 0.75f};     // more edges, more sub-pixel blending (softer)

struct Push
{
    int32_t width, height;
    float subpixel, edgeMin, edgeRelative;
};
constexpr Push MakePush(uint32_t width, uint32_t height, const Tuning& t) { return {int32_t(width), int32_t(height), t.subpixel, t.edgeMin, t.edgeRelative}; }

// FM4_POST_AA: unset, empty, "0" or "off": no pass (the default); "1" or "on": the pass at Standard; "soft", "strong": the pass at
// that strength. Anything else: false with the spellings. `tuning` (optional) is set only when the pass is on.
inline bool Parse(const char* value, bool& on, std::string& error, Tuning* tuning = nullptr)
{
    if (value == nullptr || *value == '\0' || std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0)
        return on = false, true;
    const Tuning* chosen = std::strcmp(value, "1") == 0 || std::strcmp(value, "on") == 0 ? &kStandard
                           : std::strcmp(value, "soft") == 0 ? &kSoft
                           : std::strcmp(value, "strong") == 0 ? &kStrong
                                                               : nullptr;
    if (chosen == nullptr)
    {
        error = std::string("FM4_POST_AA '") + value + "': use on (or 1), soft, strong, or off (or 0, the default)";
        return false;
    }
    if (tuning != nullptr)
        *tuning = *chosen;
    return on = true, true;
}

// Workgroups along an edge of `pixels`.
constexpr uint32_t Groups(uint32_t pixels) { return (pixels + kGroup - 1) / kGroup; }

inline std::string Glsl()
{
    return R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgb10_a2) uniform writeonly image2D dst;
// edgeMin: no AA on darker contrast than this; edgeRelative: ... or than this fraction of the local maximum (post_aa.h Tuning)
layout(push_constant) uniform P { ivec2 size; float subpixel; float edgeMin; float edgeRelative; } p;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 at(vec2 uv) { return textureLod(src, uv, 0.0).rgb; }
vec3 texel3(ivec2 t) { return texelFetch(src, clamp(t, ivec2(0), p.size - 1), 0).rgb; }

void main()
{
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    if (pix.x >= p.size.x || pix.y >= p.size.y)
        return;
    vec2 texel = 1.0 / vec2(p.size);
    vec2 uv = (vec2(pix) + 0.5) * texel;
    vec4 center = texelFetch(src, pix, 0);
    float lM = luma(center.rgb);
    float lN = luma(texel3(pix + ivec2(0, -1))), lS = luma(texel3(pix + ivec2(0, 1)));
    float lW = luma(texel3(pix + ivec2(-1, 0))), lE = luma(texel3(pix + ivec2(1, 0)));
    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = lMax - lMin;
    if (range < max(p.edgeMin, lMax * p.edgeRelative))
    {
        imageStore(dst, pix, center);
        return;
    }
    float lNW = luma(texel3(pix + ivec2(-1, -1))), lSE = luma(texel3(pix + ivec2(1, 1)));
    float lNE = luma(texel3(pix + ivec2(1, -1))), lSW = luma(texel3(pix + ivec2(-1, 1)));

    // The edge's orientation: where luma changes most (across rows for a horizontal edge).
    float acrossRows = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    float acrossColumns = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    bool horizontal = acrossRows >= acrossColumns;

    // Which side of this pixel the edge is on: the steeper neighbour.
    float lBefore = horizontal ? lN : lW, lAfter = horizontal ? lS : lE;
    float gBefore = abs(lBefore - lM), gAfter = abs(lAfter - lM);
    bool before = gBefore >= gAfter;
    float threshold = 0.25 * max(gBefore, gAfter);
    float step = horizontal ? texel.y : texel.x;
    float edgeLuma = 0.5 * ((before ? lBefore : lAfter) + lM);
    if (before)
        step = -step;

    // Walk along the edge both ways until the luma leaves the edge's.
    vec2 onEdge = uv;
    if (horizontal) onEdge.y += 0.5 * step; else onEdge.x += 0.5 * step;
    vec2 along = horizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec2 uvA = onEdge - along, uvB = onEdge + along;
    float dA = luma(at(uvA)) - edgeLuma, dB = luma(at(uvB)) - edgeLuma;
    bool endA = abs(dA) >= threshold, endB = abs(dB) >= threshold;
    const float strides[10] = float[](1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
    for (int i = 0; i < 10 && !(endA && endB); i++)
    {
        if (!endA) { uvA -= along * strides[i]; dA = luma(at(uvA)) - edgeLuma; endA = abs(dA) >= threshold; }
        if (!endB) { uvB += along * strides[i]; dB = luma(at(uvB)) - edgeLuma; endB = abs(dB) >= threshold; }
    }
    float distA = horizontal ? uv.x - uvA.x : uv.y - uvA.y;
    float distB = horizontal ? uvB.x - uv.x : uvB.y - uv.y;
    bool nearerA = distA < distB;
    float offset = 0.5 - min(distA, distB) / (distA + distB);
    // Only blend from the end whose luma variation goes the way this pixel's does (otherwise this pixel is on the edge's far side).
    if (((nearerA ? dA : dB) < 0.0) == (lM < edgeLuma))
        offset = 0.0;

    // Sub-pixel aliasing (thin lines, single-pixel features).
    float average = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
    float sub = clamp(abs(average - lM) / range, 0.0, 1.0);
    sub = (3.0 - 2.0 * sub) * sub * sub;
    offset = max(offset, sub * sub * p.subpixel);
    if (offset <= 0.0)
    {
        imageStore(dst, pix, center);
        return;
    }
    vec2 sampleUv = uv;
    if (horizontal) sampleUv.y += offset * step; else sampleUv.x += offset * step;
    imageStore(dst, pix, vec4(at(sampleUv), center.a));
}
)";
}

}  // namespace runtime::video::postaa
