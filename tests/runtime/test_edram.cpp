// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// Tests of the CPU-side EDRAM render-target model (runtime/video/edram.h), CPU only: no Vulkan header, no
// device. Every expected value is computed by hand from Xenia's formulas (xenia-canary 67f16c4e315f, cited as
// "X file:line" next to each computation), never from this code's output. "CONTROL" names a negative
// control: a wrong result or a refused configuration that must not pass.
//
//   tests/run_edram_tests.sh
#include "edram.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace runtime::video::edram;

namespace {
int failures = 0;
void Expect(bool ok, const char* what, int line)
{
    std::printf("%s %d %s\n", ok ? "pass" : "FAIL", line, what);
    failures += !ok;
}
// Variadic so that braced initializers (Rect{...}) inside the condition pass through the preprocessor.
#define CHECK(...) Expect((__VA_ARGS__), #__VA_ARGS__, __LINE__)
#define CONTROL(...) Expect(!(__VA_ARGS__), "CONTROL " #__VA_ARGS__, __LINE__)

// The three words of FM4's scene draws (the local capture trace2_cp (not distributed) "surface 0A020280 color 00020000 depth
// 000102D0"): pitch 640, 4x MSAA; colour base 0, k_2_10_10_10; depth base 720, kD24FS8 (NOTES.md §1).
constexpr uint32_t kFm4Surface = 0x0A020280, kFm4Color = 0x00020000, kFm4Depth = 0x000102D0;
// A hypothetical 1280x720 1x surface pair on the same tiles (the aliasing VERIFIED on ForzaRecomp, not yet on
// our disc): pitch 1280 at 1x, bases 0 and 720, the same formats.
constexpr uint32_t k1xSurface = 1280, k1xColor = 0x00020000, k1xDepth = 0x000102D0;

// X render_target_cache.h:269-279: base 0-10, pitch tiles at 32 bpp 11-18, MSAA 19-20, depth 21, format 22-25.
const Key kAc = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10);      // 0 | 16 << 11 | 2 << 19 | 2 << 22 = 0x908000
const Key kAd = Key::Depth(720, 16, Msaa::X4, DepthFormat::D24FS8);         // 0x2D0 | 0x8000 | 0x100000 | 0x200000 | 0x400000 = 0x7082D0
const Key kBc = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10);
const Key kBd = Key::Depth(720, 16, Msaa::X1, DepthFormat::D24FS8);

BindRegisters Fm4Bind(uint32_t mode = 4, uint32_t depthControl = 2, uint32_t colorMask = 0xF)
{
    BindRegisters r;
    r.surfaceInfo = kFm4Surface, r.modeControl = mode, r.depthControl = depthControl, r.colorMask = colorMask;
    r.depthInfo = kFm4Depth, r.colorInfo[0] = kFm4Color;
    return r;
}
BindRegisters Bind1x(uint32_t depthControl = 2, uint32_t colorMask = 0xF)
{
    BindRegisters r;
    r.surfaceInfo = k1xSurface, r.modeControl = 4, r.depthControl = depthControl, r.colorMask = colorMask;
    r.depthInfo = k1xDepth, r.colorInfo[0] = k1xColor;
    return r;
}
std::vector<Transfer> T(std::initializer_list<Transfer> l) { return l; }

// A float as the guest stores it: big-endian bytes, loaded as a little-endian word (X draw_util.cc:789-800
// reads the words so and applies the fetch constant's k8in32 swap).
uint32_t GuestFloat(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return __builtin_bswap32(b);
}

// FM4-like resolve of the whole 640x360 4x colour target into a k_8_8_8_8 texture (format 6, X xenos.h:589)
// of pitch 640, height 360, at a plain physical address, big-endian words (endian k8in32 = 2), four samples
// averaged (k0123 = 6), convert (1), no clears: NOTES.md §3 (HYPOTHESIS until a trace shows a resolve).
ResolveRegisters Fm4Resolve()
{
    ResolveRegisters r;
    r.surfaceInfo = kFm4Surface, r.colorInfo[0] = kFm4Color, r.depthInfo = kFm4Depth, r.modeControl = 6;
    r.copyControl = 1u << 20 | 6u << 4;                 // X registers.h:796-810
    r.copyDestBase = 0x00800000;
    r.copyDestPitch = 640 | 360u << 16;                 // X registers.h:828-837
    r.copyDestInfo = 2 | 6u << 7;                       // X registers.h:812-826: endian 2, format 6, number 0, bias 0, no swap
    r.windowScissorBr = 8192 | 8192u << 16, r.screenScissorBr = 8192 | 8192u << 16;
    r.fetch0 = 3 | 0x00100000;                          // X xenos.h:1141-1142: type 3 (vertex), address in dwords
    r.fetch1 = 2 | 6u << 2;                             // :1144-1145: endian k8in32, size 6 words
    // D3D's resolve rectangle: (-0.5, -0.5), (639.5, -0.5), (639.5, 359.5), the half-pixel offset reversed by
    // X draw_util.cc:801-807 (pix_center 0 -> + 0.5).
    const float v[6] = {-0.5f, -0.5f, 639.5f, -0.5f, 639.5f, 359.5f};
    for (int i = 0; i < 6; i++)
        r.vertexWords[i] = GuestFloat(v[i]);
    return r;
}

void TestRegisters()
{
    const SurfaceInfo s = DecodeSurfaceInfo(kFm4Surface);  // bits 0-13 0x280 = 640; bits 16-17 of 0x0A02 = 2; bits 18-31 = 0x0A020280 >> 18 = 640
    CHECK(s.pitch == 640 && s.msaa == 2 && s.hizPitch == 640);
    const ColorInfo c = DecodeColorInfo(kFm4Color);  // base 0, format bits 16-19 = 2, bias 0
    CHECK(c.base == 0 && c.format == 2 && c.expBias == 0);
    const DepthInfo d = DecodeDepthInfo(kFm4Depth);  // base 0x2D0 = 720, format bit 16 = 1
    CHECK(d.base == 720 && d.format == 1);
    // The signed 6-bit exponent bias (X registers.h:694): field 0x3F = -1, 0x1F = 31, 0x20 = -32.
    CHECK(DecodeColorInfo(0x03F00000).expBias == -1 && DecodeColorInfo(0x01F00000).expBias == 31 && DecodeColorInfo(0x02000000).expBias == -32);
    // The 11-bit base: bit 11 is not part of it (X registers.h:690-691).
    CHECK(DecodeColorInfo(0x800).base == 0 && DecodeColorInfo(0x7FF).base == 2047 && DecodeDepthInfo(0x9FF).base == 0x1FF);
}

void TestTileMath()
{
    // X render_target_cache.cc:606-608: (pitch << (4x)) + 79) / 80.
    CHECK(PitchTilesAt32bpp(640, Msaa::X4) == 16);   // (1280 + 79) / 80 = 16
    CHECK(PitchTilesAt32bpp(1280, Msaa::X1) == 16);  // (1280 + 79) / 80 = 16
    CHECK(PitchTilesAt32bpp(1280, Msaa::X4) == 32);  // (2560 + 79) / 80 = 32
    CHECK(PitchTilesAt32bpp(1152, Msaa::X1) == 15);  // (1152 + 79) / 80 = 15 (X's 4541099D note, .cc:812-816)
    CHECK(PitchTilesAt32bpp(640, Msaa::X2) == 8);    // 2x adds no horizontal samples: (640 + 79) / 80 = 8
    CHECK(PitchTilesAt32bpp(0, Msaa::X1) == 0 && PitchTilesAt32bpp(1, Msaa::X1) == 1);
    // X render_target_cache.cc:1017-1043: rows = ceil(2048 / pitch tiles), capped at (8192 << 2x) / 16; height = rows * (16 >> 2x).
    CHECK(RenderTargetHeight(16, Msaa::X4) == 1024);  // rows = 128, cap 1024 -> 128 * 8
    CHECK(RenderTargetHeight(16, Msaa::X1) == 2048);  // rows = 128, cap 512 -> 128 * 16
    CHECK(RenderTargetHeight(1, Msaa::X1) == 8192);   // rows = 2048, cap 512 -> 512 * 16
    CHECK(RenderTargetHeight(205, Msaa::X4) == 80);   // rows = (2048 + 204) / 205 = 10 -> 10 * 8
    CHECK(RenderTargetHeight(15, Msaa::X1) == 2192);  // rows = (2048 + 14) / 15 = 137 -> 137 * 16
    CHECK(RenderTargetHeight(0, Msaa::X1) == 0);
    // Keys: packing (X .h:269-279), width (:324-332), pitch tiles (:321-323), 64 bpp (xenos.h:323-327).
    CHECK(kAc.packed == 0x908000u && kAd.packed == 0x7082D0u);
    CHECK(kAc.Base() == 0 && kAc.PitchTilesAt32bpp() == 16 && kAc.MsaaMode() == Msaa::X4 && !kAc.IsDepth() && kAc.Format() == 2);
    CHECK(kAd.Base() == 720 && kAd.IsDepth() && kAd.Format() == 1);
    CHECK(kAc.Width() == 640 && kAc.Height() == 1024 && kBc.Width() == 1280 && kBc.Height() == 2048);
    CHECK(kAc.PitchTiles() == 16 && !kAc.Is64bpp());
    const Key k64 = Key::Color(0, 16, Msaa::X1, ColorFormat::k16_16_16_16);
    CHECK(k64.Is64bpp() && k64.PitchTiles() == 32 && k64.Width() == 1280);
    CHECK(Key{}.Empty() && !kAc.Empty() && kAc != kBc && kAc == Key::Color(2048, 16, Msaa::X4, ColorFormat::k2_10_10_10));  // base wraps in 11 bits
    // Storage formats (X xenos.h:394-404; render_target_cache.h:703-711).
    CHECK(StorageFormat(ColorFormat::k8_8_8_8_Gamma) == ColorFormat::k8_8_8_8);
    CHECK(StorageFormat(ColorFormat::k2_10_10_10_As_10_10_10_10) == ColorFormat::k2_10_10_10);
    CHECK(StorageFormat(ColorFormat::k2_10_10_10_Float_As_16_16_16_16) == ColorFormat::k2_10_10_10_Float);
    CHECK(StorageFormat(ColorFormat::k32_Float) == ColorFormat::k32_Float);
    CHECK(IsKnownColorFormat(7) && IsKnownColorFormat(10) && IsKnownColorFormat(15));
    CONTROL(IsKnownColorFormat(8) || IsKnownColorFormat(9) || IsKnownColorFormat(11) || IsKnownColorFormat(13));
    // The M4 whitelist.
    std::string why;
    CHECK(SupportedKey(kAc, why) && SupportedKey(kAd, why) && SupportedKey(Key::Color(0, 16, Msaa::X1, ColorFormat::k8_8_8_8), why));
    CHECK(SupportedKey(Key::Depth(0, 16, Msaa::X1, DepthFormat::D24S8), why));
    CHECK(SupportedKey(Key::Color(0, 16, Msaa::X2, ColorFormat::k8_8_8_8), why));  // 2x (the local capture draw_bases2 (not distributed)'s HDR resolve)
    // FM4 Recomp (alpha, RT format 5): k_16_16_16_16 (storage 5) is accepted at 1x, 2x and 4x; the other 64 bpp formats are not.
    CHECK(SupportedKey(k64, why) && SupportedKey(Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16_16_16), why) &&
          SupportedKey(Key::Color(0, 8, Msaa::X2, ColorFormat::k16_16_16_16), why));
    CONTROL(SupportedKey(Key::Color(0, 16, Msaa::X1, ColorFormat::k16_16_16_16_Float), why));
    CHECK(why.find("64 bpp") != std::string::npos);
    CHECK(SupportedKey(Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float), why) &&
          SupportedKey(Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), why));  // storage 3 (7e3)
    CHECK(SupportedKey(Key::Color(832, 16, Msaa::X1, ColorFormat::k16_16), why));  // FM4 Recomp: observed in FM4
    CONTROL(SupportedKey(Key::Color(0, 16, Msaa::X1, ColorFormat::k16_16_Float), why));
    CHECK(SupportedKey(Key::Color(0, 16, Msaa::X1, ColorFormat::k32_Float), why));  // 32_FLOAT (the local capture draw_loop1 (not distributed) draw 22725)
    CONTROL(SupportedKey(Key::Color(0, 16, Msaa::X1, ColorFormat::k32_32_Float), why));  // 64 bpp: refused by name
    CHECK(why.find("64 bpp") != std::string::npos);
    CONTROL(SupportedKey(Key{}, why));
}

void TestRangeRectangles()
{
    // X render_target_cache.cc:338-420. Tile width 80 >> (4x) >> (64 bpp), tile height 16 >> (2x or 4x).
    Rect r[3];
    // The whole FM4 surface: tiles [0, 720) at pitch 16: rows 0..44, first row starts at 0 and the last ends at
    // the pitch, so one rectangle: 4x: 16 * 40 = 640 wide, 45 * 8 = 360 high; 1x: 1280 x 720.
    CHECK(RangeRectangles(0, 720, kAc, r) == 1 && r[0] == Rect{0, 0, 640, 360});
    CHECK(RangeRectangles(0, 720, kBc, r) == 1 && r[0] == Rect{0, 0, 1280, 720});
    // Tiles [100, 250) at 4x pitch 16: rows_start 6 (first row starts at tile 4), rows_end 16 (last row ends at
    // tile 10), rows 10: first (4 * 40, 6 * 8, 12 * 40, 8), middle rows 7..14 (0, 56, 640, 64), last (0, 120, 400, 8).
    CHECK(RangeRectangles(100, 250, kAc, r) == 3 && r[0] == Rect{160, 48, 480, 8} && r[1] == Rect{0, 56, 640, 64} && r[2] == Rect{0, 120, 400, 8});
    // One partial row, tiles [3, 9): (3 * 40, 0, 6 * 40, 8).
    CHECK(RangeRectangles(3, 9, kAc, r) == 1 && r[0] == Rect{120, 0, 240, 8});
    // Tiles [16, 48): two whole rows -> one rectangle (0, 8, 640, 16).
    CHECK(RangeRectangles(16, 48, kAc, r) == 1 && r[0] == Rect{0, 8, 640, 16});
    // Tiles [20, 48): first row partial from tile 4, then one whole row: (160, 8, 480, 8) and (0, 16, 640, 8).
    CHECK(RangeRectangles(20, 48, kAc, r) == 2 && r[0] == Rect{160, 8, 480, 8} && r[1] == Rect{0, 16, 640, 8});
    // The depth key at base 720: tiles [720, 736) are its row 0: (0, 0, 640, 8).
    CHECK(RangeRectangles(720, 736, kAd, r) == 1 && r[0] == Rect{0, 0, 640, 8});
    // 64 bpp at 1x (pitch tiles 32, tile width 40): tiles [0, 64) are rows 0-1: (0, 0, 1280, 32).
    CHECK(RangeRectangles(0, 64, Key::Color(0, 16, Msaa::X1, ColorFormat::k16_16_16_16), r) == 1 && r[0] == Rect{0, 0, 1280, 32});
    // The wrapped tail (X :364-368): key at base 2040, pitch 16, 1x; tiles [0, 8) are local 8..16 of row 0: (640, 0, 640, 16).
    CHECK(RangeRectangles(0, 8, Key::Color(2040, 16, Msaa::X1, ColorFormat::k8_8_8_8), r) == 1 && r[0] == Rect{640, 0, 640, 16});
    CHECK(RangeRectangles(5, 5, kAc, r) == 0);
    CONTROL(RangeRectangles(100, 250, kAc, r) == 1);
}

void TestOwnership()
{
    Ownership o;
    std::vector<Transfer> t;
    std::string why;
    CHECK(o.At(0).Empty() && o.At(2047).Empty() && o.Count(kAc) == 0);
    // Claiming empty tiles transfers nothing (X :1599-1600).
    CHECK(o.Change(kAc, 0, 720, t, why) && t.empty() && o.Count(kAc) == 720 && o.At(719) == kAc && o.At(720).Empty());
    // Length 0: nothing (X :1548-1550).
    CHECK(o.Change(kBc, 0, 0, t, why) && t.empty() && o.Count(kAc) == 720);
    // The 1x key over the same tiles: one transfer [0, 720) from the 4x key.
    CHECK(o.Change(kBc, 0, 720, t, why) && t == T({{0, 720, kAc}}) && o.Count(kBc) == 720 && o.Count(kAc) == 0);
    // Back, partially: tiles [0, 112) from the 1x key; the rest were empty... no: still kBc's. Claim 112 only.
    t.clear();
    CHECK(o.Change(kAc, 0, 112, t, why) && t == T({{0, 112, kBc}}) && o.At(111) == kAc && o.At(112) == kBc);
    // A third colour key over all 720: two transfers, coalesced per source (X :1624-1632).
    const Key kCc = Key::Color(0, 16, Msaa::X1, ColorFormat::k8_8_8_8);
    t.clear();
    CHECK(o.Change(kCc, 0, 720, t, why) && t == T({{0, 112, kAc}, {112, 720, kBc}}));
    // Base-relative start (X :1683-1686): kAd at base 720, start 16 -> tiles [736, 752).
    t.clear();
    CHECK(o.Change(kAd, 16, 16, t, why) && t.empty() && o.At(735).Empty() && o.At(736) == kAd && o.At(751) == kAd && o.At(752).Empty());
    // FM4 Recomp (M7 CPU cost): a claim dest already owns answers without a walk and without transfers; once another owner takes
    // part of it, the next claim transfers that part again (the memo is per owner-map generation).
    t.clear();
    CHECK(o.Change(kAd, 16, 16, t, why) && t.empty() && o.Count(kAd) == 16);
    CHECK(o.Change(kAd, 18, 4, t, why) && t.empty());  // inside the remembered claim
    CHECK(o.Change(kBd, 20, 4, t, why) && t == T({{740, 744, kAd}}) && o.At(740) == kBd);
    t.clear();
    CHECK(o.Change(kAd, 16, 16, t, why) && t == T({{740, 744, kBd}}) && o.Count(kAd) == 16);
    // Wrapping (X :1688-1697): key at base 2040, 16 tiles -> [2040, 2048) and [0, 8); the second extent
    // transfers from kCc, the first from nothing.
    const Key kW = Key::Color(2040, 16, Msaa::X1, ColorFormat::k8_8_8_8);
    t.clear();
    CHECK(o.Change(kW, 0, 16, t, why) && t == T({{0, 8, kCc}}) && o.At(2040) == kW && o.At(2047) == kW && o.At(7) == kW && o.At(8) == kCc);
    CONTROL(o.At(2039) == kW);
    // The whole EDRAM from base 2040: [2040, 2048) then [0, 2040) (X: the tail stops at the start).
    Ownership all;
    t.clear();
    CHECK(all.Change(kW, 0, 2048, t, why) && all.Count(kW) == 2048 && t.empty());
    // Colour over depth tiles is X's kDepthToColor (vulkan_render_target_cache.cc:4910-4913): accepted, one transfer
    // from the depth key (on a copy, so the checks below see the map unchanged).
    Ownership overDepth = o;
    t.clear();
    CHECK(overDepth.Change(Key::Color(720, 16, Msaa::X4, ColorFormat::k2_10_10_10), 16, 16, t, why) && t == T({{736, 752, kAd}}) &&
          overDepth.At(736) == Key::Color(720, 16, Msaa::X4, ColorFormat::k2_10_10_10));
    // Depth over colour tiles is X's kColorToDepth (vulkan_render_target_cache.cc:4904-4908): accepted (on a copy).
    Ownership overColor = o;
    t.clear();
    CHECK(overColor.Change(Key::Depth(0, 16, Msaa::X4, DepthFormat::D24FS8), 0, 8, t, why) && t == T({{0, 8, kW}}));
    // FM4 Recomp: kD24S8 over kD24FS8 tiles is X's kDepthToDepth between formats (FM4, tile 720): accepted
    // (on a copy), one transfer from the kD24FS8 owner (kAd) of tiles 736-751.
    Ownership overFormat = o;
    t.clear();
    CHECK(overFormat.Change(Key::Depth(720, 16, Msaa::X4, DepthFormat::D24S8), 16, 16, t, why) && t == T({{736, 752, kAd}}) &&
          overFormat.At(736) == Key::Depth(720, 16, Msaa::X4, DepthFormat::D24S8));
    CONTROL(o.At(736) != kAd);  // the copy changed, not the original
    // Preconditions (X :1543-1547).
    CONTROL(o.Change(kAc, 2048, 1, t, why));
    CONTROL(o.Change(kAc, 0, 2049, t, why));
    CONTROL(o.Change(Key{}, 0, 1, t, why));
    // The same key again: no transfer, no change.
    t.clear();
    CHECK(o.Change(kAd, 16, 16, t, why) && t.empty());
}

