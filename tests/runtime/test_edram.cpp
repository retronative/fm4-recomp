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
    CONTROL(SupportedKey(Key::Color(0, 16, Msaa::X2, ColorFormat::k8_8_8_8), why));
    CONTROL(SupportedKey(k64, why));
    CONTROL(SupportedKey(Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float), why));
    CONTROL(SupportedKey(Key::Color(0, 16, Msaa::X1, ColorFormat::k32_Float), why));
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
    // Refusals change nothing: colour over depth tiles, depth over colour tiles, kD24S8 over kD24FS8 tiles.
    const Ownership before = o;
    t.clear();
    CONTROL(o.Change(Key::Color(720, 16, Msaa::X4, ColorFormat::k2_10_10_10), 16, 16, t, why));
    CHECK(why.find("across aspects") != std::string::npos && t.empty());
    CONTROL(o.Change(Key::Depth(0, 16, Msaa::X4, DepthFormat::D24FS8), 0, 8, t, why));
    CONTROL(o.Change(Key::Depth(720, 16, Msaa::X4, DepthFormat::D24S8), 16, 16, t, why));
    CHECK(why.find("between depth formats") != std::string::npos);
    // A refusal in the wrapped extent leaves the first extent unchanged too.
    CONTROL(o.Change(Key::Depth(2040, 16, Msaa::X1, DepthFormat::D24S8), 0, 16, t, why));
    CHECK(o.At(2040) == before.At(2040) && o.At(0) == before.At(0) && t.empty());
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
    // CONTROL: a colour-only draw of unknown height on a fresh map claims the whole EDRAM (X :847-857 with one
    // target: min(2048, 4096 >> 1, 2048)); on this map that reaches the depth tiles and is refused (cross
    // aspect), and the map is unchanged: the caller must pass the draw's rows (NOTES.md "Open questions").
    Ownership fresh;
    b = Bind(fresh, c, kHeightUnknown);
    CHECK(b.ok && b.lengthTiles[1] == 2048 && fresh.Count(kCc) == 2048);
    const Ownership before = o;
    b = Bind(o, c, kHeightUnknown);
    CONTROL(b.ok);
    CHECK(b.refusal.find("across aspects") != std::string::npos && o.Count(kCc) == before.Count(kCc) && o.Count(kAd) == before.Count(kAd));
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
    r = Fm4Bind();
    r.colorInfo[0] = 0x00050000;  // k_16_16_16_16: 64 bpp, outside the whitelist
    b = Bind(o, r, 360);
    CONTROL(b.ok);
    r = Fm4Bind();
    r.surfaceInfo = 0x00010280;  // 2x MSAA
    b = Bind(o, r, 360);
    CONTROL(b.ok);
    CHECK(b.refusal.find("2x MSAA") != std::string::npos);
    r = Fm4Bind();
    r.colorInfo[0] = 0x00030000;  // 7e3 float
    b = Bind(o, r, 360);
    CONTROL(b.ok);
    CHECK(o.Count(kAd) == before.Count(kAd) && o.Count(kAc) == before.Count(kAc));
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
    // Exactly these keys are accepted: every other colour format on either surface is refused (formats 3-9,
    // 11-15 at the 4x pitch; 2x MSAA; the 1x pitch with 64 bpp).
    for (uint32_t f = 3; f <= 15; f++)
    {
        if (f == 10)
            continue;
        r = BindRegisters{};
        r.surfaceInfo = 0x14000500, r.modeControl = 4, r.colorMask = 0xF, r.depthControl = 0x8777;
        r.depthInfo = kFm4Depth, r.colorInfo[0] = f << 16;
        Ownership scratch;
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
    CHECK(refused(r, "empty"));
    r = Fm4Resolve(), r.copyControl = 1u << 20 | 4;  // depth source
    CHECK(refused(r, "depth resolve"));
    r = Fm4Resolve(), r.copyDestInfo = 2 | 6u << 7 | 1u << 3;  // array
    CHECK(refused(r, "3D"));
    r = Fm4Resolve(), r.copyDestInfo = 4 | 6u << 7;  // endian 8in64
    CHECK(refused(r, "endian 4"));
    r = Fm4Resolve(), r.copyDestInfo = 2 | 32u << 7;  // k_16_16_16_16_FLOAT
    CHECK(refused(r, "texture format 32"));
    r = Fm4Resolve(), r.copyDestInfo = 2 | 6u << 7 | 1u << 16;  // exp bias 1
    CHECK(refused(r, "exponent bias 1"));
    r = Fm4Resolve(), r.copyDestInfo = 2 | 6u << 7 | 1u << 13;  // signed fraction
    CHECK(refused(r, "number format 1"));
    r = Fm4Resolve(), r.copyDestPitch = 0 | 360u << 16;
    CHECK(refused(r, "pitch is 0"));
    r = Fm4Resolve(), r.colorInfo[0] = 0x00050000;  // 64 bpp source
    CHECK(refused(r, "64 bpp"));
    r = Fm4Resolve(), r.colorInfo[0] = 0x00080000;  // unknown source format
    CHECK(refused(r, "unknown"));
    // The accepted destination formats: 7 (k_2_10_10_10), 50 and 54 (the _AS_16_16_16_16 variants: xenos.h:607-608).
    for (uint32_t f : {7u, 50u, 54u})
    {
        r = Fm4Resolve(), r.copyDestInfo = 2 | f << 7;
        CHECK(DecodeResolve(r).ok);
    }
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
    ResolveRegisters pendingRegs;
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
};

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
        s.resolveTransfers++, s.resolveTransferTiles += t.endTiles - t.startTiles;
    for (const Transfer& t : p.depthClear.transfers)
        s.clearTransfers[1]++, s.clearTransferTiles[1] += t.endTiles - t.startTiles;
    for (const Transfer& t : p.colorClear.transfers)
        s.clearTransfers[0]++, s.clearTransferTiles[0] += t.endTiles - t.startTiles;
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

int Replay(const char* path)
{
    FILE* f = std::fopen(path, "r");
    if (!f)
        return std::printf("cannot open %s\n", path), 2;
    ReplayState s;
    char line[1024];
    while (std::fgets(line, sizeof line, f))
    {
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
        const Binding b = Bind(s.o, r, (scissor >> 16) & 0x3FFF);
        if (!b.ok)
        {
            s.refusals++, std::printf("draw %llu: bind refused: %s\n", n, b.refusal.c_str());
            continue;
        }
        if (!b.rasterizes)
            s.noRaster++;
        else if (b.depth.Empty() && b.color[0].Empty() && b.color[1].Empty() && b.color[2].Empty() && b.color[3].Empty())
            s.unbound++;
        else
            s.bound++;
        for (uint32_t i = 0; i < 5; i++)
            for (const Transfer& t : b.transfers[i])
                s.transferCount[i == 0]++, s.transferTiles[i == 0] += t.endTiles - t.startTiles;
    }
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
    return (s.refusals || s.texMismatches) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
        return Replay(argv[1]);
    TestRegisters();
    TestTileMath();
    TestRangeRectangles();
    TestOwnership();
    TestCutout();
    TestBindFm4();
    TestBindCases();
    TestTrace10();
    TestFixedPoint();
    TestResolveDecode();
    TestResolvePlan();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
