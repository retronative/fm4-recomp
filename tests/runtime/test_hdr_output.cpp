// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// runtime/video/hdr_output.h (FM4_HDR; adapted from SpeedBreaker): the switches, the expansion curve, the frame-average limiter,
// the local budget and the surface-format choice (window_policy.h ChooseHdrSurfaceFormat). Expected values are hand-computed from
// the rules (IEC 61966-2-1 decode; white becomes the peak, v + (peak - 1) v^8; peak x frame average <= 1; a neighbourhood adds at
// most the budget), not taken from the code's output. argv[1]: a directory for the emitted compute shader (hdr.comp), which the
// runner passes through glslangValidator and spirv-val.
#include "hdr_output.h"
#include "window_policy.h"
#include <cstdio>
#include <fstream>

using namespace runtime::video::hdr;
namespace window = runtime::video::window;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", what);
    failures += ok ? 0 : 1;
}
static bool Near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

int main(int argc, char** argv)
{
    {
        bool on = true;
        std::string error;
        const bool off = Parse(nullptr, on, error) && !on && Parse("", on, error) && !on && Parse("off", on, error) && !on;
        const bool yes = Parse("on", on, error) && on && Parse("1", on, error) && on && Parse("auto", on, error) && on;
        const bool refused = !Parse("yes", on, error) && !on && !error.empty() && !Parse("0", on, error) && !Parse("ON", on, error);
        Expect(off && yes && refused, "FM4_HDR: unset, empty and off are off; auto, on and 1 ask for it; yes, 0 and ON are refused (CONTROL)");
        float v = 0;
        const bool number = ParseNumber("FM4_HDR_STRENGTH", nullptr, 1.5f, 4.0f, 2.0f, v, error) && v == 2.0f &&
                            ParseNumber("FM4_HDR_STRENGTH", "3", 1.5f, 4.0f, 2.0f, v, error) && v == 3.0f &&
                            ParseNumber("FM4_HDR_STRENGTH", "1.5", 1.5f, 4.0f, 2.0f, v, error) && v == 1.5f;
        const bool bad = !ParseNumber("FM4_HDR_STRENGTH", "5", 1.5f, 4.0f, 2.0f, v, error) && !ParseNumber("FM4_HDR_STRENGTH", "1", 1.5f, 4.0f, 2.0f, v, error) &&
                         !ParseNumber("FM4_HDR_STRENGTH", "2x", 1.5f, 4.0f, 2.0f, v, error) && !ParseNumber("FM4_HDR_STRENGTH", "nan", 1.5f, 4.0f, 2.0f, v, error);
        Expect(number && bad, "FM4_HDR_STRENGTH: default 2, 1.5 to 4 accepted; 5, 1, 2x and nan refused (CONTROL)");
    }
    // The peak: the headroom, at most the strength, never below 1 (no headroom: the standard picture).
    Expect(PeakForHeadroom(1.0f, 2.0f) == 1.0f && PeakForHeadroom(0.0f, 2.0f) == 1.0f && PeakForHeadroom(1.6f, 2.0f) == 1.6f && PeakForHeadroom(8.0f, 2.0f) == 2.0f &&
               PeakForHeadroom(8.0f, 4.0f) == 4.0f && PeakForHeadroom(8.0f, 9.0f) == 4.0f && PeakForHeadroom(NAN, 2.0f) == 1.0f,
           "peak: headroom 1 or less gives 1; 1.6 gives 1.6; 8 is capped by the strength (2, 4) and the strength by 4; not-a-number gives 1");
    // Decode: 0 -> 0, 1 -> 1, the linear segment 0.04045 / 12.92 = 0.0031308, and 0.5 -> ((0.5 + 0.055) / 1.055)^2.4 = 0.21404.
    Expect(SrgbToLinear(0.0f) == 0.0f && Near(SrgbToLinear(1.0f), 1.0f) && Near(SrgbToLinear(0.04045f), 0.0031308f, 1e-6f) && Near(SrgbToLinear(0.5f), 0.21404f) &&
               SrgbToLinear(-1.0f) == 0.0f && Near(SrgbToLinear(2.0f), 1.0f),
           "sRGB decode: 0, 1, the linear segment's end 0.0031308, 0.5 -> 0.21404; values outside 0..1 are clamped");
    // Expand: white becomes the peak; 0.5 at peak 4 adds 3 / 256 = 0.01172; peak 1 changes nothing.
    Expect(Near(Expand(1.0f, 4.0f), 4.0f) && Near(Expand(0.5f, 4.0f), 0.5f + 3.0f / 256.0f) && Expand(0.0f, 4.0f) == 0.0f && Near(Expand(0.7f, 1.0f), 0.7f) &&
               Near(Expand(0.7f, 0.5f), 0.7f) && Near(Expand(0.9f, 2.0f), 0.9f + 0.43046721f),
           "expand: white -> peak; 0.5 at 4x adds 3/256; 0.9 at 2x adds 0.9^8 = 0.43047; a peak of 1 or less is the standard picture (CONTROL)");
    // Frame limiter: a white frame keeps the peak at 1; a frame averaging 0.5 allows 2; a dark one the base.
    Expect(FramePeak(4.0f, 1.0f) == 1.0f && Near(FramePeak(4.0f, 0.5f), 2.0f) && FramePeak(4.0f, 0.1f) == 4.0f && FramePeak(4.0f, 0.0f) == 4.0f &&
               FramePeak(2.0f, 0.25f) == 2.0f && FramePeak(0.5f, 0.5f) == 1.0f,
           "frame limiter: peak x average <= 1 (white frame 1, average 0.5 -> 2), a dark frame keeps the base, never below 1");
    Expect(SmoothAverage(0.2f, 0.8f) == 0.8f && Near(SmoothAverage(0.8f, 0.2f), 0.8f - 0.6f * 0.15f) && SmoothAverage(0.3f, 0.3f) == 0.3f,
           "limiter state: a brighter frame at once, a darker one by 15 % of the gap");
    // Local budget 0.1: at peak 4 a neighbourhood of mean v^8 0.5 would add 1.5: scaled by 0.1 / 1.5; one of 0.02 adds 0.06: untouched.
    Expect(Near(LocalScale(4.0f, 0.5f, 0.1f), 0.1f / 1.5f) && LocalScale(4.0f, 0.02f, 0.1f) == 1.0f && LocalScale(4.0f, 0.5f, 0.0f) == 1.0f &&
               LocalScale(1.0f, 1.0f, 0.1f) == 1.0f,
           "local budget: a bright neighbourhood's added light is scaled to the budget; a dark one keeps the full peak; budget 0 is off; peak 1 adds nothing");
    {
        // Hue: grey 0.9 at 2x -> each channel 0.9 + 0.43047; a saturated colour keeps its channel ratios; black stays black.
        float r = 0.9f, g = 0.9f, b = 0.9f;
        ExpandRgb(r, g, b, 2.0f, 1.0f);
        const bool grey = Near(r, 1.33047f) && Near(g, r) && Near(b, r);
        float r2 = 1.0f, g2 = 0.5f, b2 = 0.0f;  // luma 0.2126 + 0.3576 = 0.5702
        ExpandRgb(r2, g2, b2, 4.0f, 1.0f);
        const float luma = 0.5702f, scale = (luma + 3.0f * std::pow(luma, 8.0f)) / luma;
        const bool colour = Near(r2, scale) && Near(g2, 0.5f * scale) && b2 == 0.0f && Near(r2 / g2, 2.0f);
        float r3 = 0.9f, g3 = 0.9f, b3 = 0.9f;
        ExpandRgb(r3, g3, b3, 2.0f, 0.5f);  // half the added light
        float k = 0.0f, k2 = 0.0f, k3 = 0.0f;
        ExpandRgb(k, k2, k3, 4.0f, 1.0f);
        Expect(grey && colour && Near(r3, 0.9f + 0.5f * 0.43046721f) && k == 0.0f && k2 == 0.0f && k3 == 0.0f,
               "expand by luma: grey 0.9 at 2x -> 1.33047 on each channel; (1, 0.5, 0) keeps its 2:1 ratio; a local scale of 0.5 halves the added light; black stays black");
    }
    {
        // The swapchain for HDR: exactly R16G16B16A16_SFLOAT in EXTENDED_SRGB_LINEAR. MoltenVK lists B8G8R8A8 in every colour space
        // (SpeedBreaker presenter.cpp:993-998): never that pair; no pair, or a device that cannot blit to the format: no HDR.
        const auto yes = [](VkFormat) { return true; };
        const auto no = [](VkFormat) { return false; };
        const VkSurfaceFormatKHR offered[] = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                              {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
                                              {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT},
                                              {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT}};
        const window::FormatChoice chosen = window::ChooseHdrSurfaceFormat(offered, yes);
        const bool exact = chosen.ok && chosen.format.format == VK_FORMAT_R16G16B16A16_SFLOAT && chosen.format.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
        const bool none = !window::ChooseHdrSurfaceFormat(std::span<const VkSurfaceFormatKHR>(offered, 3), yes).ok &&
                          !window::ChooseHdrSurfaceFormat(offered, no).ok && !window::ChooseHdrSurfaceFormat({}, yes).reason.empty();
        Expect(exact && none, "HDR surface format: only R16G16B16A16_SFLOAT with EXTENDED_SRGB_LINEAR; 8-bit in that colour space, float in another, "
                              "or a format the device cannot blit to are not HDR (CONTROL)");
    }
    {
        // The HUD's gain: the encoded value whose linear light L has L + (peak - 1) L^8 = 1. Peak 1: 1. Peak 2: L = 0.8116 (0.8116 + 0.8116^8
        // = 0.8116 + 0.1884 = 1), encoded 1.055 x 0.8116^(1/2.4) - 0.055 = 0.9121; the other peaks are checked through Expand.
        const float g2 = HudGain(2.0f), g4 = HudGain(4.0f), g15 = HudGain(1.5f);
        const bool one = Near(HudGain(1.0f), 1.0f, 1e-3f) && Near(HudGain(0.5f), 1.0f, 1e-3f);
        const bool two = Near(g2, 0.9121f, 5e-4f) && Near(Expand(SrgbToLinear(g2), 2.0f), 1.0f, 1e-4f);
        const bool others = Near(Expand(SrgbToLinear(g4), 4.0f), 1.0f, 1e-4f) && Near(Expand(SrgbToLinear(g15), 1.5f), 1.0f, 1e-4f) && g4 < g2 && g2 < g15 && g15 < 1.0f;
        const bool steps = HudGainPeak(1.0f) == 1.0f && HudGainPeak(1.98f) == 2.0f && HudGainPeak(1.6f) == 1.5f && HudGainPeak(1.63f) == 1.75f && HudGainPeak(9.0f) == 4.0f &&
                           HudGainPeak(1.1f) == 1.0f;
        Expect(one && two && others && steps, "HUD gain: 1 at a peak of 1; 0.9121 at 2x (its white expands to exactly standard white); smaller at higher peaks; "
                                              "the peak is taken in steps of a quarter (1.98 -> 2, 1.1 -> 1)");
    }
    static_assert(sizeof(Push) == 20, "the shader's push block: ivec2, int, float, float");
    Expect(Groups(1280) == 160 && Groups(720) == 90 && Groups(3840) == 480 && Groups(2160) == 270 && Groups(kCellsX) == 4 && Groups(kCellsY) == 3 && Groups(1281) == 161,
           "workgroups of 8: 1280x720 is 160x90, 3840x2160 480x270, the 32x18 cells 4x3, 1281 rounds up");
    if (argc > 1)
        std::ofstream(std::string(argv[1]) + "/hdr.comp") << Glsl();
    std::printf("hdr output tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