// The resolve clear's cutout (X render_target_cache.cc:1347-1359, :1601-1607): a tile wholly inside the clear
// rectangle is claimed but not transferred. Tile pixels from X :355-359: 1x tiles are 80x16, 4x tiles 40x8;
// a 1x key of pitch 16 has rows of 16 tiles at y = 16 * row.
// The draw extent (X draw_extent_estimator.cc; expected values by hand from its formulas, cited per line).
ExtentRegisters ClearExtent()  // FM4's fast clear (the local capture draw_7e3_4 (not distributed), draw 27833)
{
    ExtentRegisters r;
    r.surfaceInfo = kFm4Surface;           // 4x
    r.windowScissorBr = 8192 | 8192u << 16;  // scissor_br 20002000
    r.clipCntl = 0x00010000;               // clip_disable
    r.suScModeCntl = 0x00010000;           // vtx_window_offset_enable (offset 0)
    r.vteCntl = 0x00000300;                // vtx_xy_fmt, vtx_z_fmt; no viewport scale/offset
    r.vtxCntl = 0x00000004;                // pix_center 0
    r.vportYScale = 0xC3800000, r.vportYOffset = 0x43800000;  // -256, 256: in the registers, disabled by VTE
    return r;
}

void TestExtent()
{
    // NeedsVertexMaxY (X :289-316): the clear needs vertices; a 1280x720 scissor or clipping on does not; a window
    // offset moving the scissor below 8192 does not (X :279-283 adds it unless TL bit 31 disables it).
    ExtentRegisters r = ClearExtent();
    CHECK(NeedsVertexMaxY(r));
    r.windowScissorBr = 1280 | 720u << 16;
    CONTROL(NeedsVertexMaxY(r));
    r = ClearExtent(), r.clipCntl = 0;
    CONTROL(NeedsVertexMaxY(r));
    r = ClearExtent(), r.windowOffset = 0x7FFFu << 16;  // y offset -1 (signed 15-bit): bottom 8191
    CONTROL(NeedsVertexMaxY(r));
    r.windowScissorTl = 1u << 31;  // offset disabled: 8192 again
    CHECK(NeedsVertexMaxY(r));
    r = ClearExtent(), r.screenScissorBr = 8192 | 4096u << 16;  // the screen scissor bounds the bottom (X :284-286)
    CONTROL(NeedsVertexMaxY(r));

    // EstimateMaxY with clipping off and no vertices: the scissor's bottom (X :287), 8192: the stop's height.
    r = ClearExtent();
    CHECK(EstimateMaxY(r, nullptr) == 8192);
    // VertexMaxY (X :199-264): 4x, pix_center 0, no viewport, xy not divided by w. D3D's rectangle with the -0.5
    // pixel offset (as its resolves, draw_util.cc:801-803): y up to 359.5 -> 16.8 fixed 92032, + 128 = 92160,
    // + 255 (MSAA, X :262-263) = 92415, >> 8 = 360. Exactly 360.0: 92160 + 128 + 255 = 92543 >> 8 = 361 (the
    // 1.-exclusive rule with MSAA). At 1x 360.0: 92288 + 127 = 92415 >> 8 = 360 (.5 exclusive).
    const VertexPosition rect[3] = {{-0.5f, 1}, {-0.5f, 1}, {359.5f, 1}};
    CHECK(VertexMaxY(r, rect, 3) == 360);
    const VertexPosition exact[3] = {{0, 1}, {0, 1}, {360.0f, 1}};
    CHECK(VertexMaxY(r, exact, 3) == 361);
    ExtentRegisters one = r;
    one.surfaceInfo = 0x14000500;
    CHECK(VertexMaxY(one, exact, 3) == 360);
    // 1x 1280x720 with -0.5: 719.5 -> 184192 + 128 + 127 = 184447 >> 8 = 720.
    const VertexPosition full[3] = {{-0.5f, 1}, {-0.5f, 1}, {719.5f, 1}};
    CHECK(VertexMaxY(one, full, 3) == 720);
    // pix_center 1: no + 128: 359.5 -> 92032 + 255 = 92287 >> 8 = 360; 359.0 -> 91904 + 255 = 92159 >> 8 = 359.
    ExtentRegisters pc = r;
    pc.vtxCntl = 1;
    const VertexPosition y359[1] = {{359.0f, 1}};
    CHECK(VertexMaxY(pc, rect, 3) == 360 && VertexMaxY(pc, y359, 1) == 359);
    // Window offset y 16 with vtx_window_offset_enable (X :256-258): 360 + 16 = 376.
    ExtentRegisters wo = r;
    wo.windowOffset = 16u << 16;
    CHECK(VertexMaxY(wo, rect, 3) == 376);
    wo.suScModeCntl = 0;
    CHECK(VertexMaxY(wo, rect, 3) == 360);
    // Divided by w unless vtx_xy_fmt (X :212-217): y 719, w 2 -> 359.5 -> 360.
    ExtentRegisters dw = r;
    dw.vteCntl = 0;
    const VertexPosition homog[1] = {{719.0f, 2.0f}};
    CHECK(VertexMaxY(dw, homog, 1) == 360);
    // The viewport's y scale and offset when enabled (X :146-152, :219): y 0.5 * -360 + 360 = 180 -> 46080 + 128 +
    // 255 = 46463 >> 8 = 181.
    ExtentRegisters vp = r;
    vp.vteCntl = 0x30C, vp.vportYScale = 0xC3B40000, vp.vportYOffset = 0x43B40000;
    const VertexPosition half[1] = {{0.5f, 1}};
    CHECK(VertexMaxY(vp, half, 1) == 181);
    // No vertex / all negative: 0 (X :246, :261 max with 0).
    CHECK(VertexMaxY(r, nullptr, 0) == 0);
    const VertexPosition neg[1] = {{-10.0f, 1}};
    CHECK(VertexMaxY(r, neg, 1) == 0);
    // EstimateMaxY takes the vertex bound only when NeedsVertexMaxY: min(8192, 360) = 360; with a 1280x720 scissor
    // the scissor's 720 stays (X :294-313 with _with_scissor false).
    const uint32_t v360 = 360;
    CHECK(EstimateMaxY(r, &v360) == 360);
    ExtentRegisters sc = r;
    sc.windowScissorBr = 1280 | 720u << 16;
    CHECK(EstimateMaxY(sc, &v360) == 720);
    // Clipping on: the viewport's bottom (X :318-363): 0.5 (pix_center 0) + 360 + |-360| = 720.5 -> 720 (truncated),
    // at most the scissor; scale/offset disabled: 0.5 + 1 = 1.5 -> 1; window offset 16 enabled: 736.5 -> 736.
    ExtentRegisters clip = r;
    clip.clipCntl = 0, clip.vteCntl = 0x30C, clip.vportYScale = 0xC3B40000, clip.vportYOffset = 0x43B40000;
    CHECK(EstimateMaxY(clip, nullptr) == 720 && EstimateMaxY(clip, &v360) == 720);
    clip.windowScissorBr = 1280 | 700u << 16;
    CHECK(EstimateMaxY(clip, nullptr) == 700);
    clip.windowScissorBr = 8192 | 8192u << 16, clip.windowOffset = 16u << 16;
    CHECK(EstimateMaxY(clip, nullptr) == 736);
    clip.vteCntl = 0x300, clip.windowOffset = 0;
    CHECK(EstimateMaxY(clip, nullptr) == 1);

    // PassThroughPositions: three 7-dword vertices (FM4's clear VS reads xyz at dword 0, stride 7: NOTES.md §2), the
    // guest's big-endian floats loaded little-endian, fetch endian 2 (k8in32) swaps them back.
    std::vector<uint32_t> window(21, 0);
    const float ys[3] = {-0.5f, -0.5f, 359.5f};
    for (int v = 0; v < 3; v++)
        window[size_t(v) * 7 + 0] = GuestFloat(639.5f), window[size_t(v) * 7 + 1] = GuestFloat(ys[v]);
    std::vector<VertexPosition> pos;
    std::string why;
    CHECK(PassThroughPositions(window.data(), window.size(), 2, 7, 0, 3, pos, why) && pos.size() == 3 && pos[2].y == 359.5f && pos[0].y == -0.5f && pos[1].w == 1.0f);
    CHECK(VertexMaxY(r, pos.data(), pos.size()) == 360);
    // CONTROL: the wrong endian reads the byte-swapped bits (not 359.5).
    CHECK(PassThroughPositions(window.data(), window.size(), 0, 7, 0, 3, pos, why));
    CONTROL(pos[2].y == 359.5f);
    // Refusals: the window one dword short of the third vertex's y (needs 2 * 7 + 0 + 2 = 16 dwords); xyz past the stride.
    CONTROL(PassThroughPositions(window.data(), 15, 2, 7, 0, 3, pos, why));
    CHECK(why.find("shorter") != std::string::npos);
    CHECK(PassThroughPositions(window.data(), 16, 2, 7, 0, 3, pos, why));
    CONTROL(PassThroughPositions(window.data(), window.size(), 2, 7, 5, 3, pos, why));
    CONTROL(PassThroughPositions(window.data(), window.size(), 2, 0, 0, 3, pos, why));

    // Draw 27833 through Bind. Before it: the old clear (colour 00020000 base 0, depth 000102D0 base 720, 360 rows)
    // owns colour 0-719 and depth 720-1439.
    Ownership o;
    BindRegisters old;
    old.surfaceInfo = kFm4Surface, old.modeControl = 4, old.colorMask = 0xFFFF, old.depthControl = 0x8777;
    old.depthInfo = kFm4Depth, old.colorInfo[0] = kFm4Color;
    CHECK(Bind(o, old, 360).ok && o.Count(kAc) == 720 && o.Count(kAd) == 720);
    BindRegisters hdr = old;
    hdr.colorInfo[0] = 0x00030000, hdr.depthInfo = 0x00010400;  // 7e3 at 0, D24FS8 at 1024
    const Key kHc = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), kHd = Key::Depth(1024, 16, Msaa::X4, DepthFormat::D24FS8);
    // The scissor's 8192 rows -> host height 1024 -> (1024 << 1) / 16 * 16 = 2048 tiles, bounded by the bases' distance
    // (X render_target_cache.cc:819-857) to 1024: colour 0-1023 reaches the old depth key's 720-1023, now a depth ->
    // colour transfer (X kDepthToColor); depth 1024-2047 (min(2048, 1024, 2048 - 1024)) takes 1024-1439 from the old key.
    Ownership before = o;
    Binding b = Bind(o, hdr, EstimateMaxY(ClearExtent(), nullptr));
    CHECK(b.ok && b.transfers[1] == T({{0, 720, kAc}, {720, 1024, kAd}}) && b.transfers[0] == T({{1024, 1440, kAd}}) && o.Count(kAd) == 0 &&
          o.Count(kHc) == 1024 && o.Count(kHd) == 1024);
    o = before;
    // With the vertex bound 360: (360 << 1) / 16 = 45 tile rows * 16 = 720 tiles; distance 1024; colour 0-719
    // (min(720, 1024, 1024 - 0)), depth 1024-1743 (min(720, 1024, 2048 + 0 - 1024)); transfers: colour 0-719 from the
    // old 2_10_10_10 key (same samples, storage 2 -> 3), depth 1024-1439 from the old depth key (same format).
    b = Bind(o, hdr, EstimateMaxY(ClearExtent(), &v360));
    CHECK(b.ok && b.color[0] == kHc && b.depth == kHd && b.lengthTiles[1] == 720 && b.lengthTiles[0] == 720);
    CHECK(b.transfers[1] == T({{0, 720, kAc}}) && b.transfers[0] == T({{1024, 1440, kAd}}));
    CHECK(o.Count(kHc) == 720 && o.Count(kHd) == 720 && o.At(1743) == kHd && o.At(1744).Empty() && o.At(720) == kAd && o.At(1023) == kAd);
    // The 7e3 pass at 512 rows (the local capture draw_extent2 (not distributed), draw 25343's vf0: third vertex y 43FFC000 = 511.5):
    // 511.5 -> 130944 + 128 + 255 = 131327 >> 8 = 512 rows -> (512 << 1) / 16 * 16 = 1024 tiles: colour 0-1023 with the
    // depth -> colour transfer of 720-1023, depth 1024-2047.
    o = before;
    const uint32_t v512 = 512;
    const VertexPosition r512[1] = {{511.5f, 1}};
    CHECK(VertexMaxY(r, r512, 1) == 512);
    b = Bind(o, hdr, EstimateMaxY(ClearExtent(), &v512));
    CHECK(b.ok && b.transfers[1] == T({{0, 720, kAc}, {720, 1024, kAd}}) && b.transfers[0] == T({{1024, 1440, kAd}}));
    // The reverse (the local capture draw_hdr4 (not distributed) draw 25372's shape): the old depth key back over 720-1439 takes 720-1023 from the
    // 7e3 colour key (colour -> depth, X kColorToDepth) and 1024-1439 from the depth key at 1024 (X :1539-1699).
    std::vector<Transfer> back;
    std::string backWhy;
    CHECK(o.Change(kAd, 0, 720, back, backWhy) && back == T({{720, 1024, kHc}, {1024, 1440, kHd}}) && o.Count(kAd) == 720 && o.Count(kHc) == 720);
}

void TestCutout()
{
    std::vector<Transfer> t;
    std::string why;
    // Whole tiles: a 1280x720 rectangle at 1x is exactly 16 columns x 45 rows of 80x16 tiles = 720 tiles, all
    // inside: claimed, nothing transferred (trace12's front-buffer resolve clears, NOTES_3.md).
    {
        Ownership o;
        CHECK(o.Change(kAc, 0, 720, t, why) && t.empty());
        const Rect whole{0, 0, 1280, 720};
        CHECK(o.Change(kBc, 0, 720, t, why, &whole) && t.empty() && o.Count(kBc) == 720 && o.Count(kAc) == 0);
    }
    // A partial first and last tile row: the rectangle y 8..712 covers rows 1..43 (y 16..704) entirely; row 0
    // (y 0..16) and row 44 (y 704..720) reach outside it, so their 16 tiles each transfer.
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kAc, 0, 720, t, why) && t.empty());
        const Rect rows{0, 8, 1280, 704};
        CHECK(o.Change(kBc, 0, 720, t, why, &rows) && t == T({{0, 16, kAc}, {704, 720, kAc}}) && o.Count(kBc) == 720);
    }
    // Partial first and last columns: x 80..1200 covers columns 1..14 (x 80..1200); column 0 (x 0..80) and column
    // 15 (x 1200..1280) transfer in every row; column 15 of row r (tile 16r + 15) and column 0 of row r + 1 (tile
    // 16r + 16) are adjacent with the same source and coalesce: {0, 1}, {15, 17}, {31, 33}, ..., {703, 705},
    // {719, 720}: 46 transfers, 2 x 45 = 90 tiles.
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kAc, 0, 720, t, why) && t.empty());
        const Rect columns{80, 0, 1120, 720};
        CHECK(o.Change(kBc, 0, 720, t, why, &columns) && t.size() == 46 && t.front() == Transfer{0, 1, kAc} && t[1] == Transfer{15, 17, kAc} &&
              t.back() == Transfer{719, 720, kAc});
        uint64_t tiles = 0;
        for (const Transfer& x : t)
            tiles += x.endTiles - x.startTiles;
        CHECK(tiles == 90 && o.Count(kBc) == 720);
    }
    // A 4x destination (40x8 tiles) with the sub-rectangle (96, 48, 168, 128) over the tiles PrepareResolve would
    // claim for it, [98, 343): in every row only columns 3, 4, 5 (x 120..240) lie inside x 96..264; rows 6..21
    // (y 48..176) lie inside y 48..176. Skipped: 3 tiles in each of the 16 rows = 48; transferred 245 - 48 = 197.
    // Row 6 starts at column 2: {98, 99}; its columns 6..15 run into row 7's columns 0..2: {102, 115}; and so on.
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kBc, 0, 720, t, why) && t.empty());
        const Rect sub{96, 48, 168, 128};
        CHECK(o.Change(kAc, 98, 245, t, why, &sub) && t.size() >= 2 && t[0] == Transfer{98, 99, kBc} && t[1] == Transfer{102, 115, kBc});
        uint64_t tiles = 0;
        for (const Transfer& x : t)
            tiles += x.endTiles - x.startTiles;
        CHECK(tiles == 197 && o.Count(kAc) == 245 && o.Count(kBc) == 475 && o.At(99) == kAc && o.At(341) == kAc);
    }
    // CONTROLs: a cutout that misses the claimed tiles changes nothing (row 1's 16 tiles all transfer); a cutout
    // smaller than a tile, inside tile 0, still transfers tile 0; an empty cutout is no cutout.
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kAc, 0, 720, t, why) && t.empty());
        const Rect elsewhere{0, 0, 80, 16};
        CHECK(o.Change(kBc, 16, 16, t, why, &elsewhere) && t == T({{16, 32, kAc}}));
        t.clear();
        const Rect small{8, 8, 64, 8};
        CHECK(o.Change(kBc, 0, 1, t, why, &small) && t == T({{0, 1, kAc}}));
        CONTROL(t.empty());
        t.clear();
        const Rect none{0, 0, 0, 0};
        CHECK(o.Change(kBc, 1, 1, t, why, &none) && t == T({{1, 2, kAc}}));
    }
}

void TestBindFm4()
{
    Ownership o;
    // FM4's scene binding, the draw bounded to 360 rows (its surface height): X render_target_cache.cc:844-846
    // length at 32 bpp = ceil((360 << 1) / 16) * 16 = 45 * 16 = 720 tiles each; sorted bases 0 (colour), 720
    // (depth); :819-827 the smallest distance "at 64 bpp" is min((720 - 0) << 1, (2048 + 0 - 720) << 1) = 1440,
    // so at 32 bpp 720; :847-857 colour min(720, 720, 720 - 0) = 720, depth min(720, 720, 2048 + 0 - 720) = 720.
    Binding b = Bind(o, Fm4Bind(), 360);
    CHECK(b.ok && b.rasterizes && b.pitchTilesAt32bpp == 16 && b.msaa == Msaa::X4 && b.widthPixels == 640 && b.heightPixels == 1024);
    CHECK(b.depth == kAd && b.color[0] == kAc && b.color[1].Empty() && b.lengthTiles[0] == 720 && b.lengthTiles[1] == 720);
    CHECK(b.transfers[0].empty() && b.transfers[1].empty());
    CHECK(o.Count(kAc) == 720 && o.Count(kAd) == 720 && o.At(719) == kAc && o.At(720) == kAd && o.At(1439) == kAd && o.At(1440).Empty());
    // The same binding again: nothing to transfer.
    b = Bind(o, Fm4Bind(), 360);
    CHECK(b.ok && b.transfers[0].empty() && b.transfers[1].empty());
    // The height unknown: the host height 1024 gives ceil(2048 / 16) * 16 = 2048 tiles each, but the distance
    // rule clamps both to 720 (X's reason for the rule, :819-827).
    b = Bind(o, Fm4Bind(), kHeightUnknown);
    CHECK(b.ok && b.lengthTiles[0] == 720 && b.lengthTiles[1] == 720 && b.transfers[0].empty() && b.transfers[1].empty());
    // The 1280x720 1x surfaces on the same tiles (X ChangeOwnership: every tile changes owner, one transfer
    // per aspect from the 4x key): colour [0, 720) from kAc, depth [720, 1440) from kAd.
    b = Bind(o, Bind1x(), 720);
    CHECK(b.ok && b.depth == kBd && b.color[0] == kBc && b.widthPixels == 1280 && b.heightPixels == 2048);
    CHECK(b.transfers[1] == T({{0, 720, kAc}}) && b.transfers[0] == T({{720, 1440, kAd}}));
    CHECK(o.Count(kBc) == 720 && o.Count(kBd) == 720 && o.Count(kAc) == 0);
    // Back to the 4x scene: the transfers reverse.
    b = Bind(o, Fm4Bind(), 360);
    CHECK(b.ok && b.transfers[1] == T({{0, 720, kBc}}) && b.transfers[0] == T({{720, 1440, kBd}}));
    // A 1x draw of 100 rows: ceil(100 / 16) = 7 tile rows = 112 tiles of each: only those move.
    b = Bind(o, Bind1x(), 100);
    CHECK(b.ok && b.lengthTiles[0] == 112 && b.lengthTiles[1] == 112);
    CHECK(b.transfers[1] == T({{0, 112, kAc}}) && b.transfers[0] == T({{720, 832, kAd}}));
    CHECK(o.Count(kAc) == 608 && o.Count(kBc) == 112 && o.At(112) == kAc && o.At(832) == kAd);
    // The scene again: only the 112 tiles of each come back from the 1x keys.
    b = Bind(o, Fm4Bind(), 360);
    CHECK(b.ok && b.transfers[1] == T({{0, 112, kBc}}) && b.transfers[0] == T({{720, 832, kBd}}));
    CHECK(o.Count(kAc) == 720);
    // Mixed owners: 1x over 112 tiles, then a colour-only k_8_8_8_8 1x draw (no depth test) of 720 rows: a
    // single target claims min(720, 4096 >> 1, 2048 + 0 - 0) = 720 tiles with two sources in order.
    b = Bind(o, Bind1x(), 100);
    CHECK(b.ok);
    BindRegisters c = Bind1x(0, 0xF);
    c.colorInfo[0] = 0x00000000;  // base 0, k_8_8_8_8
    const Key kCc = Key::Color(0, 16, Msaa::X1, ColorFormat::k8_8_8_8);
    b = Bind(o, c, 720);
    CHECK(b.ok && b.depth.Empty() && b.color[0] == kCc && b.lengthTiles[1] == 720 && b.lengthTiles[0] == 0);
    CHECK(b.transfers[1] == T({{0, 112, kBc}, {112, 720, kAc}}) && b.transfers[0].empty());
    CHECK(o.Count(kCc) == 720 && o.Count(kAd) == 608 && o.Count(kBd) == 112);
    // A colour-only draw of unknown height on a fresh map claims the whole EDRAM (X :847-857 with one target:
    // min(2048, 4096 >> 1, 2048)); on this map that reaches the depth tiles, which now move to colour through depth ->
    // colour transfers (X kDepthToColor) from both depth keys: the caller must still pass the draw's rows, or it moves
    // every tile (NOTES.md "Open questions").
    Ownership fresh;
    b = Bind(fresh, c, kHeightUnknown);
    CHECK(b.ok && b.lengthTiles[1] == 2048 && fresh.Count(kCc) == 2048);
    const Ownership before = o;
    b = Bind(o, c, kHeightUnknown);
    bool fromAd = false, fromBd = false;
    for (const Transfer& t : b.transfers[1])
        fromAd = fromAd || t.source == kAd, fromBd = fromBd || t.source == kBd;
    CHECK(b.ok && fromAd && fromBd && o.Count(kCc) == 2048 && o.Count(kAd) == 0 && o.Count(kBd) == 0);
    o = before;
    // Gamma and _AS_10_10_10_10 formats bind the storage key (KB rule): colour format 1 -> kCc, 10 -> kBc.
    c.colorInfo[0] = 0x00010000;
    b = Bind(o, c, 720);
    CHECK(b.ok && b.color[0] == kCc && b.transfers[1].empty());
    c.colorInfo[0] = 0x000A0000;
    b = Bind(o, c, 720);
    CHECK(b.ok && b.color[0] == kBc && b.transfers[1] == T({{0, 720, kCc}}));
}

