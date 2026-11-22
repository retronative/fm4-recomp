// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// runtime/video/post_aa.h (M9, FM4_POST_AA): the fail-closed switch, the dispatch's workgroup counts (hand values for the four
// display sizes) and the emitted shader's text (argv[1]/post_aa.comp, which the runner compiles and validates).
#include "post_aa.h"

#include <cstdio>
#include <string>

namespace aa = runtime::video::postaa;

static int failures = 0, checks = 0;
static void Check(bool ok, const char* what)
{
    checks++;
    if (!ok)
        failures++, std::printf("FAIL %s\n", what);
}

int main(int argc, char** argv)
{
    // The switch: off unless asked; a refused spelling names itself and leaves `on` alone.
    bool on = true;
    std::string error;
    Check(aa::Parse(nullptr, on, error) && !on, "unset is off");
    for (const char* v : {"", "0", "off"})
        on = true, Check(aa::Parse(v, on, error) && !on, "an off spelling is off");
    for (const char* v : {"1", "on"})
        on = false, Check(aa::Parse(v, on, error) && on, "an on spelling is on");
    // The strength. Standard is ReCarbon's three values (presenter.cpp kAaCs at 66401ea: kEdgeMin 0.0312, kEdgeRelative 0.125,
    // sub-pixel 0.5); Soft and Strong are the neighbouring points of FXAA 3.11's tuning range (post_aa.h).
    const auto tuned = [&](const char* v, float edgeMin, float edgeRelative, float subpixel, const char* name) {
        aa::Tuning t{"unset", -1.0f, -1.0f, -1.0f};
        on = false;
        return aa::Parse(v, on, error, &t) && on && t.edgeMin == edgeMin && t.edgeRelative == edgeRelative && t.subpixel == subpixel &&
               std::string(t.name) == name;
    };
    Check(tuned("on", 0.0312f, 0.125f, 0.5f, "standard") && tuned("1", 0.0312f, 0.125f, 0.5f, "standard"), "on is Standard: ReCarbon's values");
    Check(tuned("soft", 0.0625f, 0.166f, 0.25f, "soft"), "soft: higher thresholds, a quarter sub-pixel weight");
    Check(tuned("strong", 0.0312f, 0.063f, 0.75f, "strong"), "strong: half the relative threshold, three quarters sub-pixel weight");
    Check(aa::kSoft.edgeRelative > aa::kStandard.edgeRelative && aa::kStandard.edgeRelative > aa::kStrong.edgeRelative &&
              aa::kSoft.subpixel < aa::kStandard.subpixel && aa::kStandard.subpixel < aa::kStrong.subpixel && aa::kSoft.edgeMin >= aa::kStandard.edgeMin,
          "the strengths are ordered: soft touches the fewest edges and blends least");
    {
        aa::Tuning t{"kept", 1.0f, 2.0f, 3.0f};
        on = true;
        Check(aa::Parse("off", on, error, &t) && !on && std::string(t.name) == "kept" && t.edgeMin == 1.0f, "off leaves the strength alone");
        on = false;
        Check(!aa::Parse("medium", on, error, &t) && !on && std::string(t.name) == "kept" && t.subpixel == 3.0f, "CONTROL: a refused strength changes nothing");
    }
    for (const char* v : {"2", "ON", "true", "fxaa", "1 ", "on\n", "00", "standard", "Soft", "STRONG", "medium", "soft "})
    {
        on = false, error.clear();
        Check(!aa::Parse(v, on, error) && !on && error.find("FM4_POST_AA") != std::string::npos && error.find(v) != std::string::npos,
              "any other spelling is refused, named, and does not turn the pass on");
    }
    // Workgroups of 8: every pixel covered, never a whole group past the edge (1280x720, 1920x1080, 2560x1440, 3840x2160; odd sizes).
    Check(aa::Groups(1280) == 160 && aa::Groups(720) == 90, "720p: 160 x 90 groups");
    Check(aa::Groups(1920) == 240 && aa::Groups(1080) == 135, "FHD: 240 x 135 groups");
    Check(aa::Groups(2560) == 320 && aa::Groups(1440) == 180, "QHD: 320 x 180 groups");
    Check(aa::Groups(3840) == 480 && aa::Groups(2160) == 270, "4K: 480 x 270 groups");
    Check(aa::Groups(1) == 1 && aa::Groups(8) == 1 && aa::Groups(9) == 2 && aa::Groups(0) == 0, "partial groups round up");
    Check(sizeof(aa::Push) == 20, "the push constants are ivec2 + three floats");
    {
        const aa::Push push = aa::MakePush(2560, 1440, aa::kStrong);
        Check(push.width == 2560 && push.height == 1440 && push.subpixel == 0.75f && push.edgeMin == 0.0312f && push.edgeRelative == 0.063f,
              "the push constants carry the size and the strength, in the shader block's order");
    }
    // The shader's text: the display image's 10-bit format, the exact centre texel, the group size the dispatch assumes.
    const std::string glsl = aa::Glsl();
    Check(glsl.find("rgb10_a2") != std::string::npos && glsl.find("rgba8") == std::string::npos, "the destination is rgb10_a2");
    Check(glsl.find("local_size_x = 8, local_size_y = 8") != std::string::npos && aa::kGroup == 8, "the local size is the dispatch's group");
    Check(glsl.find("vec4 center = texelFetch(src, pix, 0);") != std::string::npos && glsl.find("imageStore(dst, pix, center);") != std::string::npos,
          "a texel that is not moved is stored as fetched");
    Check(glsl.find("center.a") != std::string::npos, "a blended texel keeps its alpha");
    Check(glsl.find("uniform P { ivec2 size; float subpixel; float edgeMin; float edgeRelative; } p;") != std::string::npos &&
              glsl.find("if (range < max(p.edgeMin, lMax * p.edgeRelative))") != std::string::npos && glsl.find("kEdgeMin") == std::string::npos,
          "the thresholds come from the push constants, in Push's order");
    if (argc > 1)
    {
        const std::string path = std::string(argv[1]) + "/post_aa.comp";
        FILE* f = std::fopen(path.c_str(), "wb");
        Check(f != nullptr && std::fwrite(glsl.data(), 1, glsl.size(), f) == glsl.size() && std::fclose(f) == 0, "the shader text is written");
    }
    std::printf("post aa: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
