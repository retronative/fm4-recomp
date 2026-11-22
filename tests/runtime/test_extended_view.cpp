// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// runtime/kernel/extended_view.h (Extended View): the factor for a window, where a stretched frame goes, the narrowing of a HUD
// draw, which draws are the HUD, and the shared state's latch. Expected values are hand-computed: a 21:9 panel 3440x1440 is
// (3440/1440) / (16/9) = 1.34375; 2560x1080 is 1.33333; 32:9 5120x1440 is 2; the HUD rule is FM4's observed frame structure
// (research/m9/ultrawide/RESULTS.md), written out here as cases.
#include "extended_view.h"
#include <cstdio>

using namespace runtime::view;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", what);
    failures += ok ? 0 : 1;
}
static bool Near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance; }

int main()
{
    Expect(FactorForWindow(1920, 1080) == 1.0f && FactorForWindow(1280, 720) == 1.0f && FactorForWindow(0, 720) == 1.0f && FactorForWindow(1280, 0) == 1.0f &&
               FactorForWindow(1290, 720) == 1.0f && FactorForWindow(1270, 720) == 1.0f,
           "factor: a 16:9 window, an empty one, and one within 1 % of 16:9 either way are 1 (CONTROL)");
    Expect(Near(FactorForWindow(3440, 1440), 1.34375f) && Near(FactorForWindow(2560, 1080), 1.3333333f) && Near(FactorForWindow(5120, 1440), 2.0f) &&
               FactorForWindow(7680, 1440) == 2.0f && Near(FactorForWindow(1400, 720), 1.09375f),
           "factor, wider: 3440x1440 is 1.34375, 2560x1080 1.33333, 5120x1440 2; wider than 32:9 stays 2; 1400x720 is 1.09375");
    // Narrower than 16:9: 16:10 is (16/10) / (16/9) = 0.9; the owner's 2940x1846 display 1.592633 / 1.777778 = 0.895856; 3:2 is 0.84375; 4:3 is 0.75, and
    // nothing goes below it (5:4, a portrait window).
    Expect(Near(FactorForWindow(1920, 1200), 0.9f) && Near(FactorForWindow(2940, 1846), 0.895856f, 1e-5f) && Near(FactorForWindow(2160, 1440), 0.84375f) &&
               Near(FactorForWindow(1024, 768), 0.75f) && FactorForWindow(1280, 1024) == 0.75f && FactorForWindow(720, 1280) == 0.75f,
           "factor, taller: 16:10 is 0.9, 2940x1846 0.8958, 3:2 0.84375, 4:3 0.75; narrower than 4:3 stays 0.75");
    Expect(Wide(1.34375f) == 1.34375f && Tall(1.34375f) == 1.0f && Wide(0.9f) == 1.0f && Near(Tall(0.9f), 1.1111111f) && Wide(1.0f) == 1.0f && Tall(1.0f) == 1.0f,
           "stretches: a factor above 1 is a horizontal stretch, below 1 a vertical one of its inverse; never both");
    {
        // The view. Wider: the aspect grows, the field of view stays. Taller (0.9): the aspect 1.7778 becomes 1.6 and a vertical
        // field of view of 60 degrees (1.0471976 rad) becomes 2 atan(tan(30 deg) / 0.9) = 2 atan(0.6415003) = 65.36 degrees
        // (1.1407 rad); cot(fov / 2) / aspect is the same before and after (the horizontal view does not move).
        const float fov = 1.0471976f, tall = ViewFov(fov, 0.9f);
        const float before = (1.0f / std::tan(fov * 0.5f)) / 1.7777778f, after = (1.0f / std::tan(tall * 0.5f)) / ViewAspect(1.7777778f, 0.9f);
        Expect(Near(ViewAspect(1.7777778f, 1.3125f), 2.3333333f) && ViewFov(fov, 1.3125f) == fov && ViewFov(fov, 1.0f) == fov && Near(ViewAspect(1.7777778f, 0.9f), 1.6f) &&
                   Near(tall, 1.14074f, 2e-4f) && Near(before, after, 1e-5f),
               "view: wider multiplies the aspect and keeps the field of view; taller (0.9) makes the aspect 1.6 and 60 degrees 65.36, with the "
               "horizontal scale cot(fov / 2) / aspect unchanged");
    }
    {
        // A 21:9 frame in a 21:9 window fills it; in a 32:9 window it is centred with bars; a 32:9 frame in a 21:9 window is
        // letterboxed; factor 1 in a 21:9 window is the 16:9 pillarbox (2560 wide in 3440). A 16:10 frame (0.9) fills a 16:10
        // window; in a 16:9 window it is pillarboxed (1728 wide in 1920).
        const Placement fill = PlaceStretched(3440, 1440, 1.34375f), bars = PlaceStretched(5120, 1440, 1.34375f), tall = PlaceStretched(3440, 1440, 2.0f),
                        plain = PlaceStretched(3440, 1440, 1.0f), most = PlaceStretched(7680, 1440, 2.0f), mac = PlaceStretched(1920, 1200, 0.9f),
                        macIn169 = PlaceStretched(1920, 1080, 0.9f);
        Expect(fill.x0 == 0 && fill.y0 == 0 && fill.x1 == 3440 && fill.y1 == 1440 && bars.x0 == 840 && bars.x1 == 4280 && bars.y0 == 0 && bars.y1 == 1440 &&
                   tall.x0 == 0 && tall.x1 == 3440 && tall.y1 - tall.y0 == 968 && tall.y0 == 236 && plain.x0 == 440 && plain.x1 == 3000 && plain.y1 == 1440 &&
                   most.x0 == 1280 && most.x1 == 6400 && mac.x0 == 0 && mac.y0 == 0 && mac.x1 == 1920 && mac.y1 == 1200 && macIn169.x0 == 96 && macIn169.x1 == 1824 &&
                   macIn169.y0 == 0 && macIn169.y1 == 1080,
               "placement: a frame's aspect is its factor x 16:9, the largest centred rectangle of that shape (16:10 fills 1920x1200); factor 1 is the "
               "16:9 pillarbox; beyond 32:9 bars at the sides");
    }
    {
        // A HUD vertex at 0.5 of the half-extent with a stretch of 1.34375 moves to 0.37209; at -0.8977 to -0.66805; a vertex at the
        // frame's edge (a full-screen dim's corners, at -1, 1, or within 0.002 of them) stays at the edge.
        Expect(Near(PinNarrow(0.5f, 1.34375f), 0.3720930f) && Near(PinNarrow(-0.8977f, 1.34375f), -0.6680558f) && PinNarrow(1.0f, 1.34375f) == 1.0f &&
                   PinNarrow(-1.0f, 2.0f) == -1.0f && PinNarrow(0.9985f, 2.0f) == 1.0f && Near(PinNarrow(0.99f, 2.0f), 0.495f) && PinNarrow(0.0f, 2.0f) == 0.0f &&
                   PinNarrow(1.5f, 2.0f) == 1.0f && Near(PinNarrow(0.5f, 1.0f), 0.5f),
               "HUD vertex: divided by the stretch about the centre; a vertex at or beyond the frame's edge stays at the edge (a full-screen dim covers "
               "the whole picture); just inside the edge it is narrowed (CONTROL)");
        // The track map's own viewport in clip space (x scale 0.150781, offset -0.746875; the frame's centre at 0.00078) with a
        // stretch of 1.3125: scale 0.114881, offset 0.00078 + (-0.747656) / 1.3125 = -0.568863. In y with the frame's centre at
        // -0.648438 (a 720-row frame at the top of a 2048-row target) and a stretch of 1.1111: scale -0.0908203 -> -0.0817383,
        // offset -0.422363 -> -0.648438 + 0.226075 x 0.9 = -0.444971.
        float sx = 0.150781f, ox = -0.746875f, sy = -0.0908203f, oy = -0.422363f, s1 = 0.5f, o1 = 0.25f;
        NarrowAbout(sx, ox, 0.000781298f, 1.3125f), NarrowAbout(sy, oy, -0.648438f, 1.0f / 0.9f), NarrowAbout(s1, o1, 0.1f, 1.0f);
        Expect(Near(sx, 0.1148808f) && Near(ox, -0.5688630f) && Near(sy, -0.0817383f) && Near(oy, -0.4449705f) && s1 == 0.5f && o1 == 0.25f,
               "HUD viewport: a draw in a viewport of its own is narrowed and moved about the frame's centre (the track map at 21:9 and at 16:10); a "
               "stretch of 1 changes nothing");
        // Scissors, the frame at 0..1280 (x) or 0..720 (y) of the target: the whole frame stays; a panel from 115 to the right edge
        // keeps that edge, its left at 115 becomes 377.5 with a stretch of 2; the track map's 65..258 with 1.3125 becomes
        // 201.9..348.9; its rows 498..684 with 1.1111 become 484.2..651.6; a tile's scissor beyond the frame (0..2048) stays.
        uint32_t x = 0, w = 1280, px = 115, pw = 1165, mx = 65, mw = 193, my = 498, mh = 186, ty = 0, th = 2048, ex = 640, ew = 0;
        NarrowScissor(x, w, 0, 1280, 2.0f), NarrowScissor(px, pw, 0, 1280, 2.0f), NarrowScissor(mx, mw, 0, 1280, 1.3125f), NarrowScissor(my, mh, 0, 720, 1.0f / 0.9f),
            NarrowScissor(ty, th, 0, 720, 1.0f / 0.9f), NarrowScissor(ex, ew, 0, 1280, 2.0f);
        Expect(x == 0 && w == 1280 && px == 377 && pw == 903 && mx == 201 && mw == 148 && my == 484 && mh == 168 && ty == 0 && th == 2048 && ew == 0,
               "HUD scissor: an edge at or beyond the frame's edge stays, another is narrowed about the frame's centre, rounded outward (115..1280 at 2x "
               "is 377..1280; 65..258 at 1.3125 is 201..349; rows 498..684 at 16:10 are 484..652); empty stays empty");
    }
    // The HUD rule (FM4's frames): the final picture's target is colour format 2, 1280 wide; the 3D scene reaches it as unblended
    // rectangle lists (primitive 8); everything else there is 2D UI. Nothing in another target is touched.
    Expect(IsOverlayDraw(2, 1280, 4, true) && IsOverlayDraw(2, 1280, 5, true) && IsOverlayDraw(2, 1280, 6, true) && IsOverlayDraw(2, 1280, 8, true) &&
               IsOverlayDraw(2, 1280, 4, false) && !IsOverlayDraw(2, 1280, 8, false) && !IsOverlayDraw(3, 1280, 4, true) && !IsOverlayDraw(0, 1280, 8, true) &&
               !IsOverlayDraw(2, 320, 4, true) && !IsOverlayDraw(4, 520, 4, false),
           "HUD draws: anything in the 1280-wide format-2 target but an unblended rectangle list (the scene's copy); the scene's float target, "
           "shadow and bloom targets are never touched (CONTROL)");
    {
        // The state: nothing without the preference; the window's factor with it; a forced factor wins (diagnostic); the latch
        // changes only at Latch; the effective factor is 1 unless the game's hook widened a view in the last 30 vblanks.
        State s;
        s.SetWindow(3440, 1440);
        const bool off = s.Wanted() == 1.0f && !s.Latch() && s.Factor() == 1.0f;
        s.SetPreference(true);
        const bool pending = Near(s.Wanted(), 1.34375f) && s.Factor() == 1.0f;
        const bool latched = s.Latch() && Near(s.Factor(), 1.34375f) && !s.Latch();
        const bool idle = s.Effective(1000) == 1.0f && !s.Widening(1000);
        s.NoteWidened(1000);
        const bool widening = Near(s.Effective(1000), 1.34375f) && Near(s.Effective(1030), 1.34375f) && s.Effective(1031) == 1.0f;
        s.SetWindow(1920, 1080);
        const bool resized = Near(s.Factor(), 1.34375f) && s.Latch() && s.Factor() == 1.0f;
        s.SetWindow(5120, 1440);
        s.SetPreference(false);
        const bool offAgain = !s.Latch() && s.Factor() == 1.0f && !s.Preference();
        s.Force(1.3125f);
        const bool forced = s.Latch() && Near(s.Factor(), 1.3125f);
        s.Force(9.0f);
        const bool clamped = s.Latch() && s.Factor() == 2.0f;
        s.Force(0.9f);
        const bool tallForced = s.Latch() && Near(s.Factor(), 0.9f);
        s.Force(0.1f);
        const bool tallClamped = s.Latch() && s.Factor() == 0.75f;
        Expect(off && pending && latched && idle && widening && resized && offAgain && forced && clamped && tallForced && tallClamped,
               "state: off is 1; on follows the window; a change waits for the latch; the effective factor needs a main view widened in the last half second; "
               "a resize to 16:9 goes back to 1; a forced factor (diagnostic) wins and is clamped to 0.75..2");
    }
    std::printf("extended view tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