void TestBindCases()
{
    std::string why;
    // Pitch 0 (the trace's draws 0-47, NOTES.md §1): nothing rasterizes, nothing bound, nothing claimed.
    Ownership o;
    BindRegisters r = Fm4Bind();
    r.surfaceInfo = 0;
    Binding b = Bind(o, r, 360);
    CHECK(b.ok && !b.rasterizes && b.depth.Empty() && b.color[0].Empty() && o.Count(kAc) == 0 && b.pitchTilesAt32bpp == 0);
    // RB_MODECONTROL other than kColorDepth (4) / kDepth (5): no rasterization (X draw_util.cc:35-47).
    for (uint32_t mode : {0u, 1u, 6u, 7u})
    {
        b = Bind(o, Fm4Bind(mode), 360);
        CHECK(b.ok && !b.rasterizes && o.Count(kAc) == 0);
    }
    // kDepth: no colour target whatever the mask; the depth target alone claims min(720, 2048, 2048) = 720.
    b = Bind(o, Fm4Bind(5), 360);
    CHECK(b.ok && b.rasterizes && b.color[0].Empty() && b.depth == kAd && b.lengthTiles[0] == 720 && o.Count(kAd) == 720);
    // Depth and stencil off: no depth target; a zero colour mask: no colour target (X :627-648); the binding is
    // valid but empty (:730-752).
    b = Bind(o, Fm4Bind(4, 0, 0), 360);
    CHECK(b.ok && b.rasterizes && b.depth.Empty() && b.color[0].Empty());
    // Stencil alone binds the depth target.
    b = Bind(o, Fm4Bind(4, 1, 0), 360);
    CHECK(b.ok && b.depth == kAd && b.color[0].Empty());
    // Duplicate bases (X :688-719): colour 1 at colour 0's base is dropped; depth at a colour base is dropped.
    r = Fm4Bind(4, 2, 0xFF);
    r.colorInfo[1] = kFm4Color;
    { Ownership scratch; b = Bind(scratch, r, 360); }
    CHECK(b.ok && b.color[0] == kAc && b.color[1].Empty() && b.depth == kAd);
    r = Fm4Bind(4, 2, 0xFF);
    r.colorInfo[1] = 0x000002D0;  // colour 1 at the depth base 720
    { Ownership scratch; b = Bind(scratch, r, 360); }
    CHECK(b.ok && b.color[0] == kAc && b.color[1] == Key::Color(720, 16, Msaa::X4, ColorFormat::k8_8_8_8) && b.depth.Empty());
    r = Fm4Bind(4, 2, 0xF);
    r.depthInfo = 0x00010000;  // depth at the colour base 0
    { Ownership scratch; b = Bind(scratch, r, 360); }
    CHECK(b.ok && b.color[0] == kAc && b.depth.Empty());
    // Three targets (X's 4541099D shape, :812-816, at 32 bpp): pitch 1152 1x = 15 tiles, host height 137 rows
    // (2192 px); unknown height: 137 * 15 = 2055 tiles each; bases 0 (depth), 675, 1350; distances at 64 bpp
    // 1350, 1350 and (2048 + 0 - 1350) << 1 = 1396 -> 675 at 32 bpp; lengths min(2055, 675, next - base):
    // depth 675, colour 0 675 (1350 - 675), colour 1 675 (2048 + 0 - 1350 = 698).
    r = BindRegisters{};
    r.surfaceInfo = 1152, r.modeControl = 4, r.depthControl = 2, r.colorMask = 0xFF;
    r.depthInfo = 0x00000000, r.colorInfo[0] = 675, r.colorInfo[1] = 1350;
    Ownership three;
    b = Bind(three, r, kHeightUnknown);
    CHECK(b.ok && b.pitchTilesAt32bpp == 15 && b.heightPixels == 2192);
    CHECK(b.lengthTiles[0] == 675 && b.lengthTiles[1] == 675 && b.lengthTiles[2] == 675);
    CHECK(three.Count(Key::Depth(0, 15, Msaa::X1, DepthFormat::D24S8)) == 675 && three.At(1350) == Key::Color(1350, 15, Msaa::X1, ColorFormat::k8_8_8_8) && three.At(2025).Empty());
    // Refusals, each leaving the map unchanged.
    Ownership before = o;
    r = Fm4Bind();
    r.surfaceInfo = 0x00030280;  // msaa_samples 3
    b = Bind(o, r, 360);
    CONTROL(b.ok);
    CHECK(b.refusal.find("msaa_samples 3") != std::string::npos);
    r = Fm4Bind();
    r.surfaceInfo = 0x00002008;  // pitch 8200
    b = Bind(o, r, 360);
    CONTROL(b.ok);
    r = Fm4Bind();
    r.colorInfo[0] = 0x00080000;  // colour format 8: unknown
    b = Bind(o, r, 360);
    CONTROL(b.ok);
    CHECK(b.refusal.find("unknown") != std::string::npos);
    {
        // FM4 Recomp (alpha, RT format 5): k_16_16_16_16 (64 bpp) at colour base 0 with the scene's depth at 720, 360 rows, on a fresh map
        // (X render_target_cache.cc:844-857): length at 32 bpp ceil((360 << 1) / 16) * 16 = 720; the smallest distance "as if 64 bpp"
        // (:819-827) is min(4096, (720 - 0) << 0 (the 64 bpp colour below), (2048 + 0 - 720) << 1 = 2656) = 720; colour (64 bpp, shift
        // 1): min(720 << 1, 720 >> 0, 720 - 0) = 720; depth (32 bpp): min(720, 720 >> 1, 2048 + 0 - 720) = 360. The key: 0 | 16 << 11 |
        // 2 << 19 | 5 << 22 = 0x1508000.
        r = Fm4Bind();
        r.colorInfo[0] = 0x00050000;
        Ownership scratch;
        b = Bind(scratch, r, 360);
        const Key k64 = Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16_16_16);
        CHECK(k64.packed == 0x1508000u && k64.PitchTiles() == 32 && k64.Width() == 640);
        CHECK(b.ok && b.color[0] == k64 && b.depth == kAd && b.lengthTiles[1] == 720 && b.lengthTiles[0] == 360);
        CHECK(scratch.Count(k64) == 720 && scratch.Count(kAd) == 360 && scratch.At(719) == k64 && scratch.At(720) == kAd && scratch.At(1079) == kAd &&
              scratch.At(1080).Empty());
        CHECK(b.transfers[0].empty() && b.transfers[1].empty());
    }
    r = Fm4Bind();
    r.colorInfo[0] = 0x00070000;  // k_16_16_16_16_FLOAT: 64 bpp, outside the whitelist
    b = Bind(o, r, 360);
    CONTROL(b.ok);
    CHECK(b.refusal.find("64 bpp") != std::string::npos);
    {
        // 2x MSAA binds (on a copy): pitch 640 at 2x is 640 / 80 = 8 tiles a row, 2x tiles are 80x8 pixels.
        Ownership copy = o;
        r = Fm4Bind();
        r.surfaceInfo = 0x00010280;
        b = Bind(copy, r, 360);
        CHECK(b.ok && b.color[0] == Key::Color(0, 8, Msaa::X2, ColorFormat::k2_10_10_10));
    }
    r = Fm4Bind();
    r.colorInfo[0] = 0x00060000;  // k_16_16_FLOAT: 32 bpp, outside the whitelist (FM4 Recomp: k_16_16, 4, is accepted)
    b = Bind(o, r, 360);
    CONTROL(b.ok);
    CHECK(o.Count(kAd) == before.Count(kAd) && o.Count(kAc) == before.Count(kAc));

    // 7e3 (formats 3 and 12, storage 3: X xenos.h:394-404). ForzaRecomp OBSERVED FM4's first 7e3 draw as a 640x360 4x
    // clear into RB_COLOR_INFO 0x00030000, base 0 like the 2_10_10_10 scene target kAc: after
    // the scene binding, the 7e3 key takes tiles 0-719 from kAc (X ChangeOwnership, render_target_cache.cc:1539-1699);
    // the depth key is unchanged, so nothing moves there. Format 12 is the same key (no transfer).
    const Key k7e3 = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float);  // 0 | 16 << 11 | 2 << 19 | 3 << 22 = 0xD08000
    CHECK(k7e3.packed == 0xD08000u && k7e3.Width() == 640 && k7e3.Height() == 1024 && !k7e3.Is64bpp());
    Ownership h;
    CHECK(Bind(h, Fm4Bind(), 360).ok);
    r = Fm4Bind();
    r.colorInfo[0] = 0x00030000;
    b = Bind(h, r, 360);
    CHECK(b.ok && b.color[0] == k7e3 && b.transfers[1] == T({{0, 720, kAc}}) && b.depth == kAd && b.transfers[0].empty() && h.Count(k7e3) == 720);
    r.colorInfo[0] = 0x000C0000;
    b = Bind(h, r, 360);
    CHECK(b.ok && b.color[0] == k7e3 && b.transfers[1].empty());
    CONTROL(b.ok && b.color[0] == Key::Color(0, 16, Msaa::X4, ColorFormat(12)));  // 12 is never a key's format
}

// The configurations our disc uses, OBSERVED in the local capture trace10 (not distributed) (a headless run to swap 400, the CP
// trace extended with the draw initiator, RB_COLOR_MASK and RB_DEPTHCONTROL; NOTES.md §1b, line ranges there),
// as one frame-shaped sequence; and that the model accepts exactly them and nothing beside them.
void TestTrace10()
{
    // (a) The 4x scene surface: RB_SURFACE_INFO 0A020280, RB_COLOR_INFO 00020000, RB_DEPTH_INFO 000102D0.
    // (b) The 1x surface: RB_SURFACE_INFO 14000500 = pitch 0x500 = 1280, msaa_samples (0x14000500 >> 16) & 3 =
    //     0, hiz pitch 0x14000500 >> 18 = 1280; colour 000A0000 = format 10 (k_2_10_10_10_AS_10_10_10_10) or
    //     00010000 = format 1 (8_8_8_8_GAMMA) or, at the resolves, 00020000 / 00000000; the same depth word.
    const SurfaceInfo s = DecodeSurfaceInfo(0x14000500);
    CHECK(s.pitch == 1280 && s.msaa == 0 && s.hizPitch == 1280);
    CHECK(PitchTilesAt32bpp(1280, Msaa::X1) == 16);  // the same 16 tiles a row as the 4x surface: the alias
    const Key k1x2 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10);  // formats 10 and 2 (storage 2)
    const Key k1x0 = Key::Color(0, 16, Msaa::X1, ColorFormat::k8_8_8_8);     // formats 1 and 0 (storage 0)
    Ownership o;
    Binding b;
    // Draws 0-47 (cp.txt:3-52): pitch 0, point lists, colour mask 0, RB_DEPTHCONTROL 0: nothing.
    BindRegisters r;
    r.modeControl = 4;
    b = Bind(o, r, 16);
    CHECK(b.ok && !b.rasterizes);
    // The clear draws (cp.txt:55 first, 27706 last; 1,590 lines): rectangle list (initiator 00030088) with
    // RB_COLOR_MASK FFFF and RB_DEPTHCONTROL 8777 (stencil + z + z write, zfunc 7, stencilfunc 7, zpass
    // REPLACE: D3D's fast clear, KB gpu-edram-and-ring.md), scissor 8192x8192. RB_COLOR1-3_INFO are 0: colour
    // targets 1-3 at base 0, format 0, the same base as target 0, dropped (X render_target_cache.cc:688-719).
    // Height 8192 clamps to the host 1024 -> 2048 tiles at 32 bpp, bounded by the distance rule to 720 each.
    r = BindRegisters{};
    r.surfaceInfo = kFm4Surface, r.modeControl = 4, r.colorMask = 0xFFFF, r.depthControl = 0x8777;
    r.depthInfo = kFm4Depth, r.colorInfo[0] = kFm4Color;
    b = Bind(o, r, 8192);
    CHECK(b.ok && b.color[0] == kAc && b.color[1].Empty() && b.color[2].Empty() && b.color[3].Empty() && b.depth == kAd);
    CHECK(b.lengthTiles[0] == 720 && b.lengthTiles[1] == 720 && b.transfers[0].empty() && b.transfers[1].empty());
    CHECK(o.Count(kAc) == 720 && o.Count(kAd) == 720);
    // The point draws on (a) (cp.txt:61-27700, 9,648 lines) and on (b) (9,491 lines): colour mask 0 and
    // RB_DEPTHCONTROL 0, scissor 16x16: they rasterize but bind nothing (X :627-648, :730-752).
    r.colorMask = 0, r.depthControl = 0;
    b = Bind(o, r, 16);
    CHECK(b.ok && b.rasterizes && b.depth.Empty() && b.color[0].Empty() && o.Count(kAc) == 720);
    r.surfaceInfo = 0x14000500;
    b = Bind(o, r, 16);
    CHECK(b.ok && b.rasterizes && b.depth.Empty() && b.color[0].Empty() && o.Count(kAc) == 720);
    // The UI draws on (b) (cp.txt:267 first; quad lists 0020008D, 357 lines; rect lists 00180088, 38 lines):
    // colour 000A0000, mask F, RB_DEPTHCONTROL 00700730 (z_enable bit 1 and stencil bit 0 clear): colour only,
    // the storage-2 1x key; 720 rows -> 720 tiles, min(720, 4096 >> 1, 2048) = 720 (one target): the 4x colour
    // tiles move to it (the alias), the depth tiles stay the 4x depth key's.
    r.colorInfo[0] = 0x000A0000, r.colorMask = 0xF, r.depthControl = 0x00700730;
    b = Bind(o, r, 720);
    CHECK(b.ok && b.color[0] == k1x2 && b.depth.Empty() && b.lengthTiles[1] == 720);
    CHECK(b.transfers[1] == T({{0, 720, kAc}}) && o.Count(k1x2) == 720 && o.Count(kAd) == 720);
    // The resolve draws on (b) (cp.txt:269 first, 27711 last; 396 with colour 00020000, 38 with 00000000): the
    // source key is the storage key, so colour format 2 at the resolve finds the tiles the format-10 draws
    // wrote (the "key by storage format" rule, ReCarbon). The destination registers
    // are not in the trace: the swap stops name the front buffer (1280x720, fetch format 54; NOTES.md §1b), so
    // a 1280x720 format-54 destination at a plain address stands in (HYPOTHESIS for RB_COPY_DEST_INFO).
    ResolveRegisters rr = Fm4Resolve();
    rr.surfaceInfo = 0x14000500, rr.colorInfo[0] = 0x00020000, rr.copyControl = 1u << 20;  // sample 0 at 1x
    rr.copyDestPitch = 1280 | 720u << 16, rr.copyDestInfo = 2 | 54u << 7;
    const float full[6] = {-0.5f, -0.5f, 1279.5f, -0.5f, 1279.5f, 719.5f};
    for (int k = 0; k < 6; k++)
        rr.vertexWords[k] = GuestFloat(full[k]);
    ResolveInfo i = DecodeResolve(rr);
    CHECK(i.ok && i.x1 == 1280 && i.y1 == 720 && i.source == k1x2 && i.destFormat == 54 && i.spanBase == 0 && i.spanRowLength == 16 && i.spanRows == 45);
    // X texture_util.cc:443-459, :490-518 at pitch 1280 (40 tiles a row): the tile of (1279, 719) is (1248, 704):
    // macro (39 + 22 * 40) << 9 = 470528, micro 0 -> 3764224, + 0x1000 = 3768320 = 1280 * 736 * 4.
    CHECK(i.destExtentLength == 3768320 && i.destBase == 0x00800000u);
    ResolvePlan p = PrepareResolve(o, i);
    CHECK(p.ok && p.copy && p.copySource == k1x2 && p.copyRect == Rect{0, 0, 1280, 720});
    {
        // FM4's second HDR band (the local capture draw_hdr3 (not distributed)): vertices y 511.5..719.5 under a 512-row scissor
        // (02000500): X clamps to the scissor (draw_util.cc GetResolveInfo) -> y 512..512, nothing to copy or clear
        // (X vulkan_render_target_cache.cc:1051-1054): decoded ok and empty, ownership unchanged.
        ResolveRegisters band = rr;
        band.windowScissorBr = 0x02000500;
        const float second[6] = {-0.5f, 511.5f, 1279.5f, 511.5f, 1279.5f, 719.5f};
        for (int k = 0; k < 6; k++)
            band.vertexWords[k] = GuestFloat(second[k]);
        const ResolveInfo e = DecodeResolve(band);
        Ownership copy = o;
        const ResolvePlan ep = PrepareResolve(copy, e);
        CHECK(e.ok && e.empty && ep.ok && !ep.copy && copy.Count(k1x2) == o.Count(k1x2));
        band.windowScissorBr = 0x02D00500;  // CONTROL: the full scissor keeps the band (y 512..720)
        const ResolveInfo f = DecodeResolve(band);
        CHECK(f.ok && !f.empty && f.y0 == 512 && f.y1 == 720);
    }
    // The GAMMA draws (cp.txt:24931 first, 27669 last; triangle strips 00040086, 38 lines): colour 00010000 ->
    // the storage-0 1x key; a same-aspect format reinterpretation transfer of all 720 tiles from the storage-2
    // key (the GPU side's transfer draw, render_targets_vk.h §4), then the 38 resolves with colour 00000000.
    r.colorInfo[0] = 0x00010000;
    b = Bind(o, r, 720);
    CHECK(b.ok && b.color[0] == k1x0 && b.transfers[1] == T({{0, 720, k1x2}}) && o.Count(k1x0) == 720);
    rr.colorInfo[0] = 0x00000000;
    i = DecodeResolve(rr);
    p = PrepareResolve(o, i);
    CHECK(i.ok && i.source == k1x0 && p.ok && p.copySource == k1x0);
    // A storage-2 resolve while the storage-0 key holds the tiles: the source key takes them first (one transfer
    // of all 720), then the copy reads it.
    rr.colorInfo[0] = 0x00020000;
    p = PrepareResolve(o, DecodeResolve(rr));
    CHECK(p.ok && p.copySource == k1x2 && p.copyTransfers == T({{0, 720, k1x0}}) && o.Count(k1x2) == 720 && o.Count(k1x0) == 0);
    // The next frame's 4x clear: the colour tiles come back from the storage-2 1x key (MSAA change); the depth
    // tiles never left the 4x depth key: no depth transfer.
    r = BindRegisters{};
    r.surfaceInfo = kFm4Surface, r.modeControl = 4, r.colorMask = 0xFFFF, r.depthControl = 0x8777;
    r.depthInfo = kFm4Depth, r.colorInfo[0] = kFm4Color;
    b = Bind(o, r, 8192);
    CHECK(b.ok && b.transfers[1] == T({{0, 720, k1x2}}) && b.transfers[0].empty() && o.Count(kAc) == 720 && o.Count(kAd) == 720);
    // trace10 draws 19502-19504 (cp.txt:24894-24899): two 4x clears, then a 1x resolve with no 1x draw between:
    // the resolve's source key takes the 720 colour tiles from the 4x key (a 4x -> 1x transfer), copy from it.
    rr.colorInfo[0] = 0x00020000;
    p = PrepareResolve(o, DecodeResolve(rr));
    CHECK(p.ok && p.copySource == k1x2 && p.copyTransfers == T({{0, 720, kAc}}) && o.Count(k1x2) == 720 && o.Count(kAd) == 720);
    // X draw_util.cc:60-79: a depth test that always passes without writing binds no depth target (X registers.h
    // :744-752: stencil bit 0, z_enable bit 1, z_write_enable bit 2, zfunc bits 4-6: 0x72 = z_enable + always,
    // no write, no stencil); with the write bit (0x76) it does; stencil alone (0x1) does; a non-always zfunc
    // without write (0x12: zfunc 1) does.
    r.colorMask = 0xF, r.depthControl = 0x72;
    b = Bind(o, r, 360);
    CHECK(b.ok && b.depth.Empty() && b.color[0] == kAc);
    r.depthControl = 0x76;
    b = Bind(o, r, 360);
    CHECK(b.ok && b.depth == kAd);
    r.depthControl = 0x12;
    b = Bind(o, r, 360);
    CHECK(b.ok && b.depth == kAd);
    r.depthControl = 0x1;
    b = Bind(o, r, 360);
    CHECK(b.ok && b.depth == kAd);
    // Exactly these keys are accepted: every other colour format on either surface is refused (formats 6-9,
    // 11, 13, 15 at the 4x pitch; 2x MSAA; the 1x pitch with 64 bpp); 3 and 12 (7e3, storage 3), 4 (16_16, FM4 Recomp) and 14 are accepted.
    // FM4 Recomp (alpha, RT format 5): 5 (16_16_16_16) too: on the 1280-pitch 1x surface with depth at 720 and 720 rows, colour claims
    // min(720 << 1, 720 >> 0, 720) = 720 tiles and depth min(720, 720 >> 1, 1328) = 360 (X render_target_cache.cc:819-857).
    for (uint32_t f = 3; f <= 15; f++)
    {
        if (f == 10)
            continue;
        r = BindRegisters{};
        r.surfaceInfo = 0x14000500, r.modeControl = 4, r.colorMask = 0xF, r.depthControl = 0x8777;
        r.depthInfo = kFm4Depth, r.colorInfo[0] = f << 16;
        Ownership scratch;
        if (f == 3 || f == 4 || f == 5 || f == 12 || f == 14)
            CHECK(Bind(scratch, r, 720).ok);
        else
            CONTROL(Bind(scratch, r, 720).ok);
    }
    std::string why;
    CHECK(SupportedKey(kAc, why) && SupportedKey(kAd, why) && SupportedKey(k1x2, why) && SupportedKey(k1x0, why));
}

