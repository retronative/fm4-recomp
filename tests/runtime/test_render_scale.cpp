// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// runtime/video/render_scale.h (M9, FM4_INTERNAL_RES): the parse (fail-closed) and the guest -> host pixel arithmetic. Expected
// values are hand-computed (the scale in halves: 2 = 1x, 3 = 1.5x, 4 = 2x; a host edge is floor(guest * halves / 2)).
#include "render_scale.h"
#include <cstdio>
#include <string>

using namespace runtime::video::scale;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", what);
    failures += ok ? 0 : 1;
}

int main()
{
    // The scale is in halves: 2 = 1x, 3 = 1.5x (FHD), 4 = 2x (QHD).
    uint32_t s = 0;
    std::string error;
    Expect(ParseInternalRes(nullptr, s, error) && s == 2 && ParseInternalRes("", s, error) && s == 2 && ParseInternalRes("720p", s, error) &&
               s == 2 && ParseInternalRes("1x", s, error) && s == 2,
           "parse: unset, empty, 720p, 1x -> 2 halves (today's path)");
    Expect(ParseInternalRes("fhd", s, error) && s == 3 && (s = 0, ParseInternalRes("1080p", s, error)) && s == 3 &&
               (s = 0, ParseInternalRes("1.5x", s, error)) && s == 3,
           "parse: fhd, 1080p, 1.5x -> 3 halves");
    Expect(ParseInternalRes("qhd", s, error) && s == 4 && (s = 0, ParseInternalRes("1440p", s, error)) && s == 4 &&
               (s = 0, ParseInternalRes("2x", s, error)) && s == 4,
           "parse: qhd, 1440p, 2x -> 4 halves");
    Expect(ParseInternalRes("4k", s, error) && s == 6 && (s = 0, ParseInternalRes("2160p", s, error)) && s == 6 &&
               (s = 0, ParseInternalRes("3x", s, error)) && s == 6 && HostEdge(1280, 6) == 3840 && HostEdge(720, 6) == 2160 && HostEdge(7, 6) == 21,
           "parse: 4k, 2160p, 3x -> 6 halves; 1280x720 is 3840x2160");
    s = 7;
    Expect(!ParseInternalRes("QHD", s, error) && error.find("use 720p") != std::string::npos && error.find("fhd") != std::string::npos && s == 7 &&
               !ParseInternalRes("8k", s, error) && !ParseInternalRes("4K", s, error) && !ParseInternalRes("2", s, error) && !ParseInternalRes("1.5", s, error) && !ParseInternalRes("FHD", s, error),
           "CONTROL: parse: other spellings are refused, the scale untouched");
    // Guest 1280x720: 2560x1440 at 2x, 1920x1080 at 1.5x. EDRAM tile sizes (80, 40, 20 wide; 16, 8 high) and the half tiles (40, 20, 10)
    // at 1.5x: 120, 60, 30; 24, 12; 60, 30, 15 (whole).
    Expect(HostEdge(1280, 4) == 2560 && HostEdge(720, 4) == 1440 && HostEdge(720, 2) == 720 && HostEdge(1280, 3) == 1920 && HostEdge(720, 3) == 1080,
           "host edge: 1280x720 is 2560x1440 at 2x and 1920x1080 at 1.5x");
    Expect(HostEdge(80, 3) == 120 && HostEdge(40, 3) == 60 && HostEdge(20, 3) == 30 && HostEdge(10, 3) == 15 && HostEdge(16, 3) == 24 && HostEdge(8, 3) == 12 &&
               HostExact(80, 3) && HostExact(10, 3) && HostExact(8, 3) && HostExact(7, 4) && HostExact(7, 2) && !HostExact(7, 3) && !HostExact(1, 3),
           "host edge: every tile and half-tile size is whole at 1.5x; an odd size is not (CONTROL)");
    // An odd edge at 1.5x: guest 7 is host 10.5 -> 10; guest 9 is 13.5 -> 13.
    Expect(HostEdge(7, 3) == 10 && HostEdge(9, 3) == 13 && HostEdge(8, 3) == 12 && HostEdge(7, 4) == 14, "host edge: an odd edge at 1.5x takes the host edge before it");
    // A band (0,256 1280x208) -> (0,512 2560x416) at 2x, (0,384 1920x312) at 1.5x; a scissor (8,16 1272x704) -> (16,32 2544x1408) and
    // (12,24 1908x1056). An odd rectangle at 1.5x, (7,9 10x5): edges 7 -> 10, 17 -> 25, 9 -> 13, 14 -> 21, so (10,13 15x8); the size
    // times 1.5 would be 15x7 (CONTROL: the bottom edge would move).
    Expect(HostRect({0, 256, 1280, 208}, 4) == Rect{0, 512, 2560, 416} && HostRect({8, 16, 1272, 704}, 4) == Rect{16, 32, 2544, 1408} &&
               HostRect({8, 16, 1272, 704}, 2) == Rect{8, 16, 1272, 704} && HostRect({0, 256, 1280, 208}, 3) == Rect{0, 384, 1920, 312} &&
               HostRect({8, 16, 1272, 704}, 3) == Rect{12, 24, 1908, 1056},
           "host rect: 2x and 1.5x of even rectangles; 1x unchanged");
    Expect(HostRect({7, 9, 10, 5}, 3) == Rect{10, 13, 15, 8} && !(HostRect({7, 9, 10, 5}, 3) == Rect{10, 13, 15, 7}),
           "host rect: an odd rectangle at 1.5x from its four edges, not its size (CONTROL)");
    // Neighbours share their edge: guest rows [0, 7) and [7, 16) at 1.5x are host rows [0, 10) and [10, 24); every split of a 64-wide
    // span leaves no gap and no overlap, and the whole 1280x720 maps to the whole host image.
    bool tiles = true;
    for (uint32_t halves : {2u, 3u, 4u})
        for (uint32_t cut = 0; cut <= 64; cut++)
        {
            const Rect a = HostRect({0, 0, cut, 1}, halves), b = HostRect({cut, 0, 64 - cut, 1}, halves);
            tiles = tiles && a.x == 0 && a.x + a.width == b.x && b.x + b.width == 64 * halves / 2;
        }
    Expect(tiles && HostRect({0, 0, 7, 1}, 3) == Rect{0, 0, 10, 1} && HostRect({7, 0, 9, 1}, 3) == Rect{10, 0, 14, 1} &&
               HostRect({0, 0, 1280, 720}, 3) == Rect{0, 0, 1920, 1080},
           "host rect: two rectangles that share an edge neither overlap nor leave a gap; the full surface is the full image");
    // Host pixel 321 at 2x lies in guest 160, host 320 too. At 1.5x host 0, 1 | 2 | 3, 4 | 5 lie in guest 0 | 1 | 2 | 3 (host pixel 1
    // spans guest [0.67, 1.33): its first half is in guest 0).
    Expect(GuestOfHost(321, 4) == 160 && GuestOfHost(320, 4) == 160 && GuestOfHost(321, 2) == 321 && GuestOfHost(0, 3) == 0 && GuestOfHost(1, 3) == 0 &&
               GuestOfHost(2, 3) == 1 && GuestOfHost(3, 3) == 2 && GuestOfHost(4, 3) == 2 && GuestOfHost(5, 3) == 3 && GuestOfHost(1919, 3) == 1279,
           "host -> guest pixel at 2x and 1.5x");
    // The downsample's box filter. 2x: guest g covers host 2g and 2g + 1 whole (2 halves each). 1.5x: guest 0 covers host [0, 1.5):
    // pixel 0 whole and half of pixel 1; guest 1 covers [1.5, 3): half of pixel 1 and pixel 2 whole; guest 2 starts again at host 3.
    Expect(DownsampleTaps(0, 4) == BoxTaps{0, {2, 2, 0}} && DownsampleTaps(5, 4) == BoxTaps{10, {2, 2, 0}} && DownsampleTaps(0, 3) == BoxTaps{0, {2, 1, 0}} &&
               DownsampleTaps(1, 3) == BoxTaps{1, {1, 2, 0}} && DownsampleTaps(2, 3) == BoxTaps{3, {2, 1, 0}} && DownsampleTaps(3, 3) == BoxTaps{4, {1, 2, 0}} &&
               DownsampleTaps(7, 2) == BoxTaps{7, {2, 0, 0}} && DownsampleTaps(0, 6) == BoxTaps{0, {2, 2, 2}} && DownsampleTaps(5, 6) == BoxTaps{15, {2, 2, 2}},
           "downsample taps: (2, 2) at 2x; (2, 1) and (1, 2) by parity at 1.5x; (2, 2, 2) at 3x (guest 5 covers host 15, 16, 17); the one pixel at 1x");
    // Every host pixel's halves are handed out exactly once across the guest pixels (the filter loses and repeats nothing), and each
    // guest pixel gets `halves` of them.
    bool partition = true;
    for (uint32_t halves : {3u, 4u, 6u})
    {
        uint32_t covered[200] = {};
        for (uint32_t g = 0; g < 64; g++)
        {
            const BoxTaps t = DownsampleTaps(g, halves);
            partition = partition && t.weight[0] + t.weight[1] + t.weight[2] == halves;
            covered[t.first] += t.weight[0], covered[t.first + 1] += t.weight[1], covered[t.first + 2] += t.weight[2];
        }
        for (uint32_t h = 0; h < 64 * halves / 2; h++)
            partition = partition && covered[h] == 2;
    }
    Expect(partition, "downsample taps: each host pixel's two halves are used once, each guest pixel takes `halves` of them");
    // A depth resolve's unfiltered pick, the host pixel covered whole: 2g at 2x; at 1.5x 0, 2, 3, 5, 6, 8 for guest 0-5.
    Expect(WholeHostPixel(0, 4) == 0 && WholeHostPixel(5, 4) == 10 && WholeHostPixel(0, 3) == 0 && WholeHostPixel(1, 3) == 2 && WholeHostPixel(2, 3) == 3 &&
               WholeHostPixel(3, 3) == 5 && WholeHostPixel(4, 3) == 6 && WholeHostPixel(5, 3) == 8 && WholeHostPixel(9, 2) == 9 && WholeHostPixel(5, 6) == 15 && DownsampleTaps(5, 6).weight[0] == 2 && GuestOfHost(17, 6) == 5 && GuestOfHost(18, 6) == 6,
           "whole host pixel: 2g at 2x; 3m and 3m + 2 at 1.5x");
    bool whole = true;
    for (uint32_t g = 0; g < 64; g++)
    {
        const BoxTaps t = DownsampleTaps(g, 3);
        whole = whole && t.weight[WholeHostPixel(g, 3) - t.first] == 2;
    }
    Expect(whole, "whole host pixel: it is the tap covered by both halves");
    // ScaleFragCoordReads on the translator's two reads as shader_glsl.cpp emits them (param_gen, alpha-to-mask): host pixel 7.5 at
    // 2x -> guest pixel floor(floor(7.5) * 2 / 4) = 3, at 1.5x floor(7 * 2 / 3) = 4; the integer form uvec2(7) * 2 / 4 = 3 and * 2 / 3 = 4
    // (GuestOfHost above). Text without either read is left alone.
    std::string glsl = "vec4 xe_param_gen()\n{\n    float x = abs(floor(gl_FragCoord.x)), y = abs(floor(gl_FragCoord.y));\n}\n"
                       "{\n        uvec2 xe_p = uvec2(gl_FragCoord.xy);\n}\n";
    const bool rewritten = ScaleFragCoordReads(glsl) &&
                           glsl.find("float x = abs(floor(floor(gl_FragCoord.x) * 2.0 / float(XE_HALVES))), y = abs(floor(floor(gl_FragCoord.y) * 2.0 / float(XE_HALVES)));") !=
                               std::string::npos &&
                           glsl.find("uvec2 xe_p = uvec2(gl_FragCoord.xy) * 2u / XE_HALVES;") != std::string::npos &&
                           glsl.find("abs(floor(gl_FragCoord.x))") == std::string::npos;
    std::string other = "void main() { xe_color0 = vec4(1.0); }\n";
    const std::string before = other;
    Expect(rewritten && GuestOfHost(7, 4) == 3 && GuestOfHost(7, 3) == 4 && !ScaleFragCoordReads(other) && other == before,
           "fragcoord: both reads in guest pixels; other text untouched (CONTROL)");
    std::printf("render_scale tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