void TestFixedPoint()
{
    // X ui/graphics_util.cc:17-60: nearest even in 16.8.
    CHECK(FloatToFixed16p8(0.5f) == 128 && FloatToFixed16p8(-0.5f) == -128 && FloatToFixed16p8(639.5f) == 163712 && FloatToFixed16p8(360.0f) == 92160);
    CHECK(FloatToFixed16p8(1.0f / 1024.0f) == 0);   // below 2^-9: 0 (:21-23)
    CHECK(FloatToFixed16p8(1.0f / 512.0f) == 0);    // 0.5 -> even 0 (:52-58)
    CHECK(FloatToFixed16p8(3.0f / 512.0f) == 2);    // 1.5 -> even 2
    CHECK(FloatToFixed16p8(5.0f / 512.0f) == 2);    // 2.5 -> even 2
    CHECK(FloatToFixed16p8(40000.0f) == (1 << 23) - 1 && FloatToFixed16p8(-40000.0f) == -32768 * 256);  // saturation (:24-31)
    CHECK(FloatToFixed16p8(0.0f) == 0);
    // X xenos.h:1070-1086.
    CHECK(GpuSwap32(0x11223344, 0) == 0x11223344u && GpuSwap32(0x11223344, 1) == 0x22114433u && GpuSwap32(0x11223344, 2) == 0x44332211u &&
          GpuSwap32(0x11223344, 3) == 0x33441122u);
}

void TestResolveDecode()
{
    // The whole 640x360 target (X draw_util.cc:801-826): x fixed 0 and 163840, (v + 127) >> 8 -> 0 and 640;
    // y 0 and 360; aligned to 8 unchanged; within the pitch 640; width_div_8 80, height_div_8 45.
    ResolveInfo i = DecodeResolve(Fm4Resolve());
    CHECK(i.ok && i.x0 == 0 && i.y0 == 0 && i.x1 == 640 && i.y1 == 360 && i.msaa == Msaa::X4 && i.pitchTilesAt32bpp == 16);
    CHECK(!i.isDepth && i.sourceIndex == 0 && i.source == kAc && i.command == CopyCommand::Convert && i.sampleSelect == SampleSelect::k0123);
    CHECK(i.copy && i.destFormat == 6 && i.destEndian == 2 && i.destExpBias == 0 && !i.destSwap && i.destPitch == 640 && i.destHeight == 360);
    // :1146-1178: (0, 0) is the origin of its 32x32 tile: base unchanged; the extent is up to the tile of
    // (639, 359): X texture_util.cc:443-459 GetTiledOffset2D(608, 352, 640, 2) = macro (19 + 11 * 20) << 9 =
    // 122368, micro 0 -> 978944, plus 0x400 << 2 (:510-512): 983040 = 640 * 384 * 4 (the 32-row-padded surface).
    CHECK(i.destBase == 0x00800000u && i.destOffsetX == 0 && i.destOffsetY == 0);
    CHECK(i.destExtentStart == 0x00800000u && i.destExtentLength == 983040);
    // :1202-1220 and :875-904: offsets 0, pitch 16, base 0; x1 tiles = ((80 << 4) + 79) / 80 = 16, y1 tiles =
    // ((45 << 4) + 15) / 16 = 45.
    CHECK(i.spanBase == 0 && i.spanRowLength == 16 && i.spanRows == 45 && i.spanPitch == 16 && i.edramOffsetXDiv8 == 0 && i.edramOffsetYDiv8 == 0);
    CHECK(!i.clearColor && !i.clearDepth && i.depthBase == 720 && i.depthFormat == DepthFormat::D24FS8);

    // A sub-rectangle: vertices (99.5, 49.5) .. (259.5, 169.5) -> 100..260 x 50..170 before alignment,
    // 96..264 x 48..176 after (:854-861): 21 x 16 groups of 8.
    ResolveRegisters r = Fm4Resolve();
    const float v[6] = {99.5f, 49.5f, 259.5f, 49.5f, 259.5f, 169.5f};
    for (int k = 0; k < 6; k++)
        r.vertexWords[k] = GuestFloat(v[k]);
    r.copyControl |= 1u << 8 | 1u << 9;  // colour and depth clears
    r.colorClear = 0x3FF00000, r.depthClear = 0xF0000000;
    i = DecodeResolve(r);
    CHECK(i.ok && i.x0 == 96 && i.y0 == 48 && i.x1 == 264 && i.y1 == 176);
    // Destination (:1146-1178, bpp_log2 2 -> 32x32 granularity): tile origin (96, 32), offset (0, 16);
    // GetTiledOffset2D(96, 32, 640, 2) = (3 + 1 * 20) << 9 = 11776 -> 94208; the extent ends at the tile of
    // (263, 175) = (256, 160): (8 + 5 * 20) << 9 = 55296 -> 442368, + 0x1000 = 446464; length 352256.
    CHECK(i.destBase == 0x00800000u + 94208 && i.destOffsetX == 0 && i.destOffsetY == 16);
    CHECK(i.destExtentStart == 0x00800000u + 94208 && i.destExtentLength == 352256);
    // EDRAM (:1202-1220): samples (192, 96): tile offset x 192 / 80 = 2, y 96 / 16 = 6 -> 6 * 16 + 2 = 98;
    // offsets (192 % 80) >> 4 = 2, (96 % 16) >> 4 = 0; span (:875-904): x0t (2 << 4) / 80 = 0, x1t ((23 << 4) +
    // 79) / 80 = 5, y0t 0, y1t ((16 << 4) + 15) / 16 = 16: base 98, 5 tiles a row, 16 rows.
    CHECK(i.edramOffsetXDiv8 == 2 && i.edramOffsetYDiv8 == 0 && i.spanBase == 98 && i.spanRowLength == 5 && i.spanRows == 16);
    CHECK(i.clearColor && i.clearDepth && i.colorClear == 0x3FF00000u && i.depthClear == 0xF0000000u);

    // The window offset (:830-836) and the scissor (:838-850): offset (8, 8) enabled moves the rectangle to
    // 8..648 x 8..368; the window scissor 0..128 x 0..64 also takes the offset (X :596-606, bit 31 clear), so
    // it is 8..136 x 8..72 and clips the rectangle to that; already multiples of 8.
    r = Fm4Resolve();
    r.suScModeCntl = 1u << 16, r.windowOffset = 8 | 8u << 16;
    r.windowScissorBr = 128 | 64u << 16;
    i = DecodeResolve(r);
    CHECK(i.ok && i.x0 == 8 && i.y0 == 8 && i.x1 == 136 && i.y1 == 72);
    // The window scissor's own offset (X :596-601 unless bit 31): tl (16, 16) + offset (8, 8) = (24, 24) clamps
    // the origin up to 24; disabled by bit 31 it stays 16 (the rectangle starts at 8 and is clamped to 16).
    r.windowScissorTl = 16 | 16u << 16;
    i = DecodeResolve(r);
    CHECK(i.ok && i.x0 == 24 && i.y0 == 24);
    r.windowScissorTl |= 1u << 31;
    i = DecodeResolve(r);
    CHECK(i.ok && i.x0 == 16 && i.y0 == 16);
    // The screen scissor intersects (X :613-616): 0..32 x 0..32 -> right/bottom 32.
    r = Fm4Resolve();
    r.screenScissorBr = 32 | 32u << 16;
    i = DecodeResolve(r);
    CHECK(i.ok && i.x1 == 32 && i.y1 == 32);
    // pix_center 1: no half-pixel offset (:801-803): vertices 0.5 .. 639.5 -> fixed 128 .. 163712 -> (128 + 127) >> 8
    // = 0, (163712 + 127) >> 8 = 639 -> aligned up to 640.
    r = Fm4Resolve();
    r.vtxCntl = 1;
    const float c[6] = {0.5f, 0.5f, 639.5f, 0.5f, 639.5f, 359.5f};
    for (int k = 0; k < 6; k++)
        r.vertexWords[k] = GuestFloat(c[k]);
    i = DecodeResolve(r);
    CHECK(i.ok && i.x0 == 0 && i.x1 == 640 && i.y1 == 360);
    // Vertex endian none (the words already host order): fetch1 endian 0.
    r = Fm4Resolve();
    r.fetch1 = 0 | 6u << 2;
    for (int k = 0; k < 6; k++)
        r.vertexWords[k] = __builtin_bswap32(r.vertexWords[k]);
    i = DecodeResolve(r);
    CHECK(i.ok && i.x1 == 640 && i.y1 == 360);
    // Sample selection sanitized (X :495-545): 4x colour k01 stays; 1x anything -> k0; a 4x value 7 -> k0123.
    r = Fm4Resolve();
    r.copyControl = 1u << 20 | 4u << 4;
    CHECK(DecodeResolve(r).sampleSelect == SampleSelect::k01);
    r.copyControl = 1u << 20 | 7u << 4;
    CHECK(DecodeResolve(r).sampleSelect == SampleSelect::k0123);
    r.surfaceInfo = 1280, r.copyControl = 1u << 20 | 6u << 4;
    i = DecodeResolve(r);
    CHECK(i.ok && i.sampleSelect == SampleSelect::k0 && i.source == kBc && i.spanRows == 23 && i.spanRowLength == 8);  // 640 px at 1x: 8 tiles; 360 rows: 23 tile rows
    // Source colour 1 (copy_src_select 1) with its own base.
    r = Fm4Resolve();
    r.copyControl = 1u << 20 | 6u << 4 | 1;
    r.colorInfo[1] = 0x00020000 | 1440;
    i = DecodeResolve(r);
    CHECK(i.ok && i.sourceIndex == 1 && i.source == Key::Color(1440, 16, Msaa::X4, ColorFormat::k2_10_10_10) && i.spanBase == 1440);
    // Gamma / _AS_ source formats key the storage format (as Bind does).
    r = Fm4Resolve();
    r.colorInfo[0] = 0x000A0000;
    CHECK(DecodeResolve(r).source == kAc);

    // Refusals (each a listed one in NOTES.md).
    auto refused = [](ResolveRegisters rr, const char* text) {
        const ResolveInfo x = DecodeResolve(rr);
        return !x.ok && x.refusal.find(text) != std::string::npos;
    };
    r = Fm4Resolve(), r.modeControl = 4;
    CHECK(refused(r, "not kCopy"));
    r = Fm4Resolve(), r.copyControl = 2u << 20 | 6u << 4;
    CHECK(refused(r, "copy command 2"));
    r = Fm4Resolve(), r.copyControl = 0u << 20 | 6u << 4;
    CHECK(refused(r, "raw (0)"));
    r = Fm4Resolve(), r.fetch0 = 2 | 0x00100000;
    CHECK(refused(r, "vertex fetch constant"));
    r = Fm4Resolve(), r.fetch1 = 2 | 5u << 2;
    CHECK(refused(r, "vertex fetch constant"));
    r = Fm4Resolve(), r.surfaceInfo = 0x00030280;
    CHECK(refused(r, "msaa_samples 3"));
    r = Fm4Resolve(), r.surfaceInfo = 0x0A020200;  // pitch 512 < x1 640
    CHECK(refused(r, "outside the surface pitch"));
    r = Fm4Resolve();
    for (int k = 0; k < 6; k++)
        r.vertexWords[k] = GuestFloat(0.0f);
    {
        const ResolveInfo e = DecodeResolve(r);  // a zero-area rectangle: nothing to copy or clear, as X (not a refusal)
        CHECK(e.ok && e.empty);
    }
    r = Fm4Resolve(), r.copyControl = 1u << 20 | 4;  // depth source: decoded (TestDepthResolve)
    CHECK(DecodeResolve(r).ok && DecodeResolve(r).isDepth);
    r = Fm4Resolve(), r.copyDestInfo = 2 | 6u << 7 | 1u << 3;  // array
    CHECK(refused(r, "3D"));
    r = Fm4Resolve(), r.copyDestInfo = 4 | 6u << 7;  // endian 8in64
    CHECK(refused(r, "endian 4"));
    r = Fm4Resolve(), r.copyDestInfo = 2 | 32u << 7;  // k_16_16_16_16_FLOAT
    CHECK(refused(r, "texture format 32"));
    r = Fm4Resolve(), r.copyDestInfo = 2 | 6u << 7 | 1u << 16;  // exp bias 1: decoded (X's full resolve applies it)
    CHECK(DecodeResolve(r).ok && DecodeResolve(r).destExpBias == 1);
    r = Fm4Resolve(), r.copyDestInfo = 2 | 6u << 7 | 1u << 13;  // signed fraction
    CHECK(refused(r, "number format 1"));
    {
        // FM4's k_32_FLOAT luminance resolve (the local capture draw_loop2 (not distributed) draw 31424): surface 00800050 (pitch 80, 1x),
        // colour 000E0000 (format 14), RB_COPY_CONTROL 00100100, DEST_BASE 08D4F000, DEST_PITCH 00200020 (32x32),
        // DEST_INFO 0000F202 = endian 2 | format 0x24 (36) << 7 | number 7 << 13; scissor 32x32; rectangle (0,0)-(32,32).
        ResolveRegisters lum = Fm4Resolve();
        lum.surfaceInfo = 0x00800050, lum.colorInfo[0] = 0x000E0000, lum.copyControl = 0x00100100, lum.copyDestBase = 0x08D4F000;
        lum.copyDestPitch = 0x00200020, lum.copyDestInfo = 0x0000F202, lum.windowScissorBr = 0x00200020;
        const float rect[6] = {0.0f, 0.0f, 32.0f, 0.0f, 32.0f, 32.0f};
        for (int k = 0; k < 6; k++)
            lum.vertexWords[k] = GuestFloat(rect[k]);
        const ResolveInfo li = DecodeResolve(lum);
        CHECK(li.ok && li.destFormat == 36 && li.x1 == 32 && li.y1 == 32);
        lum.copyDestInfo = 2 | 6u << 7 | 7u << 13;  // CONTROL: number 7 (float) on a k_8_8_8_8 destination stays refused
        CHECK(refused(lum, "number format 7"));
    }
    r = Fm4Resolve(), r.copyDestPitch = 0 | 360u << 16;
    CHECK(refused(r, "pitch is 0"));
    r = Fm4Resolve(), r.colorInfo[0] = 0x00050000;  // 64 bpp source
    CHECK(refused(r, "64 bpp"));
    r = Fm4Resolve(), r.colorInfo[0] = 0x00080000;  // unknown source format
    CHECK(refused(r, "unknown"));
    // A 7e3 source (format 12 keys storage 3) decodes; ForzaRecomp's OBSERVED FM4 menu resolve shape (its
    // tests/video/test_renderer.cpp:1210-1215, a HYPOTHESIS for our disc): 4x, k0123, into k_2_10_10_10 (7) with exp bias
    // -1 (bits 16-21 = 0x3F), swap, endian 2. The refusal names the whole shape (one stop records it all).
    r = Fm4Resolve(), r.colorInfo[0] = 0x000C0000;
    CHECK(DecodeResolve(r).ok && DecodeResolve(r).source == Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float));
    r.copyDestInfo = 2 | 7u << 7 | 0x3Fu << 16 | 1u << 24;
    CHECK(DecodeResolve(r).ok && DecodeResolve(r).destExpBias == -1 && DecodeResolve(r).destSwap && DecodeResolve(r).destEndian == 2);
    // A destination refusal still names the whole shape (here the 3D bit added).
    r.copyDestInfo |= 1u << 3;
    CHECK(refused(r, "(RB_COLOR_INFO format 12, storage 3, 4x, sample select 6) to texture format 7 (number 0, exp bias -1, swap 1, endian 2, "
                     "command 1): a resolve into a 3D (array) destination"));
    // The accepted destination formats: 7 (k_2_10_10_10), 50 and 54 (the _AS_16_16_16_16 variants: xenos.h:607-608).
    for (uint32_t f : {7u, 50u, 54u})
    {
        r = Fm4Resolve(), r.copyDestInfo = 2 | f << 7;
        CHECK(DecodeResolve(r).ok);
    }
    // FM4's 16_16 resolve: RB_COLOR_INFO format 4 (bits 16-19), 4x, k0123,
    // into texture format 25 (k_16_16, X xenos.h:599) with number 3 (kSignedInteger, xenos.h:221), exp bias 10, no swap,
    // endian 1: DEST_INFO = 1 | 25 << 7 | 3 << 13 | 10 << 16 (X registers.h:812-826).
    r = Fm4Resolve(), r.colorInfo[0] = 0x00040000, r.copyDestInfo = 1 | 25u << 7 | 3u << 13 | 10u << 16;
    {
        const ResolveInfo rg = DecodeResolve(r);
        CHECK(rg.ok && rg.source == Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16) && rg.sampleSelect == SampleSelect::k0123 &&
              rg.destFormat == 25 && rg.destExpBias == 10 && !rg.destSwap && rg.destEndian == 1);
        // k_16_16 is 32 bpp (X texture_info_formats.inl:26): the same extent as the k_8_8_8_8 copy (TestResolveDecode: 983040).
        CHECK(rg.destExtentLength == 983040);
    }
    // CONTROL: 25 from a source that is not 16_16 (Fm4Resolve's 2_10_10_10) stays refused.
    r = Fm4Resolve(), r.copyDestInfo = 1 | 25u << 7 | 3u << 13 | 10u << 16;
    CHECK(refused(r, "texture format 25 is not supported"));
    // CONTROL: number 3 is accepted only with format 25 (a 16_16 source into k_8_8_8_8 with number 3 stays refused).
    r = Fm4Resolve(), r.colorInfo[0] = 0x00040000, r.copyDestInfo = 2 | 6u << 7 | 3u << 13;
    CHECK(refused(r, "number format 3"));
}

// Depth resolves (X draw_util.cc GetResolveInfo's depth path; no trace holds one yet: the local capture clip240_1 (not distributed) and clip240_2
// stop at it without a CP trace, window4 stops before it). FM4's 640x360 4x scene with its kD24FS8 depth at 720.
void TestDepthResolve()
{
    auto refused = [](ResolveRegisters rr, const char* text) {
        const ResolveInfo x = DecodeResolve(rr);
        return !x.ok && x.refusal.find(text) != std::string::npos;
    };
    ResolveRegisters r = Fm4Resolve();
    r.copyControl = 1u << 20 | 6u << 4 | 4;  // convert, sample select k0123, copy_src_select 4
    ResolveInfo i = DecodeResolve(r);
    // SanitizeCopySampleSelect for 4x depth: k01 and k0123 -> k0, k23 -> k2 (single samples pass).
    CHECK(i.ok && i.isDepth && i.sourceIndex == 4 && i.source == kAd && i.sampleSelect == SampleSelect::k0);
    // The destination format is the depth buffer's (k_24_8_FLOAT = 23 for kD24FS8, X texture_info.h:170-180), not
    // RB_COPY_DEST_INFO's 6; no bias, no swap; the endian is RB_COPY_DEST_INFO's (2).
    CHECK(i.copy && i.destFormat == 23 && i.destExpBias == 0 && !i.destSwap && i.destEndian == 2 && i.destPitch == 640 && i.destHeight == 360);
    // 32 bpp like the colour case: the same destination base and extent (TestResolveDecode: 983040 bytes).
    CHECK(i.destBase == 0x00800000u && i.destExtentStart == 0x00800000u && i.destExtentLength == 983040);
    // The span from the depth base (X :1222-1232: depth_base + edram_base_offset_tiles): 720, 16 tiles a row, 45 rows.
    CHECK(i.spanBase == 720 && i.spanRowLength == 16 && i.spanRows == 45 && i.spanPitch == 16);
    r.copyControl = 1u << 20 | 5u << 4 | 4;  // k23 -> k2
    CHECK(DecodeResolve(r).sampleSelect == SampleSelect::k2);
    r.copyControl = 1u << 20 | 1u << 4 | 4;  // k1 stays
    CHECK(DecodeResolve(r).sampleSelect == SampleSelect::k1);
    // kD24S8 -> k_24_8 (22).
    r = Fm4Resolve(), r.copyControl = 1u << 20 | 4, r.depthInfo = 0x2D0;
    i = DecodeResolve(r);
    CHECK(i.ok && i.destFormat == 22 && i.source == Key::Depth(720, 16, Msaa::X4, DepthFormat::D24S8));
    // RB_COPY_DEST_INFO's format and number and the copy command do not matter for depth (X ignores them on this path).
    r = Fm4Resolve(), r.copyControl = 0u << 20 | 4, r.copyDestInfo = 2 | 50u << 7 | 3u << 13;
    i = DecodeResolve(r);
    CHECK(i.ok && i.destFormat == 23 && i.command == CopyCommand::Raw);
    // Still refused: an array destination, a 64/128-bit endian, pitch 0.
    r = Fm4Resolve(), r.copyControl = 1u << 20 | 4, r.copyDestInfo = 2 | 6u << 7 | 1u << 3;
    CHECK(refused(r, "3D"));
    r = Fm4Resolve(), r.copyControl = 1u << 20 | 4, r.copyDestInfo = 4 | 6u << 7;
    CHECK(refused(r, "endian 4"));
    r = Fm4Resolve(), r.copyControl = 1u << 20 | 4, r.copyDestPitch = 360u << 16;
    CHECK(refused(r, "pitch is 0"));

    // The plan: after FM4's scene binding kAd holds tiles 720-1439; the copy reads it with no transfer. With colour and
    // depth clears requested, only the depth clear happens (X draw_util.h: a depth resolve cannot clear colour).
    Ownership o;
    CHECK(Bind(o, Fm4Bind(), 360).ok);
    r = Fm4Resolve(), r.copyControl = 1u << 20 | 4 | 1u << 8 | 1u << 9, r.depthClear = 0xF0000000u;
    i = DecodeResolve(r);
    ResolvePlan p = PrepareResolve(o, i);
    CHECK(p.ok && p.copy && p.copySource == kAd && p.copyRect == Rect{0, 0, 640, 360} && p.copyTransfers.empty());
    CHECK(p.hasDepthClear && !p.hasColorClear && p.depthClear.target == kAd && p.depthClear.rect == Rect{0, 0, 640, 360});
}

// FM4's k_8_8 resolve: colour 0 of RB_COLOR_INFO format 0 (8_8_8_8, storage 0),
// 1x, sample select 0, into texture format 10 (k_8_8, X xenos.h:593) with number 0, exp bias 0, no swap, endian 1 (8in16),
// command 1. The run logs no raw registers: Fm4Resolve's 640x360 rectangle stands in, on a 1x surface (0x0A000280: pitch 640,
// msaa_samples 0), DEST_INFO 1 | 10 << 7 (X registers.h:812-826).
void TestK88ResolveDecode()
{
    auto refused = [](ResolveRegisters rr, const char* text) {
        const ResolveInfo x = DecodeResolve(rr);
        return !x.ok && x.refusal.find(text) != std::string::npos;
    };
    ResolveRegisters r = Fm4Resolve();
    r.surfaceInfo = 0x0A000280, r.colorInfo[0] = 0, r.copyControl = 1u << 20, r.copyDestInfo = 1 | 10u << 7;
    const ResolveInfo k = DecodeResolve(r);
    CHECK(k.ok && k.msaa == Msaa::X1 && k.source.Format() == uint32_t(ColorFormat::k8_8_8_8) && k.sampleSelect == SampleSelect::k0 &&
          k.destFormat == 10 && k.destEndian == 1 && k.destExpBias == 0 && !k.destSwap && k.destBytesLog2 == 1);
    // The extent with bpp_log2 1 (X draw_util.cc:1133-1180): lower bound TiledOffset2D(0, 0) = 0; upper bound (texture_util.cc
    // :490-517) the tile of (639, 359) -> (608, 352): macro = (19 + 11 * 20) << (1 + 7) = 61184 = 119 * 512 + 256 (:448-449, pitch
    // 640 / 32 = 20); micro 0; offset 61184; mix (:456-457): (61184 & ~0x1FF) << 3 = 487424, (352 & 16) << 7 = 0,
    // (61184 & 0x1C0) << 2 = 1024, ((((352 & 8) >> 2) + (608 >> 3)) & 3) << 6 = (0 + 76) & 3 = 0, & 0x3F = 0 -> 488448;
    // + 0xC00 (16 bpp, :507-511) = 491520, half the k_8_8_8_8 copy's 983040 (TestResolveDecode).
    CHECK(k.destBase == 0x00800000u && k.destExtentStart == k.destBase && k.destOffsetX == 0 && k.destOffsetY == 0 && k.destExtentLength == 491520);
    CONTROL(k.destExtentLength == 983040);  // the 32 bpp extent
    {
        ResolveRegisters r32 = r;
        r32.copyDestInfo = 1 | 6u << 7;  // CONTROL: the same copy into k_8_8_8_8 keeps the 32 bpp layout
        const ResolveInfo w = DecodeResolve(r32);
        CHECK(w.ok && w.destBytesLog2 == 2 && w.destExtentLength == 983040);
    }
    // An origin at x0 = 32 (vertex x 31.5 + 0.5, X draw_util.cc:801-807): the 16 bpp base granularity is 64 texels
    // (xenos.h:1188-1195: 7 - min(2, 1) = 6), so the base stays the (0, 0) tile and the offset is 32; the lower bound is the
    // 32x32 tile of (32, 0) (texture_util.h:297-303): TiledOffset2D(32, 0, 640, 1): macro 1 << 8 = 256, offset 256 ->
    // (256 & 0x1C0) << 2 = 1024, ((0 + 4) & 3) << 6 = 0 -> 1024. Length 491520 - 1024 = 490496.
    const float v32[6] = {31.5f, -0.5f, 639.5f, -0.5f, 639.5f, 359.5f};
    for (int i = 0; i < 6; i++)
        r.vertexWords[i] = GuestFloat(v32[i]);
    const ResolveInfo o = DecodeResolve(r);
    CHECK(o.ok && o.x0 == 32 && o.destOffsetX == 32 && o.destBase == 0x00800000u && o.destExtentStart == 0x00800000u + 1024 &&
          o.destExtentLength == 490496);
    {
        // CONTROL: at 32 bpp the granularity is 32 texels: offset 0, base = extent start = TiledOffset(32, 0, 640, 2):
        // macro 1 << 9 = 512 -> (512 & ~0x1FF) << 3 = 4096.
        ResolveRegisters r32 = r;
        r32.copyDestInfo = 1 | 6u << 7;
        const ResolveInfo w = DecodeResolve(r32);
        CHECK(w.ok && w.destOffsetX == 0 && w.destBase == 0x00800000u + 4096 && w.destExtentStart == w.destBase);
        CONTROL(o.destOffsetX == w.destOffsetX);
    }
    // XeEndianSwap16 swaps only under 8in16 (endian.xesli:25-31): 8in32 (2) and 16in32 (3) on k_8_8 are refused by name.
    r = Fm4Resolve(), r.surfaceInfo = 0x0A000280, r.colorInfo[0] = 0, r.copyControl = 1u << 20, r.copyDestInfo = 2 | 10u << 7;
    CHECK(refused(r, "endian 2 on a 16 bpp destination"));
    r.copyDestInfo = 3 | 10u << 7;
    CHECK(refused(r, "endian 3 on a 16 bpp destination"));
    r.copyDestInfo = 0 | 10u << 7;  // no swap: decoded
    CHECK(DecodeResolve(r).ok);
    r.copyDestInfo = 1 | 10u << 7 | 1u << 13;  // CONTROL: a non-zero number format on k_8_8 stays refused
    CHECK(refused(r, "number format 1"));
}

void TestResolvePlan()
{
    // After FM4's scene binding, the whole-target resolve reads kAc's (0, 0, 640, 360): every span tile is its.
    Ownership o;
    CHECK(Bind(o, Fm4Bind(), 360).ok);
    ResolveInfo i = DecodeResolve(Fm4Resolve());
    ResolvePlan p = PrepareResolve(o, i);
    CHECK(p.ok && p.copy && p.copySource == kAc && p.copyRect == Rect{0, 0, 640, 360} && !p.hasColorClear && !p.hasDepthClear);
    CHECK(p.copyTransfers.empty());
    // The 1x key holds tiles [0, 112): the source takes them back first (the 45 full rows coalesce into one
    // transfer of the foreign part), then the copy reads kAc alone.
    CHECK(Bind(o, Bind1x(), 100).ok);
    p = PrepareResolve(o, i);
    CHECK(p.ok && p.copySource == kAc && p.copyTransfers == T({{0, 112, kBc}}) && o.Count(kBc) == 0 && o.Count(kAc) == 720);
    // CONTROL: an unowned span tile is refused, nothing changes.
    Ownership empty;
    p = PrepareResolve(empty, i);
    CONTROL(p.ok);
    CHECK(p.refusal.find("no host target") != std::string::npos && empty.Count(kAc) == 0);
    // A sub-rectangle resolve reads only its span: tiles 98 + row * 16 + 0..4 for 16 rows; with kBc holding
    // [0, 112) only row 0 (98..102) is foreign: one transfer of those 5 tiles, the rest of kBc's tiles stay.
    CHECK(Bind(o, Bind1x(), 100).ok);
    ResolveRegisters r = Fm4Resolve();
    const float v[6] = {99.5f, 49.5f, 259.5f, 49.5f, 259.5f, 169.5f};
    for (int k = 0; k < 6; k++)
        r.vertexWords[k] = GuestFloat(v[k]);
    const ResolveInfo sub = DecodeResolve(r);
    CHECK(sub.ok);
    p = PrepareResolve(o, sub);
    CHECK(p.ok && p.copyRect == Rect{96, 48, 168, 128} && p.copyTransfers == T({{98, 103, kBc}}) && o.Count(kBc) == 107 && o.At(97) == kBc && o.At(103) == kBc);
    CONTROL(o.At(98) == kBc);
    CHECK(Bind(o, Fm4Bind(), 360).ok);
    p = PrepareResolve(o, sub);
    CHECK(p.ok && p.copyRect == Rect{96, 48, 168, 128} && p.copyTransfers.empty());
    // A resolve of a region kBc does not touch (rows 120.. at 4x = tiles 240..) passes while kBc holds [0, 112).
    CHECK(Bind(o, Bind1x(), 100).ok);
    const float low[6] = {-0.5f, 119.5f, 639.5f, 119.5f, 639.5f, 359.5f};
    for (int k = 0; k < 6; k++)
        r.vertexWords[k] = GuestFloat(low[k]);
    const ResolveInfo lower = DecodeResolve(r);
    CHECK(lower.ok && lower.y0 == 120 && lower.spanBase == 240 && lower.spanRows == 30);  // 120 px at 4x = 240 samples = 15 tile rows -> tile 240; 240 rows / 8 = 30 tile rows
    p = PrepareResolve(o, lower);
    CHECK(p.ok && p.copyRect == Rect{0, 120, 640, 240});

    // Resolve clears (X render_target_cache.cc:1160-1360). The whole-target colour clear: rectangle (0, 0, 640,
    // 360) of kAc; tiles start 0, length ((359 << 1) / 16) * 16 + (639 << 1) / 80 + 1 = 704 + 15 + 1 = 720.
    Ownership c;
    CHECK(Bind(c, Fm4Bind(), 360).ok);
    r = Fm4Resolve();
    r.copyControl |= 1u << 8;
    r.colorClear = 0x3FF00000;
    ResolveInfo ci = DecodeResolve(r);
    p = PrepareResolve(c, ci);
    CHECK(p.ok && p.copy && p.hasColorClear && !p.hasDepthClear);
    CHECK(p.colorClear.target == kAc && p.colorClear.rect == Rect{0, 0, 640, 360} && p.colorClear.value == 0x3FF00000u && p.colorClear.transfers.empty());
    // The same clear while the 1x key holds [0, 112) of the colour tiles: the copy's transfer takes them first,
    // so the clear's own ownership change finds nothing foreign (X's cutout would have skipped the transfer of
    // the cleared area: NOTES.md §2).
    CHECK(Bind(c, Bind1x(), 100).ok);
    p = PrepareResolve(c, ci);
    CHECK(p.ok && p.copyTransfers == T({{0, 112, kBc}}) && p.hasColorClear && p.colorClear.transfers.empty() && c.Count(kBc) == 0);
    // The sub-rectangle with both clears (depth 1.0 = 0xF00000 << 8): the rectangle (96, 48, 168, 128) of
    // kAc and of kAd; tiles from (48 << 1) / 16 * 16 + (96 << 1) / 80 = 98, length ((175 << 1) / 16) * 16 +
    // (263 << 1) / 80 + 1 - 98 = 336 + 6 + 1 - 98 = 245; the overlap rule (:1290-1313): depth wrapped start
    // 720 + 98 = 818, colour 98: depth length min(245, 2048 + 98 - 818) = 245, colour min(245, 818 - 98) = 245.
    Ownership d;
    CHECK(Bind(d, Fm4Bind(), 360).ok);
    r = Fm4Resolve();
    for (int k = 0; k < 6; k++)
        r.vertexWords[k] = GuestFloat(v[k]);
    r.copyControl |= 1u << 8 | 1u << 9;
    r.colorClear = 0x12345678, r.depthClear = 0xF0000000;
    ResolveInfo di = DecodeResolve(r);
    p = PrepareResolve(d, di);
    CHECK(p.ok && p.hasColorClear && p.hasDepthClear);
    CHECK(p.colorClear.target == kAc && p.colorClear.rect == Rect{96, 48, 168, 128} && p.colorClear.value == 0x12345678u);
    CHECK(p.depthClear.target == kAd && p.depthClear.rect == Rect{96, 48, 168, 128} && p.depthClear.value == 0xF0000000u);
    CHECK(p.colorClear.transfers.empty() && p.depthClear.transfers.empty());
    // The cleared tiles change owner (X :1347-1359): with the 1x depth key holding [720, 832), the depth clear
    // of tiles 818..1062 transfers [818, 832) from kBd first (no cutout: NOTES.md); the colour clear's tiles
    // 98..342 come from kBc for [98, 112). The copy's span (98 + 16 rows x 5) overlaps kBc's tiles, so the copy
    // is refused: use a clear-only shape instead by resolving a region kBc does not hold... the copy always
    // happens (NOTES.md), so first give the colour tiles back and keep only the depth tiles foreign.
    CHECK(Bind(d, Bind1x(2, 0), 100).ok);  // depth only: kBd takes [720, 832)
    p = PrepareResolve(d, di);
    // With the cutout: the foreign tiles 818..831 are kAd's row 6 (local 96..111), columns 2..15 of 40x8 pixels at
    // y 48..56; the clear rectangle x 96..264 contains columns 3, 4, 5 (x 120..240) entirely, so those three
    // tiles (819, 820, 821) are claimed without a transfer; column 2 (x 80..120) and columns 6..15 (x 240..640)
    // reach outside it and transfer: {818, 819} and {822, 832}, 11 tiles.
    CHECK(p.ok && p.depthClear.transfers == T({{818, 819, kBd}, {822, 832, kBd}}) && p.colorClear.transfers.empty());
    CHECK(d.At(818) == kAd && d.At(831) == kAd && d.At(817) == kBd && d.Count(kBd) == 98);
    // A 20e4 depth clear above 1.0 is refused (X xenos.cc:150-166: 0xF00001 > 1.0), leaving the map unchanged.
    r.depthClear = 0xF0000100;
    const Ownership before = d;
    p = PrepareResolve(d, DecodeResolve(r));
    CONTROL(p.ok);
    CHECK(p.refusal.find("above 1.0") != std::string::npos && d.Count(kBd) == before.Count(kBd));
    // kD24S8 takes any value (unorm): 0xFFFFFF00.
    Ownership u;
    BindRegisters ub = Fm4Bind();
    ub.depthInfo = 0x000002D0;  // kD24S8 at 720
    CHECK(Bind(u, ub, 360).ok);
    r.depthInfo = 0x000002D0, r.depthClear = 0xFFFFFF00;
    p = PrepareResolve(u, DecodeResolve(r));
    CHECK(p.ok && p.hasDepthClear && p.depthClear.target == Key::Depth(720, 16, Msaa::X4, DepthFormat::D24S8));
    // The clear rectangle's height clamp (X :1239-1242, rtHeight - y): a 1x surface of pitch 16 has host height
    // 2048, so a rectangle at row 2040 (vertices 2039.5 .. 2047.5) of 8 rows is inside, 2048 - 2040 = 8 high. The
    // tiles 2032..2039 are only claimed by a colour-only draw of 2048 rows (with a depth target the distance
    // rule stops the claim at 720, and the resolve would read unowned tiles).
    Ownership h;
    CHECK(Bind(h, Bind1x(0, 0xF), 2048).ok && h.Count(kBc) == 2048);
    r = Fm4Resolve();
    r.surfaceInfo = 1280, r.copyControl = 1u << 20 | 1u << 8;
    const float tall[6] = {-0.5f, 2039.5f, 639.5f, 2039.5f, 639.5f, 2047.5f};
    for (int k = 0; k < 6; k++)
        r.vertexWords[k] = GuestFloat(tall[k]);
    const ResolveInfo ti = DecodeResolve(r);
    CHECK(ti.ok && ti.y0 == 2040 && ti.y1 == 2048 && ti.spanBase == 127 * 16 && ti.spanRows == 1);
    p = PrepareResolve(h, ti);
    CHECK(p.ok && p.hasColorClear && p.colorClear.rect == Rect{0, 2040, 640, 8});
    // An undecoded resolve is refused.
    p = PrepareResolve(h, ResolveInfo{});
    CONTROL(p.ok);
}

// Replay mode (argv[1] = a CP trace, e.g. the local capture trace12 (not distributed)): every mode-4/5 draw goes
// through Bind with the window scissor's bottom as its rows and the line's RB_COLOR_MASK, RB_DEPTHCONTROL,
// RB_DEPTH_INFO, RB_COLOR_INFO and RB_COLOR1_INFO (RB_COLOR2/3_INFO are not traced: 0). A mode-6 draw is decoded
// once its `resolve copy` line arrives (cp_trace.cpp: RB_COPY_CONTROL, DEST_BASE, DEST_PITCH, DEST_INFO,
// RB_DEPTH_CLEAR, RB_COLOR_CLEAR, RB_COLOR_CLEAR_LO, PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL, the six words
// of fetch constant 0) through DecodeResolve + PrepareResolve with those registers; only the rectangle's three
// vertices are not in the trace (they live at the fetch constant's address), so the whole surface within the
// window scissor stands in for them, with the fetch constant's own endian. A draw with no `resolve copy` line
// (trace10's format) gets the synthesized destination of the first replay. `tex` lines (the draw's texture fetch
// constants, front_buffer.h layout: base bits 12-31 and format bits 0-5, endian 6-7 of the second word) are
// matched against earlier resolve destinations: the fetch must read the same bytes in a bitwise-equivalent
// format (X xenos.h:616-641) with the same endian. Prints the counts and every distinct resolve configuration
// decoded; exit 1 on any refusal or mismatch.
struct ReplayState
{
    Ownership o;
    uint64_t draws = 0, bound = 0, unbound = 0, noRaster = 0, resolves = 0, refusals = 0, unparsed = 0, texLines = 0, texHits = 0, texMismatches = 0;
    uint64_t transferTiles[2] = {}, transferCount[2] = {}, resolveTransfers = 0, resolveTransferTiles = 0, synthesized = 0, fetchNotVertex = 0;
    uint64_t clearTransfers[2] = {}, clearTransferTiles[2] = {};  // the resolve clears' transfers (0 colour, 1 depth)
    bool pending = false;  // a mode-6 draw waiting for its `resolve copy` line
    unsigned long long pendingDraw = 0;
    // A pending bind whose extent needs its vertices waits for its `vf0` line (cp_trace.cpp: vf0's first three
    // 7-dword vertices of a clip-disabled 8192x8192-scissor draw); `vf0Bound` counts the binds that used one.
    bool awaitVf0 = false;
    uint64_t vf0Bound = 0, crossAspect = 0, crossAspectTiles = 0;
    std::vector<std::string> crossAspectLog;
    uint64_t otherBase = 0, otherBaseTiles = 0;  // same-aspect transfers between keys of different bases
    std::vector<std::string> otherBaseLog;       // the first 20
    uint64_t pitchOrF32 = 0;                     // transfers between pitches or with a 32_FLOAT key
    std::vector<std::string> pitchOrF32Log;      // the first 20
    ResolveRegisters pendingRegs;
    // A mode-4/5 draw waiting for its `state` line (window offset, scissor TL, PA_SU_VTX_CNTL, viewport: the extent).
    bool bindPending = false;
    unsigned long long bindDraw = 0;
    BindRegisters bindRegs;
    ExtentRegisters bindExtent;
    const uint32_t* vertexMaxY = nullptr;  // argv: the hypothesis for draws whose extent needs their vertices
    uint64_t needVertices = 0, withState = 0, heightLimited = 0;
    struct Config
    {
        unsigned copyControl, destPitch, destInfo, depthClear, colorClear, colorClearLo, windowOffset, windowScissorTl, fetch1;
        uint64_t count = 0;
        std::vector<uint32_t> destBases;
        std::vector<uint64_t> destBaseCounts;
        ResolveInfo decoded;  // of the first occurrence
        std::string planRefusal;
    };
    std::vector<Config> configs;
    struct Dest
    {
        uint32_t base, extentStart, extentLength, format, endian;
    };
    std::vector<Dest> dests;  // every distinct resolve destination
    // Every distinct (source key, destination key) transfer pair, with its count and first draw (binds and resolves), so
    // that the Vulkan unit's classification can be checked against exactly the traced pairs (test_edram_vk).
    struct Pair
    {
        uint32_t source, dest;
        uint64_t count;
        unsigned long long firstDraw;
    };
    std::vector<Pair> pairs;
};

void NotePair(ReplayState& s, Key source, Key dest, unsigned long long draw)
{
    for (auto& p : s.pairs)
        if (p.source == source.packed && p.dest == dest.packed)
            return void(p.count++);
    s.pairs.push_back({source.packed, dest.packed, 1, draw});
}

void ReplayResolve(ReplayState& s, unsigned long long n, ResolveRegisters rr, bool fromTrace)
{
    // The rectangle: the whole surface within the window scissor's bottom-right (the vertices are not traced).
    const float w = float(std::min(DecodeSurfaceInfo(rr.surfaceInfo).pitch, rr.windowScissorBr & 0x3FFF));
    const float h = float((rr.windowScissorBr >> 16) & 0x3FFF);
    const float v[6] = {-0.5f, -0.5f, w - 0.5f, -0.5f, w - 0.5f, h - 0.5f};
    for (int k = 0; k < 6; k++)
    {
        uint32_t bits;
        std::memcpy(&bits, &v[k], 4);
        rr.vertexWords[k] = GpuSwap32(bits, rr.fetch1 & 3);  // stored so that DecodeResolve's swap gives the float back
    }
    if ((rr.fetch0 & 3) != 3 || ((rr.fetch1 >> 2) & 0xFFFFFF) != 6)
        s.fetchNotVertex++;
    const ResolveInfo i = DecodeResolve(rr);
    const ResolvePlan p = PrepareResolve(s.o, i);
    s.resolves++;
    s.synthesized += !fromTrace;
    if (!p.ok)
        s.refusals++, std::printf("draw %llu: resolve refused: %s\n", n, p.refusal.c_str());
    for (const Transfer& t : p.copyTransfers)
        s.resolveTransfers++, s.resolveTransferTiles += t.endTiles - t.startTiles, NotePair(s, t.source, p.copySource, n);
    for (const Transfer& t : p.depthClear.transfers)
        s.clearTransfers[1]++, s.clearTransferTiles[1] += t.endTiles - t.startTiles, NotePair(s, t.source, p.depthClear.target, n);
    for (const Transfer& t : p.colorClear.transfers)
        s.clearTransfers[0]++, s.clearTransferTiles[0] += t.endTiles - t.startTiles, NotePair(s, t.source, p.colorClear.target, n);
    // Record the configuration.
    ReplayState::Config* c = nullptr;
    for (auto& k : s.configs)
        if (k.copyControl == rr.copyControl && k.destPitch == rr.copyDestPitch && k.destInfo == rr.copyDestInfo && k.depthClear == rr.depthClear &&
            k.colorClear == rr.colorClear && k.colorClearLo == rr.colorClearLo && k.windowOffset == rr.windowOffset && k.windowScissorTl == rr.windowScissorTl &&
            k.fetch1 == rr.fetch1)
            c = &k;
    if (!c)
    {
        s.configs.push_back({rr.copyControl, rr.copyDestPitch, rr.copyDestInfo, rr.depthClear, rr.colorClear, rr.colorClearLo, rr.windowOffset,
                             rr.windowScissorTl, rr.fetch1, 0, {}, {}, i, p.ok ? "" : p.refusal});
        c = &s.configs.back();
    }
    c->count++;
    size_t bi = 0;
    while (bi < c->destBases.size() && c->destBases[bi] != rr.copyDestBase)
        bi++;
    if (bi == c->destBases.size())
        c->destBases.push_back(rr.copyDestBase), c->destBaseCounts.push_back(0);
    c->destBaseCounts[bi]++;
    if (i.ok && i.copy)
    {
        bool known = false;
        for (const auto& d : s.dests)
            known |= d.base == rr.copyDestBase && d.format == i.destFormat;
        if (!known)
            s.dests.push_back({rr.copyDestBase, i.destExtentStart, i.destExtentLength, i.destFormat, i.destEndian});
    }
}

// X xenos.h:616-641: the texture formats that read a resolve destination's bytes unchanged.
bool FetchReadsResolve(uint32_t destFormat, uint32_t fetchFormat)
{
    if (destFormat == 6 || destFormat == 50)
        return fetchFormat == 6 || fetchFormat == 50;
    if (destFormat == 7 || destFormat == 54)
        return fetchFormat == 7 || fetchFormat == 54;
    return destFormat == fetchFormat;
}

// The deferred bind of a mode-4/5 draw: the height is EstimateMaxY (X draw_extent_estimator.cc:267-366) of the
// draw line's clip/sumode/vte and, when it arrived, the state line's window offset, scissor TL, PA_SU_VTX_CNTL and
// viewport; the vertex bound only from the command line's hypothesis (the trace has no vertex data).
void ReplayBind(ReplayState& s, const uint32_t* vertexMaxY = nullptr)
{
    if (!s.bindPending)
        return;
    s.bindPending = false, s.awaitVf0 = false;
    const ExtentRegisters& e = s.bindExtent;
    const bool needs = NeedsVertexMaxY(e);
    s.needVertices += needs;
    s.vf0Bound += vertexMaxY != nullptr;
    const uint32_t height = EstimateMaxY(e, vertexMaxY ? vertexMaxY : s.vertexMaxY);
    s.heightLimited += height < ((e.windowScissorBr >> 16) & 0x3FFF);
    const Binding b = Bind(s.o, s.bindRegs, height);
    if (!b.ok)
    {
        s.refusals++, std::printf("draw %llu: bind refused (height %u%s): %s\n", s.bindDraw, height, needs ? ", extent needs the vertices" : "", b.refusal.c_str());
        return;
    }
    if (!b.rasterizes)
        s.noRaster++;
    else if (b.depth.Empty() && b.color[0].Empty() && b.color[1].Empty() && b.color[2].Empty() && b.color[3].Empty())
        s.unbound++;
    else
        s.bound++;
    for (uint32_t i = 0; i < 5; i++)
        for (const Transfer& t : b.transfers[i])
        {
            s.transferCount[i == 0]++, s.transferTiles[i == 0] += t.endTiles - t.startTiles;
            const Key dest = i == 0 ? b.depth : b.color[i - 1];
            if (t.source.IsDepth() != dest.IsDepth())
            {
                s.crossAspect++, s.crossAspectTiles += t.endTiles - t.startTiles;
                char text[160];
                std::snprintf(text, sizeof text, "draw %llu (height %u): tiles %u-%u from key %06X to key %06X", s.bindDraw, height, t.startTiles, t.endTiles - 1,
                              t.source.packed, dest.packed);
                s.crossAspectLog.push_back(text);
            }
            else if (t.source.Base() != dest.Base())  // same aspect, another base (MapEdramTexel's tile offset)
            {
                s.otherBase++, s.otherBaseTiles += t.endTiles - t.startTiles;
                char text[200];
                std::snprintf(text, sizeof text, "draw %llu (height %u): tiles %u-%u from key %06X (base %u, %ux) to key %06X (base %u, %ux)", s.bindDraw,
                              height, t.startTiles, t.endTiles - 1, t.source.packed, t.source.Base(), 1u << uint32_t(t.source.MsaaMode()), dest.packed,
                              dest.Base(), 1u << uint32_t(dest.MsaaMode()));
                if (s.otherBaseLog.size() < 20)
                    s.otherBaseLog.push_back(text);
            }
            // Different pitches (X's separate source pitch, vulkan_render_target_cache.cc:2956-2965) or a 32_FLOAT key
            // (storage 14), listed whatever the aspect.
            if (t.source.PitchTilesAt32bpp() != dest.PitchTilesAt32bpp() || (!t.source.IsDepth() && t.source.Format() == 14) ||
                (!dest.IsDepth() && dest.Format() == 14))
            {
                char text[200];
                std::snprintf(text, sizeof text, "draw %llu (height %u): tiles %u-%u from key %06X (pitch %u, format %u) to key %06X (pitch %u, format %u)",
                              s.bindDraw, height, t.startTiles, t.endTiles - 1, t.source.packed, t.source.PitchTilesAt32bpp(), t.source.Format(), dest.packed,
                              dest.PitchTilesAt32bpp(), dest.Format());
                s.pitchOrF32++;
                if (s.pitchOrF32Log.size() < 20)
                    s.pitchOrF32Log.push_back(text);
            }
            NotePair(s, t.source, dest, s.bindDraw);
        }
}

int Replay(const char* path, const uint32_t* vertexMaxY)
{
    FILE* f = std::fopen(path, "r");
    if (!f)
        return std::printf("cannot open %s\n", path), 2;
    ReplayState s;
    s.vertexMaxY = vertexMaxY;
    char line[1024];
    while (std::fgets(line, sizeof line, f))
    {
        if (std::strncmp(line, "state ", 6) == 0)
        {
            unsigned viz, vizq, outpath, interp, vsc, psc, wo, wst, vtx, sref, xs, xo, ys, yo, zs, zo;
            if (std::sscanf(line, "state viz %x %x outpath %x interp %x vsconst %x psconst %x window %x %x vtxcntl %x stencilref %x vport %x %x %x %x %x %x", &viz,
                            &vizq, &outpath, &interp, &vsc, &psc, &wo, &wst, &vtx, &sref, &xs, &xo, &ys, &yo, &zs, &zo) != 16)
            {
                s.unparsed++;
                continue;
            }
            if (s.bindPending)
            {
                s.bindExtent.windowOffset = wo, s.bindExtent.windowScissorTl = wst, s.bindExtent.vtxCntl = vtx;
                s.bindExtent.vportYScale = ys, s.bindExtent.vportYOffset = yo;
                s.withState++;
                if (NeedsVertexMaxY(s.bindExtent) && !s.vertexMaxY)
                    s.awaitVf0 = true;  // bound at its vf0 line, or by the scissor at any other line
                else
                    ReplayBind(s);
            }
            continue;
        }
        if (std::strncmp(line, "vf0 ", 4) == 0)
        {
            // "vf0 endian E at ADDR: w0 .. w20": the words as the guest stores them (cp_trace.cpp Load32, big-endian); the
            // renderer's vertex window holds the GPU's little-endian 32-bit loads of the same bytes, so each is swapped
            // back before PassThroughPositions applies the fetch's endian (as draw_record.cpp's extent path does).
            unsigned endian = 0, address = 0;
            int at = 0;
            std::vector<uint32_t> loads;
            if (std::sscanf(line, "vf0 endian %u at %x:%n", &endian, &address, &at) == 2)
                for (const char* p = line + at; loads.size() < 21;)
                {
                    char* next = nullptr;
                    const unsigned long w = std::strtoul(p, &next, 16);
                    if (next == p)
                        break;
                    loads.push_back(__builtin_bswap32(uint32_t(w)));
                    p = next;
                }
            std::vector<VertexPosition> positions;
            std::string why;
            if (loads.size() != 21 || !PassThroughPositions(loads.data(), loads.size(), endian, 7, 0, 3, positions, why))
            {
                s.unparsed++;
                continue;
            }
            if (s.bindPending && s.awaitVf0)
            {
                const uint32_t maxY = VertexMaxY(s.bindExtent, positions.data(), positions.size());
                ReplayBind(s, &maxY);
            }
            continue;
        }
        ReplayBind(s);  // any other line ends the previous draw (trace10/12 have no state line)
        if (std::strncmp(line, "resolve copy ", 13) == 0)
        {
            unsigned cc, db, dp, di, dc, col, colLo, wo, wst, f0, f1, f2, f3, f4, f5;
            if (std::sscanf(line, "resolve copy %x %x %x %x offset %x %x %x window %x %x fetch0 %x %x %x %x %x %x", &cc, &db, &dp, &di, &dc, &col, &colLo, &wo, &wst,
                            &f0, &f1, &f2, &f3, &f4, &f5) != 15 || !s.pending)
            {
                s.unparsed++;
                continue;
            }
            ResolveRegisters rr = s.pendingRegs;
            rr.copyControl = cc, rr.copyDestBase = db, rr.copyDestPitch = dp, rr.copyDestInfo = di;
            rr.depthClear = dc, rr.colorClear = col, rr.colorClearLo = colLo;
            rr.windowOffset = wo, rr.windowScissorTl = wst, rr.fetch0 = f0, rr.fetch1 = f1;
            s.pending = false;
            ReplayResolve(s, s.pendingDraw, rr, true);
            continue;
        }
        if (std::strncmp(line, "tex ", 4) == 0)
        {
            unsigned slot, d0, d1, d2, d3, d4, d5;
            if (std::sscanf(line, "tex %u %x %x %x %x %x %x", &slot, &d0, &d1, &d2, &d3, &d4, &d5) != 7)
            {
                s.unparsed++;
                continue;
            }
            s.texLines++;
            const uint32_t base = d1 & 0xFFFFF000u, format = d1 & 0x3F, endian = (d1 >> 6) & 3;
            for (const auto& d : s.dests)
                if (d.base == base)
                {
                    s.texHits++;
                    if (!FetchReadsResolve(d.format, format) || endian != d.endian)
                        s.texMismatches++, std::printf("tex %u reads resolve destination %08X as format %u endian %u (resolved as format %u endian %u)\n", slot, base,
                                                       format, endian, d.format, d.endian);
                }
            continue;
        }
        if (std::strncmp(line, "draw ", 5) != 0)
            continue;
        if (s.pending)  // trace10's format: no `resolve copy` line followed the mode-6 draw
        {
            ResolveRegisters rr = s.pendingRegs;
            rr.copyControl = 1u << 20 | (DecodeSurfaceInfo(rr.surfaceInfo).msaa == 2 ? 6u : 0u) << 4;
            rr.copyDestPitch = 1280 | 720u << 16, rr.copyDestInfo = 0 | 54u << 7, rr.copyDestBase = 0x00800000;
            rr.fetch0 = 3 | 0x00100000, rr.fetch1 = 2 | 6u << 2;
            s.pending = false;
            ReplayResolve(s, s.pendingDraw, rr, false);
        }
        unsigned long long n;
        unsigned op, surface, color, depth, color1, mode, scissor, initiator, colorMask, depthCtl;
        if (std::sscanf(line, "draw %llu op %x surface %x color %x depth %x color1 %x mode %x scissor_br %x initiator %x colormask %x depthctl %x", &n, &op,
                        &surface, &color, &depth, &color1, &mode, &scissor, &initiator, &colorMask, &depthCtl) != 11)
        {
            s.unparsed++;
            continue;
        }
        s.draws++;
        if ((mode & 7) == 6)
        {
            s.pendingRegs = ResolveRegisters{};
            s.pendingRegs.surfaceInfo = surface, s.pendingRegs.colorInfo[0] = color, s.pendingRegs.colorInfo[1] = color1, s.pendingRegs.depthInfo = depth;
            s.pendingRegs.modeControl = mode, s.pendingRegs.windowScissorBr = scissor, s.pendingRegs.screenScissorBr = 8192 | 8192u << 16;
            s.pending = true, s.pendingDraw = n;
            continue;
        }
        BindRegisters r;
        r.surfaceInfo = surface, r.modeControl = mode, r.depthControl = depthCtl, r.colorMask = colorMask;
        r.depthInfo = depth, r.colorInfo[0] = color, r.colorInfo[1] = color1;
        s.bindRegs = r, s.bindDraw = n, s.bindPending = true;
        s.bindExtent = ExtentRegisters{};
        s.bindExtent.surfaceInfo = surface, s.bindExtent.windowScissorBr = scissor;
        unsigned blend0, colorCtl, clip, sumode, vte;
        // The draw line's tail (cp_trace.cpp: clip 2204, sumode 2205, vte 2206); trace10 lines have it too.
        const char* tail = std::strstr(line, " blend0 ");
        if (tail && std::sscanf(tail, " blend0 %x colorctl %x clip %x sumode %x vte %x", &blend0, &colorCtl, &clip, &sumode, &vte) == 5)
            s.bindExtent.clipCntl = clip, s.bindExtent.suScModeCntl = sumode, s.bindExtent.vteCntl = vte;
        // PA_SU_VTX_CNTL until a state line says otherwise: pix_center 0, as every traced draw has it (vtxcntl 00000004).
        s.bindExtent.vtxCntl = 4;
    }
    ReplayBind(s);
    std::fclose(f);
    for (const auto& c : s.configs)
    {
        const ResolveInfo& i = c.decoded;
        std::printf("resolve config x%llu: RB_COPY_CONTROL %08X (src %u, sample select %u, colour clear %u, depth clear %u, command %u) "
                    "RB_COPY_DEST_PITCH %08X (%u x %u) RB_COPY_DEST_INFO %08X (endian %u, array %u, format %u, number %u, exp bias %d, swap %u) "
                    "clears %08X %08X %08X window %08X %08X fetch1 %08X (endian %u, %u words)\n",
                    (unsigned long long)c.count, c.copyControl, c.copyControl & 7, (c.copyControl >> 4) & 7, (c.copyControl >> 8) & 1, (c.copyControl >> 9) & 1,
                    (c.copyControl >> 20) & 3, c.destPitch, c.destPitch & 0x3FFF, (c.destPitch >> 16) & 0x3FFF, c.destInfo, c.destInfo & 7, (c.destInfo >> 3) & 1,
                    (c.destInfo >> 7) & 0x3F, (c.destInfo >> 13) & 7, int32_t(((c.destInfo >> 16) & 0x3F) ^ 0x20) - 0x20, (c.destInfo >> 24) & 1, c.depthClear,
                    c.colorClear, c.colorClearLo, c.windowOffset, c.windowScissorTl, c.fetch1, c.fetch1 & 3, (c.fetch1 >> 2) & 0xFFFFFF);
        if (i.ok)
            std::printf("  decoded: rect %u,%u-%u,%u source key %08X (base %u, %u tiles/row, %s, format %u) span base %u rows %u x %u; dest format %u endian %u swap %u "
                        "exp bias %d pitch %u height %u extent length %u; clears colour %u depth %u\n",
                        i.x0, i.y0, i.x1, i.y1, i.source.packed, i.source.Base(), i.source.PitchTiles(), i.msaa == Msaa::X4 ? "4x" : i.msaa == Msaa::X2 ? "2x" : "1x",
                        i.source.Format(), i.spanBase, i.spanRows, i.spanRowLength, i.destFormat, i.destEndian, unsigned(i.destSwap), i.destExpBias, i.destPitch,
                        i.destHeight, i.destExtentLength, unsigned(i.clearColor), unsigned(i.clearDepth));
        else
            std::printf("  REFUSED by DecodeResolve: %s\n", i.refusal.c_str());
        if (!c.planRefusal.empty())
            std::printf("  REFUSED by PrepareResolve (first occurrence): %s\n", c.planRefusal.c_str());
        for (size_t k = 0; k < c.destBases.size(); k++)
            std::printf("  RB_COPY_DEST_BASE %08X x%llu\n", c.destBases[k], (unsigned long long)c.destBaseCounts[k]);
    }
    std::printf("replay %s: %llu draws (%llu unparsed lines), %llu bound, %llu rasterizing without targets, %llu not rasterizing, %llu resolves "
                "(%llu with traced registers, %llu synthesized; %llu whose fetch constant 0 is not a 6-word vertex fetch); "
                "bind colour transfers %llu (%llu tiles), bind depth transfers %llu (%llu tiles), resolve copy transfers %llu (%llu tiles), "
                "resolve clear transfers colour %llu (%llu tiles) depth %llu (%llu tiles); "
                "%llu tex lines, %llu read a resolve destination, %llu mismatched; %llu refusal(s)\n",
                path, (unsigned long long)s.draws, (unsigned long long)s.unparsed, (unsigned long long)s.bound, (unsigned long long)s.unbound,
                (unsigned long long)s.noRaster, (unsigned long long)s.resolves, (unsigned long long)(s.resolves - s.synthesized), (unsigned long long)s.synthesized,
                (unsigned long long)s.fetchNotVertex, (unsigned long long)s.transferCount[0], (unsigned long long)s.transferTiles[0],
                (unsigned long long)s.transferCount[1], (unsigned long long)s.transferTiles[1], (unsigned long long)s.resolveTransfers,
                (unsigned long long)s.resolveTransferTiles, (unsigned long long)s.clearTransfers[0], (unsigned long long)s.clearTransferTiles[0],
                (unsigned long long)s.clearTransfers[1], (unsigned long long)s.clearTransferTiles[1], (unsigned long long)s.texLines, (unsigned long long)s.texHits,
                (unsigned long long)s.texMismatches, (unsigned long long)s.refusals);
    std::printf("extent: %llu draw(s) with a state line, %llu bound below their scissor by EstimateMaxY, %llu whose extent needs their vertices "
                "(clip disabled, 8192x8192 scissor; vertex bound %s)\n",
                (unsigned long long)s.withState, (unsigned long long)s.heightLimited, (unsigned long long)s.needVertices,
                s.vertexMaxY ? (std::to_string(*s.vertexMaxY) + " rows (hypothesis from the command line)").c_str() : "unknown: the scissor's");
    std::printf("vertices: %llu bind(s) bounded by their vf0 line (VertexMaxY of PassThroughPositions, stride 7, position at dword 0); "
                "%llu cross-aspect (depth <-> colour) transfer(s), %llu tiles\n",
                (unsigned long long)s.vf0Bound, (unsigned long long)s.crossAspect, (unsigned long long)s.crossAspectTiles);
    for (const std::string& l : s.crossAspectLog)
        std::printf("  cross-aspect %s\n", l.c_str());
    std::printf("bases: %llu same-aspect transfer(s) between keys of different bases, %llu tiles (first %zu listed)\n", (unsigned long long)s.otherBase,
                (unsigned long long)s.otherBaseTiles, s.otherBaseLog.size());
    for (const std::string& l : s.otherBaseLog)
        std::printf("  other-base %s\n", l.c_str());
    std::printf("pitch/32_FLOAT: %llu transfer(s) between pitches or with a 32_FLOAT key (first %zu listed)\n", (unsigned long long)s.pitchOrF32,
                s.pitchOrF32Log.size());
    std::printf("pairs: %zu distinct transfer pair(s) (source key -> destination key: count, first draw)\n", s.pairs.size());
    for (const auto& p : s.pairs)
        std::printf("  pair %06X -> %06X: %llu, first draw %llu\n", p.source, p.dest, (unsigned long long)p.count, p.firstDraw);
    for (const std::string& l : s.pitchOrF32Log)
        std::printf("  pitch/f32 %s\n", l.c_str());
    return (s.refusals || s.texMismatches) ? 1 : 0;
}

}  // namespace

// FM4_OVERWRITE_CLAIM: Bind with cutouts (edram.h Bind; Ownership::Change's clearCutout, the rule a resolve clear already uses). After the
// old 2_10_10_10 + D24FS8 bind holds 720 + 720 tiles, the 7e3 + D24FS8@1024 bind with the vertex bound 360 (TestBind's case) transfers
// colour 0-719 and depth 1024-1439. The 4x 640-pixel target's tiles are 40 x 8 pixels (80 x 16 samples at 4x), 16 a row, 45 rows for
// 360 rows. A cutout of the whole 640x360 contains every tile: no transfer. A cutout of 640x352 contains rows 0-43 (44 x 16 = 704
// tiles); row 44 (tiles 704-719, pixel rows 352-359) is partly outside and transfers. The depth claim is tiles 1024-1743, of which
// the old depth key (720-1439) owns 1024-1439 = rows 0-25: all inside the 640x352 cutout, so no depth transfer; its row 44
// (1728-1743) has no owner.
void TestOverwriteCutout()
{
    Ownership o;
    BindRegisters old;
    old.surfaceInfo = kFm4Surface, old.modeControl = 4, old.colorMask = 0xFFFF, old.depthControl = 0x8777;
    old.depthInfo = kFm4Depth, old.colorInfo[0] = kFm4Color;
    CHECK(Bind(o, old, 360).ok && o.Count(kAc) == 720 && o.Count(kAd) == 720);
    BindRegisters hdr = old;
    hdr.colorInfo[0] = 0x00030000, hdr.depthInfo = 0x00010400;
    const Key kHc = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), kHd = Key::Depth(1024, 16, Msaa::X4, DepthFormat::D24FS8);
    const Ownership before = o;
    const Rect whole{0, 0, 640, 360}, most{0, 0, 640, 352};
    Binding b = Bind(o, hdr, 360, &whole, &whole);
    CHECK(b.ok && b.transfers[0].empty() && b.transfers[1].empty() && o.Count(kHc) == 720 && o.Count(kHd) == 720 && o.Count(kAc) == 0);
    o = before;
    b = Bind(o, hdr, 360, &most, &most);
    CHECK(b.ok && b.transfers[1] == T({{704, 720, kAc}}) && b.transfers[0].empty() && o.Count(kHc) == 720 && o.Count(kHd) == 720);
    const Rect top{0, 0, 640, 200};  // rows 0-24 inside (25 x 16 = 400 tiles): depth 1024-1423 claimed free, 1424-1439 (row 25) transfers
    o = before;
    b = Bind(o, hdr, 360, &top, &top);
    CHECK(b.ok && b.transfers[1] == T({{400, 720, kAc}}) && b.transfers[0] == T({{1424, 1440, kAd}}));
    o = before;
    b = Bind(o, hdr, 360, &whole, nullptr);  // depth only: the colour transfer stays
    CHECK(b.ok && b.transfers[0].empty() && b.transfers[1] == T({{0, 720, kAc}}));
    o = before;
    b = Bind(o, hdr, 360);
    CONTROL(b.transfers[1].empty() || b.transfers[0].empty());  // without cutouts both transfer, as TestBind
}

// FM4 Recomp (M7 GPU cost, FM4_TRANSFER_SKIP_UNWRITTEN): the shadow map (edram.h Ownership::ShadowAt) and the skip. Family B of
// FM4's transfers: the 4x kD24FS8 depth at 720 (kAd, 640x360 4x: tiles 720-1439) lends its tiles to the 1x kD24FS8 key at
// the same base (kBd, 1280x720) and takes them back. Expected values by hand: a transfer of [720, 1440) is one run (16 tiles a row,
// 45 rows, all from one owner, X render_target_cache.cc:1624-1632 coalescing); a skipped tile changes owner and emits nothing.
void TestTransferSkip()
{
    std::vector<Transfer> t;
    std::string why;
    // Lend, no write, reclaim: no transfer, 1 run of 720 tiles skipped.
    Ownership o;
    o.skipUnwritten = true;
    CHECK(o.Change(kAd, 0, 720, t, why) && t.empty() && o.ShadowAt(720).Empty());
    CHECK(o.Change(kBd, 0, 720, t, why, nullptr, false) && t == T({{720, 1440, kAd}}) && o.ShadowAt(720) == kAd && o.ShadowAt(1439) == kAd);
    t.clear();
    CHECK(o.Change(kAd, 0, 720, t, why) && t.empty() && o.Count(kAd) == 720 && o.Skipped().transfers == 1 && o.Skipped().tiles == 720);
    CHECK(o.ShadowAt(720).Empty());  // kAd's claim writes: kBd's image no longer holds kAd's content
    CONTROL(o.Count(kBd) != 0);
    // A testing-only reclaim keeps the shadow: the interim owner (kBd) holds the content now.
    Ownership r;
    r.skipUnwritten = true;
    CHECK(r.Change(kAd, 0, 720, t, why) && r.Change(kBd, 0, 720, t, why, nullptr, false));
    t.clear();
    CHECK(r.Change(kAd, 0, 720, t, why, nullptr, false) && t.empty() && r.ShadowAt(720) == kBd);
    CHECK(r.Change(kBd, 0, 720, t, why, nullptr, false) && t.empty() && r.Skipped().transfers == 2 && r.Skipped().tiles == 1440);  // and back again
    // CONTROL: the interim owner writes (a depth-writing bind): the reclaim transfers the whole range.
    Ownership w;
    w.skipUnwritten = true;
    t.clear();
    CHECK(w.Change(kAd, 0, 720, t, why) && w.Change(kBd, 0, 720, t, why) && t == T({{720, 1440, kAd}}) && w.ShadowAt(720).Empty());
    t.clear();
    CHECK(w.Change(kAd, 0, 720, t, why) && t == T({{720, 1440, kBd}}) && w.Skipped().tiles == 0);
    // A partial write: kBd lends read-only, then writes its first 112 tiles (rows 0-6, 720-831; the claim memo's read-only entry
    // must not answer a writing claim). The reclaim transfers exactly the written run [720, 832) and skips [832, 1440): 608 tiles.
    // Per tile, not whole: an unwritten tile's bytes are the reclaimer's own, whatever its neighbours hold.
    Ownership p;
    p.skipUnwritten = true;
    t.clear();
    CHECK(p.Change(kAd, 0, 720, t, why) && p.Change(kBd, 0, 720, t, why, nullptr, false) && p.Change(kBd, 0, 720, t, why, nullptr, false));
    t.clear();
    CHECK(p.Change(kBd, 0, 112, t, why) && t.empty() && p.ShadowAt(831).Empty() && p.ShadowAt(832) == kAd);
    CHECK(p.Change(kAd, 0, 720, t, why) && t == T({{720, 832, kBd}}) && p.Skipped().transfers == 1 && p.Skipped().tiles == 608);
    // The skip off (the default): the shadows are tracked, every transfer stays.
    Ownership off;
    t.clear();
    CHECK(off.Change(kAd, 0, 720, t, why) && off.Change(kBd, 0, 720, t, why, nullptr, false) && off.ShadowAt(720) == kAd);
    t.clear();
    CHECK(off.Change(kAd, 0, 720, t, why) && t == T({{720, 1440, kBd}}) && off.Skipped().tiles == 0);
    // CONTROL: a transfer into the interim owner from a third key replaces the shadow (kBd's image now holds the third key's
    // content, not kAd's): kAd's reclaim transfers from the third key. The third key: 2x kD24FS8 at 720 (Key bits as X .h:269-279).
    const Key kXd = Key::Depth(720, 16, Msaa::X2, DepthFormat::D24FS8);
    Ownership x;
    x.skipUnwritten = true;
    t.clear();
    CHECK(x.Change(kAd, 0, 720, t, why) && x.Change(kBd, 0, 720, t, why, nullptr, false) && x.Change(kXd, 0, 720, t, why, nullptr, false));
    CHECK(x.ShadowAt(720) == kBd);
    t.clear();
    CHECK(x.Change(kAd, 0, 720, t, why) && t == T({{720, 1440, kXd}}) && x.Skipped().tiles == 0);
    // MarkWritten (a resolve clear's rectangle past its claim, a draw's rows past a clamped claim): tiles 720-735 written in kBd's
    // image and 736-751 in kAd's (while kBd owns them) both lose their shadow and transfer; 752-1439 are skipped (688 tiles).
    Ownership m;
    m.skipUnwritten = true;
    t.clear();
    CHECK(m.Change(kAd, 0, 720, t, why) && m.Change(kBd, 0, 720, t, why, nullptr, false));
    m.MarkWritten(kBd, 0, 16);
    m.MarkWritten(kAd, 16, 16);
    t.clear();
    CHECK(m.Change(kAd, 0, 720, t, why) && t == T({{720, 752, kBd}}) && m.Skipped().tiles == 688);
    // MarkWritten wraps as Change does: a key at base 2040, 16 tiles: 2040-2047 then 0-7; tile 8 keeps its shadow.
    const Key kW1 = Key::Color(2040, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float), kW4 = Key::Color(2040, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float);
    Ownership wrap;
    t.clear();
    CHECK(wrap.Change(kW4, 0, 32, t, why) && wrap.Change(kW1, 0, 32, t, why, nullptr, false) && wrap.ShadowAt(0) == kW4 && wrap.ShadowAt(8) == kW4);
    wrap.MarkWritten(kW1, 0, 16);
    CHECK(wrap.ShadowAt(2040).Empty() && wrap.ShadowAt(7).Empty() && wrap.ShadowAt(8) == kW4);
    // The exact pairs only (edram.cpp SkipExact): same aspect, same format, a float or integer host. CONTROL: kD24S8 (a UNORM host
    // where supported) and colour storage 2 (A2B10G10R10_UNORM) keep their transfers; storage 3 (RGBA16F) is skipped.
    const Key kS4 = Key::Depth(720, 16, Msaa::X4, DepthFormat::D24S8), kS1 = Key::Depth(720, 16, Msaa::X1, DepthFormat::D24S8);
    Ownership s;
    s.skipUnwritten = true;
    t.clear();
    CHECK(s.Change(kS4, 0, 720, t, why) && s.Change(kS1, 0, 720, t, why, nullptr, false) && s.ShadowAt(720) == kS4);
    t.clear();
    CHECK(s.Change(kS4, 0, 720, t, why) && t == T({{720, 1440, kS1}}) && s.Skipped().tiles == 0);
    Ownership c;
    c.skipUnwritten = true;
    t.clear();
    CHECK(c.Change(kAc, 0, 720, t, why) && c.Change(kBc, 0, 720, t, why, nullptr, false));
    t.clear();
    CHECK(c.Change(kAc, 0, 720, t, why) && t == T({{0, 720, kBc}}) && c.Skipped().tiles == 0);
    const Key kH4 = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), kH1 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float);
    Ownership h;
    h.skipUnwritten = true;
    t.clear();
    CHECK(h.Change(kH4, 0, 720, t, why) && h.Change(kH1, 0, 720, t, why, nullptr, false));
    t.clear();
    CHECK(h.Change(kH4, 0, 720, t, why) && t.empty() && h.Skipped().tiles == 720);
    // CONTROL: a pair across formats is never exact (colour storage 3 -> 2 repacks through the guest word).
    const Key kT1 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10);
    Ownership f;
    f.skipUnwritten = true;
    t.clear();
    CHECK(f.Change(kH4, 0, 720, t, why) && f.Change(kT1, 0, 720, t, why, nullptr, false) && f.ShadowAt(0) == kH4);
    t.clear();
    CHECK(f.Change(kH4, 0, 720, t, why) && t == T({{0, 720, kT1}}) && f.Skipped().tiles == 0);

    // Through Bind: a testing-only 1x depth draw (Bind1x with colour mask 0, depthReadOnly) borrows tiles 720-1439 (720 rows:
    // 45 tile rows x 16), the 4x scene's depth bind (360 rows at 4x: (360 << 1) / 16 = 45 rows) takes them back without a transfer.
    Ownership b;
    b.skipUnwritten = true;
    CHECK(Bind(b, Fm4Bind(4, 2, 0), 360).ok && b.Count(kAd) == 720);
    Binding bb = Bind(b, Bind1x(2, 0), 720, nullptr, nullptr, true);
    CHECK(bb.ok && bb.depth == kBd && bb.transfers[0] == T({{720, 1440, kAd}}));
    bb = Bind(b, Fm4Bind(4, 2, 0), 360);
    CHECK(bb.ok && bb.transfers[0].empty() && b.Skipped().tiles == 720);
    // CONTROL: the same 1x draw with depth writes (depthReadOnly false) makes the reclaim transfer.
    CHECK(Bind(b, Bind1x(2, 0), 720).ok);
    bb = Bind(b, Fm4Bind(4, 2, 0), 360);
    CHECK(bb.ok && bb.transfers[0] == T({{720, 1440, kBd}}) && b.Skipped().tiles == 720);
    // CONTROL: a depth cutout overrides depthReadOnly (the draw overwrites): no transfer in (the cutout), and the reclaim transfers.
    const Rect whole1x{0, 0, 1280, 720};
    bb = Bind(b, Bind1x(2, 0), 720, &whole1x, nullptr, true);
    CHECK(bb.ok && bb.transfers[0].empty() && b.ShadowAt(720).Empty());
    bb = Bind(b, Fm4Bind(4, 2, 0), 360);
    CHECK(bb.ok && bb.transfers[0] == T({{720, 1440, kBd}}));
    // A draw's rows past its clamped claim: kAc (colour, base 0) owns tiles 720-1023 and lends them read-only to kAd. The 4x scene
    // bind at 512 rows: (512 << 1) / 16 = 64 tile rows = 1024 tiles, but the bases' distance (720, X :819-857) clamps kAc's claim to
    // 0-719; the host draw still writes rows 360-511 of kAc's image = tiles 720-1023, so their shadows (kAc) go. kAd's claim is
    // read-only (depthReadOnly) and keeps nothing else from changing them.
    Ownership k;
    t.clear();
    CHECK(k.Change(kAc, 720, 304, t, why) && k.Change(kAd, 0, 304, t, why, nullptr, false) && t == T({{720, 1024, kAc}}) && k.ShadowAt(720) == kAc);
    bb = Bind(k, Fm4Bind(), 512, nullptr, nullptr, true);
    CHECK(bb.ok && bb.lengthTiles[1] == 720 && bb.lengthTiles[0] == 720 && k.ShadowAt(720).Empty() && k.ShadowAt(1023).Empty());

    // A depth resolve copy is a read-only claim: after the testing-only 1x draw, the resolve of the 4x depth (TestDepthResolve's
    // registers: copy_src_select 4, the whole 640x360) reclaims the span row by row (45 rows of 16 tiles, PrepareResolve) with no
    // copy transfer; one skipped run per row.
    Ownership d;
    d.skipUnwritten = true;
    CHECK(Bind(d, Fm4Bind(4, 2, 0), 360).ok && Bind(d, Bind1x(2, 0), 720, nullptr, nullptr, true).ok);
    ResolveRegisters rr = Fm4Resolve();
    rr.copyControl = 1u << 20 | 4;
    const ResolvePlan plan = PrepareResolve(d, DecodeResolve(rr));
    CHECK(plan.ok && plan.copy && plan.copySource == kAd && plan.copyTransfers.empty() && d.Skipped().transfers == 45 && d.Skipped().tiles == 720);
    CHECK(d.ShadowAt(720) == kBd);  // the copy wrote nothing: kBd's image still holds the content
}

// FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): whole-tile constants (edram.h Ownership::SetClearValue) move to a key of the same
// aspect and format as `value` transfers. Tile sizes from X render_target_cache.cc:355-359: 4x 40x8 pixels, 2x 80x8, 1x 80x16; pitch 16
// tiles a row. W is an arbitrary word: the model never decodes it.
void TestClearValueOwnership()
{
    constexpr uint32_t W = 0x12345678u;
    std::vector<Transfer> t;
    std::string why;
    uint32_t w = 0;
    const Rect scene4x{0, 0, 640, 360};  // 16 x 45 tiles of 40x8 = tiles 0-719
    // The whole 4x scene cleared: 720 tiles recorded; the 1x key of the same format (storage 2) takes them as one value transfer.
    {
        Ownership o;
        CHECK(o.Change(kAc, 0, 720, t, why) && t.empty());
        CHECK(o.SetClearValue(kAc, scene4x, W) == 720 && o.ClearValueTiles() == 720 && o.ClearValue(0, w) && w == W && o.ClearValue(719, w));
        CONTROL(o.ClearValue(720, w));
        CHECK(o.Change(kBc, 0, 720, t, why) && t == T({{0, 720, kAc, true, W}}) && o.Count(kBc) == 720);
        CHECK(o.ClearValueTiles() == 0);  // the claimant writes them: no constant any more
        CONTROL(t == T({{0, 720, kAc}}));  // not an ordinary copy
    }
    // Partial cover: 640x352 holds rows 0-43 (704 tiles); row 44 (pixel rows 352-359) is not a constant and is copied.
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kAc, 0, 720, t, why) && o.SetClearValue(kAc, Rect{0, 0, 640, 352}, W) == 704);
        CHECK(o.Change(kBc, 0, 720, t, why) && t == T({{0, 704, kAc, true, W}, {704, 720, kAc}}));
    }
    // A rectangle that starts mid-tile: y 4..360 leaves row 0 (y 0..8) out: tiles 16-719.
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kAc, 0, 720, t, why) && o.SetClearValue(kAc, Rect{0, 4, 640, 356}, W) == 704);
        CHECK(!o.ClearValue(15, w) && o.ClearValue(16, w));
    }
    // CONTROL: another format (1x storage 0) gets an ordinary copy, and the constants still go.
    {
        Ownership o;
        t.clear();
        const Key kCc = Key::Color(0, 16, Msaa::X1, ColorFormat::k8_8_8_8);
        CHECK(o.Change(kAc, 0, 720, t, why) && o.SetClearValue(kAc, scene4x, W) == 720);
        CHECK(o.Change(kCc, 0, 720, t, why) && t == T({{0, 720, kAc}}) && o.ClearValueTiles() == 0);
    }
    // A write by the owner itself drops its constants, also through the claim memo (the first claim of tiles 0-719 is remembered and
    // SetClearValue does not invalidate it): tiles 0-111 are copied afterwards, 112-719 still move as a value.
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kAc, 0, 720, t, why) && o.SetClearValue(kAc, scene4x, W) == 720);
        CHECK(o.Change(kAc, 0, 112, t, why) && t.empty() && o.ClearValueTiles() == 608 && !o.ClearValue(111, w) && o.ClearValue(112, w));
        CHECK(o.Change(kBc, 0, 720, t, why) && t == T({{0, 112, kAc}, {112, 720, kAc, true, W}}));
    }
    // FM4 Recomp (both flags): a value transfer leaves the source's constant possibly unrecorded in its image, so the source is no shadow
    // of those tiles: the reclaim is a real transfer, even with the skip on, and never a skip. CONTROL: an ordinary lend of the same tiles is skipped.
    {
        const Key c4 = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), c1 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float);
        Ownership o;
        o.skipUnwritten = true;
        t.clear();
        CHECK(o.Change(c4, 0, 720, t, why) && o.SetClearValue(c4, scene4x, W) == 720);
        CHECK(o.Change(c1, 0, 720, t, why, nullptr, false) && t == T({{0, 720, c4, true, W}}) && o.ShadowAt(0).Empty());
        t.clear();
        CHECK(o.Change(c4, 0, 720, t, why) && t == T({{0, 720, c1}}) && o.Skipped().tiles == 0);
        Ownership p;
        p.skipUnwritten = true;
        t.clear();
        CHECK(p.Change(c4, 0, 720, t, why) && p.Change(c1, 0, 720, t, why, nullptr, false) && p.ShadowAt(0) == c4);
        t.clear();
        CHECK(p.Change(c4, 0, 720, t, why) && t.empty() && p.Skipped().tiles == 720);
    }
    // A constant with no exact guest word travels by its host-colour index (Transfer::exact), kept through a run split; a different index does not coalesce.
    {
        const Key c4 = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), c2 = Key::Color(0, 16, Msaa::X2, ColorFormat::k2_10_10_10_Float);
        Ownership o;
        t.clear();
        CHECK(o.Change(c4, 0, 720, t, why) && o.SetClearValue(c4, Rect{0, 0, 640, 176}, 0, 3) == 352 && o.SetClearValue(c4, Rect{0, 176, 640, 184}, 0, 5) == 368);  // 22 and 23 rows of 16 tiles of 8 pixel rows
        CHECK(o.Change(c2, 100, 10, t, why) && t == T({{100, 110, c4, true, 0, 3}}));  // [100, 110) cuts the first run in two
        t.clear();
        CHECK(o.Change(c2, 0, 720, t, why) && t == T({{0, 100, c4, true, 0, 3}, {110, 352, c4, true, 0, 3}, {352, 720, c4, true, 0, 5}}));
        CONTROL(t == T({{0, 100, c4, true, 0, 3}, {110, 720, c4, true, 0, 3}}));
    }
    // A hole in a run: the 1x key claims tiles 100-109 (a value transfer); 99 and 110 keep the constant (the run split in two).
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kAc, 0, 720, t, why) && o.SetClearValue(kAc, scene4x, W) == 720);
        CHECK(o.Change(kBc, 100, 10, t, why) && t == T({{100, 110, kAc, true, W}}));
        CHECK(o.ClearValue(99, w) && !o.ClearValue(105, w) && o.ClearValue(110, w) && o.ClearValueTiles() == 710);
    }
    // A claim whose cutout covers the tiles (a clear of the claimant) transfers nothing and drops the constants too.
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kAc, 0, 720, t, why) && o.SetClearValue(kAc, scene4x, W) == 720);
        const Rect whole1x{0, 0, 1280, 720};
        CHECK(o.Change(kBc, 0, 720, t, why, &whole1x) && t.empty() && o.ClearValueTiles() == 0);
    }
    // CONTROL: tiles the key does not own are never recorded.
    {
        Ownership o;
        t.clear();
        CHECK(o.Change(kBc, 0, 720, t, why));
        CHECK(o.SetClearValue(kAc, scene4x, W) == 0 && o.ClearValueTiles() == 0);
    }
    // FM4's transfer family A: the 4x 7e3 colour key at 0 and the 4x kD24FS8 key at 1024 are cleared over their
    // 1024 tiles each (16 x 64 tiles of 40x8 = 640x512), then the 2x keys of the same tiles and formats claim them: one value transfer
    // each, with the words; nothing is copied.
    {
        Ownership o;
        t.clear();
        const Key c4 = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), c2 = Key::Color(0, 16, Msaa::X2, ColorFormat::k2_10_10_10_Float);
        const Key d4 = Key::Depth(1024, 16, Msaa::X4, DepthFormat::D24FS8), d2 = Key::Depth(1024, 16, Msaa::X2, DepthFormat::D24FS8);
        CHECK(c4.packed == 0xD08000u && d4.packed == 0x708400u);  // the keys of the s202 tally (REPORT §2A)
        CHECK(o.Change(c4, 0, 1024, t, why) && o.Change(d4, 0, 1024, t, why) && t.empty());
        CHECK(o.SetClearValue(c4, Rect{0, 0, 640, 512}, 0xC0020180u) == 1024 && o.SetClearValue(d4, Rect{0, 0, 640, 512}, 0xF00000FFu) == 1024);
        CHECK(o.Change(c2, 0, 1024, t, why) && t == T({{0, 1024, c4, true, 0xC0020180u}}));
        t.clear();
        CHECK(o.Change(d2, 0, 1024, t, why) && t == T({{1024, 2048, d4, true, 0xF00000FFu}}));
        // CONTROL: a 4x 2_10_10_10 (storage 2) claimant of the 7e3 constants is a format change: copied.
        Ownership p;
        t.clear();
        CHECK(p.Change(c4, 0, 1024, t, why) && p.SetClearValue(c4, Rect{0, 0, 640, 512}, 0xC0020180u) == 1024);
        CHECK(p.Change(kAc, 0, 1024, t, why) && t == T({{0, 1024, c4}}));
    }
}

// FM4 Recomp (alpha, RT format 5): ownership between 32 bpp and 64 bpp keys of the same tiles (X render_target_cache.cc:1539-1699
// ChangeOwnership is format-blind: a tile changes owner and the previous owner is a transfer source whatever its bits per pixel; the
// lengths double at 64 bpp, :847-857 `<< is_64bpp`). The 1280-pitch 1x surface (16 tiles a row at 32 bpp, 32 at 64 bpp), colour only.
void TestOwnership64()
{
    BindRegisters c32 = Bind1x(0, 0xF), c64 = Bind1x(0, 0xF);
    c32.colorInfo[0] = 0x00000000;  // base 0, k_8_8_8_8
    c64.colorInfo[0] = 0x00050000;  // base 0, k_16_16_16_16
    const Key k32 = Key::Color(0, 16, Msaa::X1, ColorFormat::k8_8_8_8), k64 = Key::Color(0, 16, Msaa::X1, ColorFormat::k16_16_16_16);
    Ownership o;
    // 32 rows of 32 bpp: ceil(32 / 16) = 2 tile rows * 16 = 32 tiles (one target: min(32, 4096 >> 1, 2048)).
    Binding b = Bind(o, c32, 32);
    CHECK(b.ok && b.color[0] == k32 && b.lengthTiles[1] == 32 && o.Count(k32) == 32 && b.transfers[1].empty());
    // 16 rows of 64 bpp: ceil(16 / 16) = 1 tile row * 16 = 16 tiles at 32 bpp, << 1 = 32 tiles (min(32, 4096 >> 0, 2048)): the same 32
    // tiles, all from the 32 bpp key (one transfer, X :1624-1632 coalescing).
    b = Bind(o, c64, 16);
    CHECK(b.ok && b.color[0] == k64 && b.lengthTiles[1] == 32 && b.transfers[1] == T({{0, 32, k32}}));
    CHECK(o.Count(k64) == 32 && o.Count(k32) == 0 && o.At(31) == k64 && o.At(32).Empty());
    // Tiles [0, 32) in the 64 bpp key's pixels: one 32-tile row of 40-pixel tiles (X :355-357): (0, 0, 1280, 16); in the 32 bpp
    // key's: two 16-tile rows of 80-pixel tiles: (0, 0, 1280, 32).
    Rect r[3];
    CHECK(RangeRectangles(0, 32, k64, r) == 1 && r[0] == Rect{0, 0, 1280, 16});
    CHECK(RangeRectangles(0, 32, k32, r) == 1 && r[0] == Rect{0, 0, 1280, 32});
    // Back to 32 bpp, 48 rows: 3 tile rows = 48 tiles; [0, 32) from the 64 bpp key, [32, 48) had no owner (no transfer).
    b = Bind(o, c32, 48);
    CHECK(b.ok && b.lengthTiles[1] == 48 && b.transfers[1] == T({{0, 32, k64}}) && o.Count(k32) == 48 && o.Count(k64) == 0);
    // CONTROL: the 64 bpp claim is not the 32 bpp length (16 tiles would leave tiles 16-31 with the 32 bpp key).
    Ownership q;
    CHECK(Bind(q, c32, 32).ok);
    b = Bind(q, c64, 16);
    CONTROL(b.lengthTiles[1] == 16 || q.At(16) == k32);
}

// FM4 Recomp (alpha, 64 bpp resolve): FM4's resolve after a race in Car Select (its log:
// colour 0, RB_COLOR_INFO format 5, 1x, sample select 0, to texture format 32, number 7, exp bias 0, swap 0, endian 1,
// convert). The source key is the one the log names (key 1402000: base 0, 4 tiles a row at 32 bpp, 1x, storage 5; 320x8192): pitch
// 320 (X render_target_cache.cc:606-608: (320 + 79) / 80 = 4). The rectangle here is 320x64 (the log does not give FM4's).
ResolveRegisters Resolve64()
{
    ResolveRegisters r = Fm4Resolve();
    r.surfaceInfo = 320, r.colorInfo[0] = 5u << 16, r.copyControl = 1u << 20;  // RB_COLOR_INFO format bits 16-19 (X registers.h:689-694)
    r.copyDestPitch = 320 | 64u << 16;
    r.copyDestInfo = 1 | 32u << 7 | 7u << 13;  // X registers.h:812-826: endian 1, format 32, number 7, bias 0, no swap
    const float v[6] = {-0.5f, -0.5f, 319.5f, -0.5f, 319.5f, 63.5f};
    for (int i = 0; i < 6; i++)
        r.vertexWords[i] = GuestFloat(v[i]);
    return r;
}

void TestResolve64Decode()
{
    auto refused = [](ResolveRegisters rr, const char* text) {
        const ResolveInfo x = DecodeResolve(rr);
        return !x.ok && x.refusal.find(text) != std::string::npos;
    };
    ResolveRegisters r = Resolve64();
    const ResolveInfo i = DecodeResolve(r);
    CHECK(i.ok && i.copy && !i.isDepth && i.source == Key::Color(0, 4, Msaa::X1, ColorFormat::k16_16_16_16) && i.source.packed == 0x1402000u);
    CHECK(i.x0 == 0 && i.y0 == 0 && i.x1 == 320 && i.y1 == 64 && i.sampleSelect == SampleSelect::k0);
    CHECK(i.destFormat == 32 && i.destNumber == 7 && i.destEndian == 1 && i.destExpBias == 0 && !i.destSwap && i.destBytesLog2 == 3);
    // The destination with bpp_log2 3 (X draw_util.cc:1133-1173): granularity 32x32 (xenos.h:1188-1195: 7 - min(2, 3) = 5), so offset
    // (0, 0) and base = lower bound = copyDestBase + TiledOffset2D(0, 0) = copyDestBase. The upper bound (texture_util.cc:490-517):
    // the tile of (319, 63) -> (288, 32); TiledOffset2D(288, 32, 320, 3) (:443-458): pitch 320 (already 32-aligned), macro = (288 >> 5
    // = 9) + (32 >> 5 = 1) * (320 >> 5 = 10) = 19, << (3 + 7) = 19456 = 0x4C00; micro = ((288 & 7) + ((32 & 0xE) << 2)) << 3 = 0;
    // offset 19456; mix: (0x4C00 & ~0x1FF) << 3 = 0x26000 = 155648, (32 & 16) << 7 = 0, (0x4C00 & 0x1C0) << 2 = 0,
    // (((32 & 8) >> 2) + (288 >> 3 = 36)) & 3 = 0, 0x4C00 & 0x3F = 0 -> 155648; + 0x400 << 3 = 8192 (:512-514) -> 163840
    // = 320 * 64 * 8 bytes.
    CHECK(i.destOffsetX == 0 && i.destOffsetY == 0 && i.destBase == 0x00800000u && i.destExtentStart == i.destBase && i.destExtentLength == 163840);
    CONTROL(i.destExtentLength == 81920);  // the 32 bpp extent of the same rectangle
    // The span (X draw_util.cc:1202-1256, GetResolveEdramTileSpan :875-904): 64 bpp tiles are 40x16 pixels at 1x, 8 a row
    // (pitch 4 << 1); 320 pixels = tiles 0-7 (((0 + 40) << (3 + 1)) + 79) / 80 = 8), 64 rows = 4 tile rows.
    CHECK(i.spanBase == 0 && i.spanPitch == 8 && i.spanRowLength == 8 && i.spanRows == 4);
    CONTROL(i.spanRowLength == 4);  // the 32 bpp span

    // An origin at x0 = 32, y0 = 8 (vertex 31.5, 7.5 + the half pixel, X draw_util.cc:801-807): at bpp_log2 3 the granularity is
    // still 32 (unlike 16 bpp's 64), so offset (0, 8) and base = extent start = TiledOffset2D(32, 0, 320, 3): macro 1 << 10 = 0x400;
    // (0x400 & ~0x1FF) << 3 = 8192; (0 + (32 >> 3 = 4)) & 3 = 0 -> 8192. The upper bound is the same: length 163840 - 8192 = 155648.
    const float v1[6] = {31.5f, 7.5f, 319.5f, 7.5f, 319.5f, 63.5f};
    ResolveRegisters o = r;
    for (int k = 0; k < 6; k++)
        o.vertexWords[k] = GuestFloat(v1[k]);
    const ResolveInfo oi = DecodeResolve(o);
    CHECK(oi.ok && oi.x0 == 32 && oi.y0 == 8 && oi.destOffsetX == 0 && oi.destOffsetY == 8 && oi.destBase == 0x00800000u + 8192 &&
          oi.destExtentStart == oi.destBase && oi.destExtentLength == 155648);
    CONTROL(oi.destOffsetX == 32);  // the 16 bpp (64-texel) granularity

    // Exponent bias and swap go through the full resolve's generic factor and .bgra (decoded as given).
    o = r, o.copyDestInfo |= 3u << 16 | 1u << 24;
    CHECK(DecodeResolve(o).ok && DecodeResolve(o).destExpBias == 3 && DecodeResolve(o).destSwap);
    o = r, o.copyDestInfo = 32u << 7 | 7u << 13;  // endian 0
    CHECK(DecodeResolve(o).ok && DecodeResolve(o).destEndian == 0);

    // Neighbours, each refused by name.
    o = r, o.copyDestInfo = 1 | 32u << 7;  // number 0
    CHECK(refused(o, "number format 0 into texture format 32"));
    o = r, o.copyDestInfo = 2 | 32u << 7 | 7u << 13;  // 8in32
    CHECK(refused(o, "endian 2 on a 64 bpp destination"));
    o = r, o.copyDestInfo = 4 | 32u << 7 | 7u << 13;  // 8in64 (X xenos.h:200-207)
    CHECK(refused(o, "64/128-bit swap"));
    o = r, o.copyDestInfo = 1 | 33u << 7 | 7u << 13;
    CHECK(refused(o, "into texture format 33"));
    o = r, o.copyDestInfo = 1 | 26u << 7 | 7u << 13;  // k_16_16_16_16 (xenos.h:600): not the observed destination
    CHECK(refused(o, "into texture format 26"));
    o = r, o.surfaceInfo = 320 | 1u << 16;  // 2x (RB_SURFACE_INFO msaa_samples bits 16-17)
    CHECK(refused(o, "64 bpp source (storage 5) at 2x"));
    o = r, o.surfaceInfo = 320 | 2u << 16;  // 4x (pitch tiles (640 + 79) / 80 = 8)
    CHECK(refused(o, "64 bpp source (storage 5) at 4x"));
    o = r, o.copyControl |= 1u << 8;  // copy_color_clear_enable
    CHECK(refused(o, "that also clears (colour 1, depth 0)"));
    o = r, o.copyControl |= 1u << 9;  // copy_depth_clear_enable
    CHECK(refused(o, "that also clears (colour 0, depth 1)"));
    // CONTROL: a 32 bpp source into 32 stays refused by the 32 bpp destination list, and storage 5 into 6 by the 64 bpp one.
    o = r, o.colorInfo[0] = 0;
    CHECK(refused(o, "texture format 32 is not supported in M4"));
    CONTROL(DecodeResolve(o).ok);
    o = r, o.copyDestInfo = 1 | 6u << 7;
    CHECK(refused(o, "into texture format 6"));
    CONTROL(DecodeResolve(o).ok);
}

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        // test_edram <cp.txt> [--vertex-max-y=N]: N stands in for VertexMaxY of every draw that needs its vertices.
        uint32_t vertexMaxY = 0;
        const bool hypothesis = argc > 2 && std::sscanf(argv[2], "--vertex-max-y=%u", &vertexMaxY) == 1;
        return Replay(argv[1], hypothesis ? &vertexMaxY : nullptr);
    }
    TestRegisters();
    TestTileMath();
    TestRangeRectangles();
    TestOwnership();
    TestCutout();
    TestExtent();
    TestBindFm4();
    TestBindCases();
    TestTrace10();
    TestFixedPoint();
    TestResolveDecode();
    TestResolvePlan();
    TestDepthResolve();
    TestOverwriteCutout();
    TestK88ResolveDecode();
    TestTransferSkip();
    TestClearValueOwnership();
    TestOwnership64();  // FM4 Recomp (alpha, RT format 5)
    TestResolve64Decode();  // FM4 Recomp (alpha, 64 bpp resolve)
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
