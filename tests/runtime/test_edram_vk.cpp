// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// Tests of the pure part of the Vulkan EDRAM unit (runtime/video/edram_vk_plan.h), CPU only: Vulkan enums from the
// headers, no Vulkan call, no device. Every expected value is computed by hand from Xenia's formulas (xenia-canary
// 67f16c4e315f src/xenia/gpu, cited as "X file:line" next to each computation) or from the observed trace
// (the local capture trace12 (not distributed)), never from this code's output. "CONTROL" names a negative control.
//
//   tests/run_edram_vk_tests.sh
#include "draw_state.h"
#include "edram.h"
#include "edram_vk_plan.h"
#include "front_buffer.h"  // TiledOffset (the serial reference scan)
#include "render_targets_vk.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace runtime::video;
using edram::ColorFormat;
using edram::DepthFormat;
using edram::Key;
using edram::Msaa;

namespace {
int failures = 0;
void Expect(bool ok, const char* what, int line)
{
    std::printf("%s %d %s\n", ok ? "pass" : "FAIL", line, what);
    failures += !ok;
}
#define CHECK(...) Expect((__VA_ARGS__), #__VA_ARGS__, __LINE__)
#define CONTROL(...) Expect(!(__VA_ARGS__), "CONTROL " #__VA_ARGS__, __LINE__)

// FM4's five keys (edram_NOTES.md §1b): 4x colour storage 2 and 4x depth D24FS8 (the fast clears), 1x colour storage 2
// (the UI, RB_COLOR_INFO format 10), 1x colour storage 0 (the GAMMA draws, format 1), 1x depth (the resolve's clear).
const Key kAc = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10);
const Key kAd = Key::Depth(720, 16, Msaa::X4, DepthFormat::D24FS8);
const Key kBc2 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10);
const Key kBc0 = Key::Color(0, 16, Msaa::X1, ColorFormat::k8_8_8_8);
const Key kBd = Key::Depth(720, 16, Msaa::X1, DepthFormat::D24FS8);
// The 7e3 keys (storage 3: RB_COLOR_INFO formats 3 and 12), 4x and 1x, on the same tiles.
const Key kA7 = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float);
const Key kB7 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float);

void TestFormats()
{
    CHECK(HostColorFormat(ColorFormat::k8_8_8_8) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(HostColorFormat(ColorFormat::k2_10_10_10) == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    // X vulkan_render_target_cache.cc:1601-1603; pipeline_cache.cpp:80 HostColorFormat(3) is the same (renderer.cpp:699 checks).
    CHECK(HostColorFormat(ColorFormat::k2_10_10_10_Float) == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(HostColorFormat(ColorFormat::k16_16) == VK_FORMAT_R16G16_SNORM);  // FM4 Recomp: X vulkan_render_target_cache.cc:1604-1608
    CONTROL(HostColorFormat(ColorFormat::k16_16_Float) != VK_FORMAT_UNDEFINED);  // not mapped (edram::SupportedKey refuses it)
    CHECK(HostDepthFormat(DepthFormat::D24FS8, false) == VK_FORMAT_D32_SFLOAT_S8_UINT);
    CHECK(HostDepthFormat(DepthFormat::D24FS8, true) == VK_FORMAT_D32_SFLOAT_S8_UINT);  // X vulkan_render_target_cache.cc:1581-1588
    CHECK(HostDepthFormat(DepthFormat::D24S8, true) == VK_FORMAT_D24_UNORM_S8_UINT);
    CHECK(HostDepthFormat(DepthFormat::D24S8, false) == VK_FORMAT_D32_SFLOAT_S8_UINT);
}

void TestClassify()
{
    std::string why;
    // The observed traffic (trace12, 400 swaps): 4x s2 -> 1x s2 (395), 1x s2 -> 4x s2 (395), 4x s2 -> 1x s0 (38),
    // 1x s0 -> 4x s2 (38), 1x depth -> 4x depth (394).
    CHECK(ClassifyTransfer(kAc, kBc2, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kBc2, kAc, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kAc, kBc0, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kBc0, kAc, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kBd, kAd, why) == TransferKind::Depth);
    // A multisampled depth source (sampler2DMS / usampler2DMS: the local capture draw_hdr4 (not distributed) draw 25372).
    CHECK(ClassifyTransfer(kAd, kBd, why) == TransferKind::Depth);
    // An unchanged sample count with a format change: a reinterpretation through the 32-bit packing (X :3387-3700).
    CHECK(ClassifyTransfer(kBc2, kBc0, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(Key::Color(0, 16, Msaa::X4, ColorFormat::k8_8_8_8), kAc, why) == TransferKind::Color);
    // 7e3 (storage 3) with every other accepted storage, both directions, at 4x <-> 1x and at an unchanged count.
    CHECK(ClassifyTransfer(kAc, kA7, why) == TransferKind::Color && ClassifyTransfer(kA7, kAc, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kA7, kBc2, why) == TransferKind::Color && ClassifyTransfer(kBc0, kA7, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kA7, kB7, why) == TransferKind::Color && ClassifyTransfer(kB7, kA7, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kA7, kA7, why) == TransferKind::Refused && why.find("own source") != std::string::npos);
    CHECK(ClassifyTransfer(Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16), kA7, why) == TransferKind::Color);  // TestK16_16
    CHECK(ClassifyTransfer(Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16_Float), kA7, why) == TransferKind::Refused &&
          why.find("storage format other than") != std::string::npos);
    CHECK(ClassifyTransfer(kA7, Key::Color(0, 16, Msaa::X1, ColorFormat::k32_Float), why) == TransferKind::Color);  // TestFloat32
    CHECK(ClassifyTransfer(kA7, Key::Color(0, 16, Msaa::X1, ColorFormat::k32_32_Float), why) == TransferKind::Refused);
    // Different bases: accepted through the tile offset (X :2938-2954, one path for every mode; TestBases).
    CHECK(ClassifyTransfer(Key::Color(16, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), kB7, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(Key::Color(16, 16, Msaa::X4, ColorFormat::k2_10_10_10), kBc2, why) == TransferKind::Color);
    // Different pitches: X's separate source pitch (:2956-2987) for every mode (TestDepthPitch).
    CHECK(ClassifyTransfer(Key::Color(0, 8, Msaa::X4, ColorFormat::k2_10_10_10), kBc2, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(Key::Depth(720, 8, Msaa::X1, DepthFormat::D24FS8), kAd, why) == TransferKind::Depth);
    CHECK(ClassifyTransfer(Key::Color(0, 16, Msaa::X2, ColorFormat::k2_10_10_10), kBc2, why) == TransferKind::Color);  // 2x -> 1x (X :2868-2886)
    // FM4 Recomp: between depth formats: Depth (the shader reinterprets the guest bits, ConvertDepthFormat).
    CHECK(ClassifyTransfer(Key::Depth(720, 16, Msaa::X1, DepthFormat::D24S8), kAd, why) == TransferKind::Depth);
    // ConvertDepthFormat by hand (X :3591-3604 pack, :4100-4124 read):
    //   1.0 kD24FS8 -> 20e4 0xF00000 (exponent 15, mantissa 0) -> as unorm24 n = 0xF00000: (n + (n >> 23)) * 2^-24 = 15728641 / 2^24.
    //   0.5 kD24S8 -> RoundEven(0.5 * 16777215 = 8388607.5) = 8388608 = 0x800000 -> as 20e4: exponent 8, mantissa 0 = 2^(8-15) = 2^-7.
    //   2^-13 kD24FS8 -> 0x200000 (exponent 2) -> unorm24 2^21 / 2^24 = 0.125 (the depth self-check's x = 0, row 0 sample).
    CHECK(ConvertDepthFormat(1.0f, DepthFormat::D24FS8, DepthFormat::D24S8) == 15728641.0f / 16777216.0f);
    CHECK(ConvertDepthFormat(0.5f, DepthFormat::D24S8, DepthFormat::D24FS8) == 0.0078125f);
    CHECK(ConvertDepthFormat(1.0f / 8192.0f, DepthFormat::D24FS8, DepthFormat::D24S8) == 0.125f);
    CHECK(ConvertDepthFormat(0.0f, DepthFormat::D24FS8, DepthFormat::D24S8) == 0.0f && ConvertDepthFormat(0.0f, DepthFormat::D24S8, DepthFormat::D24FS8) == 0.0f);
    CHECK(ConvertDepthFormat(0.3f, DepthFormat::D24S8, DepthFormat::D24S8) == 0.3f);  // the same format: the float as is (X :3587-3590)
    CONTROL(ConvertDepthFormat(1.0f, DepthFormat::D24FS8, DepthFormat::D24S8) == 1.0f);  // a reinterpretation, not a value copy
    CHECK(ClassifyTransfer(kBd, kAc, why) == TransferKind::DepthToColor);  // X kDepthToColor (TestDepthToColor)
    CHECK(ClassifyTransfer(kAc, kBd, why) == TransferKind::ColorToDepth);  // X kColorToDepth (TestColorToDepth)
    CONTROL(ClassifyTransfer(kAc, kAc, why) != TransferKind::Refused);  // a key is never its own source
}

// Depth -> colour (X kDepthToColor), as the local capture draw_extent2 (not distributed) draw 25343 needs it: tiles 720-1023 of the 1x kD24FS8
// key at base 720 (kBd) into the 4x 7e3 colour key at base 0 (kA7).
void TestDepthToColor()
{
    std::string why;
    CHECK(ClassifyTransfer(kBd, kA7, why) == TransferKind::DepthToColor);
    CHECK(ClassifyTransfer(kBd, kB7, why) == TransferKind::DepthToColor && ClassifyTransfer(kBd, kBc0, why) == TransferKind::DepthToColor);
    // A multisampled or kD24S8 depth source (TestDepthToColorMs); FM4 Recomp: depth -> colour 1x <-> 2x too (MapTransferTexel's rule).
    CHECK(ClassifyTransfer(kAd, kA7, why) == TransferKind::DepthToColor);
    CHECK(ClassifyTransfer(Key::Depth(720, 16, Msaa::X1, DepthFormat::D24S8), kA7, why) == TransferKind::DepthToColor);
    CHECK(ClassifyTransfer(Key::Depth(720, 16, Msaa::X2, DepthFormat::D24FS8), kB7, why) == TransferKind::DepthToColor);  // 2x -> 1x (FM4 Recomp)
    CHECK(ClassifyTransfer(Key{0x602040u}, Key{0x082800u}, why) == TransferKind::DepthToColor);  // 1x depth -> 2x colour
    CHECK(ClassifyTransfer(kBd, Key::Color(0, 8, Msaa::X4, ColorFormat::k2_10_10_10_Float), why) == TransferKind::DepthToColor);  // TestDepthToColorPitch
    CHECK(ClassifyTransfer(kBd, Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16), why) == TransferKind::DepthToColor);  // TestK16_16
    CHECK(ClassifyTransfer(kBd, Key::Color(0, 16, Msaa::X4, ColorFormat::k32_Float), why) == TransferKind::Refused);

    // The texel mapping, by hand from X (vulkan_render_target_cache.cc:2516-2577, :2823-2828, :2857-2864, :2916-2954):
    // destination 4x tiles are 40x8 pixels, source 1x tiles 80x16; source tile = destination tile + 0 - 720; the
    // column swap of a 1x source is by 40. Destination (0, 360) is tile row 45 = tile 720 -> source tile 0; sample s
    // -> tile pixel (s & 1, s >> 1) -> x + 40.
    auto is = [](SourceTexel t, uint32_t x, uint32_t y) { return t.x == x && t.y == y && t.sample == 0; };
    CHECK(is(MapCrossAspectTexel(0, 360, 0, kBd, kA7), 40, 0));
    CHECK(is(MapCrossAspectTexel(0, 360, 1, kBd, kA7), 41, 0));
    CHECK(is(MapCrossAspectTexel(0, 360, 2, kBd, kA7), 40, 1));
    CHECK(is(MapCrossAspectTexel(0, 360, 3, kBd, kA7), 41, 1));
    CHECK(is(MapCrossAspectTexel(20, 360, 0, kBd, kA7), 0, 0));      // tile pixel 40 -> 0
    CHECK(is(MapCrossAspectTexel(39, 367, 3, kBd, kA7), 39, 15));    // 79 -> 39, 15
    CHECK(is(MapCrossAspectTexel(40, 360, 0, kBd, kA7), 120, 0));    // tile 721 -> source tile 1: 80 + 40
    CHECK(is(MapCrossAspectTexel(0, 400, 0, kBd, kA7), 40, 80));     // row 50 = tile 800 -> source tile 80 = row 5
    CHECK(is(MapCrossAspectTexel(639, 511, 3, kBd, kA7), 1239, 303)); // tile 1023 -> 303 = row 18 col 15; (79, 15) -> 39
    // Cross-check against X shaders/edram.xesli:23-59 XeEdramOffsetInts: colour 4x (0, 360) sample 1 is EDRAM sample
    // (2 * 0 + 1, 2 * 360 + 0) = (1, 720) -> tile 45 * 16 = 720, in-tile (1, 0); depth 1x (41, 0) of base 720 is tile 720,
    // in-tile (41, 0), and the depth swap moves 41 >= 40 to 1: the same EDRAM sample.
    // Wrapping (:2938-2954, & 2047): a 1x colour key at base 1600 over a 1x depth key at base 0: destination tile 640
    // (row 40, (0, 640)) is EDRAM tile 2240 & 2047 = 192 -> source row 12, column 0 -> (0 + 40, 192).
    CHECK(is(MapCrossAspectTexel(0, 640, 0, Key::Depth(0, 16, Msaa::X1, DepthFormat::D24FS8), Key::Color(1600, 16, Msaa::X1, ColorFormat::k8_8_8_8)), 40, 192));
    CONTROL(is(MapCrossAspectTexel(0, 360, 0, kBd, kA7), 0, 0));  // without the column swap

    // 20e4 (X spirv_shader_translator_rb.cc:236-326, remap 0, truncating): 0 -> 0 (denormal path, 0x800000 >> 24 = 0);
    // 1.0 = 0x3F800000 - (112 << 23) = 0x07800000 >> 3 = 0xF00000; 0.5 -> 0x07000000 >> 3 = 0xE00000; 0.75 = 0x3F400000 ->
    // 0xE80000; 1/3 = 0x3EAAAAAB -> 0x06AAAAAB >> 3 = 0xD55555 (truncated); 2^-16 = 0x37800000 (below 0x38800000):
    // (0x800000 >> min(113 - 111, 24)) = 0x200000 >> 3 = 0x40000 (a 20e4 denormal: 0x40000 / 2^20 * 2^-14 = 2^-16).
    CHECK(Depth32To20e4(0.0f) == 0u);
    CHECK(Depth32To20e4(1.0f) == 0xF00000u);
    CHECK(Depth32To20e4(0.5f) == 0xE00000u);
    CHECK(Depth32To20e4(0.75f) == 0xE80000u);
    CHECK(Depth32To20e4(1.0f / 3.0f) == 0xD55555u);
    CHECK(Depth32To20e4(std::ldexp(1.0f, -16)) == 0x40000u);
    // Round trip through the decode already tested (DecodeDepthClear / Float20e4To32, X xenos.cc:150-166).
    CHECK(Float20e4To32(Depth32To20e4(0.75f)) == 0.75f && Float20e4To32(Depth32To20e4(std::ldexp(1.0f, -16))) == std::ldexp(1.0f, -16));
    // The word (X :3614-3617): stencil 0-7, depth 8-31. Depth 1.0, stencil 0x5A -> 0xF000005A; the cleared 1x depth
    // (depth 0, stencil 0: trace12/draw_extent2's RB_DEPTH_CLEAR 0) -> 0.
    CHECK(PackDepthStencilWord(1.0f, 0x5A) == 0xF000005Au);
    CHECK(PackDepthStencilWord(0.0f, 0) == 0u);
    CHECK(PackDepthStencilWord(0.5f, 0x1FF) == 0xE00000FFu);  // only 8 stencil bits
    // Into 7e3 (X :3669-3690; xenos.h:303): 0xF000005A -> R 0x05A (e 0, m 90: 90 / 512), G 0, B (0xF0000 >> 0) & 0x3FF:
    // word >> 20 = 0xF00 & 0x3FF = 0x300 (e 6, m 0: 2^3 = 8), alpha 3 / 3 = 1.
    float c[4];
    UnpackTransferColor(0xF000005Au, 3, c);
    CHECK(c[0] == 90.0f / 512.0f && c[1] == 0.0f && c[2] == 8.0f && c[3] == 1.0f);
}

void TestTexelMap()
{
    // X vulkan_render_target_cache.cc:2597-2601: host sample bit 0 horizontal, bit 1 vertical (Vulkan's standard 4x
    // locations: 0 (0.375, 0.125), 1 (0.875, 0.375), 2 (0.125, 0.625), 3 (0.625, 0.875)). 4x -> 1x, :2804-2818:
    // sample = (x & 1) | (y & 1) << 1, pixel = (x >> 1, y >> 1).
    auto is = [](SourceTexel t, uint32_t x, uint32_t y, uint32_t s) { return t.x == x && t.y == y && t.sample == s; };
    CHECK(is(MapTransferTexel(0, 0, 0, Msaa::X4, Msaa::X1), 0, 0, 0));
    CHECK(is(MapTransferTexel(1, 0, 0, Msaa::X4, Msaa::X1), 0, 0, 1));
    CHECK(is(MapTransferTexel(0, 1, 0, Msaa::X4, Msaa::X1), 0, 0, 2));
    CHECK(is(MapTransferTexel(1, 1, 0, Msaa::X4, Msaa::X1), 0, 0, 3));
    CHECK(is(MapTransferTexel(1279, 719, 0, Msaa::X4, Msaa::X1), 639, 359, 3));
    // Tile alignment: 1x pixel (80, 16) starts tile 1 * 16 + 1 = 17 (80x16 pixels a tile); the 4x pixel (40, 8) starts the
    // same tile (40x8 pixels a tile, X render_target_cache.cc:355-359).
    CHECK(is(MapTransferTexel(80, 16, 0, Msaa::X4, Msaa::X1), 40, 8, 0));
    // 1x -> 4x, :2823-2828 and :2857-2864: x = x << 1 | (s & 1), y = y << 1 | (s >> 1).
    CHECK(is(MapTransferTexel(0, 0, 0, Msaa::X1, Msaa::X4), 0, 0, 0));
    CHECK(is(MapTransferTexel(0, 0, 1, Msaa::X1, Msaa::X4), 1, 0, 0));
    CHECK(is(MapTransferTexel(0, 0, 2, Msaa::X1, Msaa::X4), 0, 1, 0));
    CHECK(is(MapTransferTexel(0, 0, 3, Msaa::X1, Msaa::X4), 1, 1, 0));
    CHECK(is(MapTransferTexel(639, 359, 3, Msaa::X1, Msaa::X4), 1279, 719, 0));
    CHECK(is(MapTransferTexel(40, 8, 0, Msaa::X1, Msaa::X4), 80, 16, 0));
    // Round trip: every 1x pixel of a 4x4 block -> its 4x sample -> back.
    bool roundTrip = true;
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++)
        {
            const SourceTexel s = MapTransferTexel(x, y, 0, Msaa::X4, Msaa::X1);
            const SourceTexel back = MapTransferTexel(s.x, s.y, s.sample, Msaa::X1, Msaa::X4);
            roundTrip = roundTrip && back.x == x && back.y == y;
        }
    CHECK(roundTrip);
    CONTROL(is(MapTransferTexel(1, 0, 0, Msaa::X4, Msaa::X1), 0, 0, 2));  // the vertical-first order is not X's host order
    // An unchanged sample count: the same pixel and sample (X :2804-2864 remap only between different counts).
    CHECK(is(MapTransferTexel(5, 7, 2, Msaa::X4, Msaa::X4), 5, 7, 2));
    CHECK(is(MapTransferTexel(5, 7, 0, Msaa::X1, Msaa::X1), 5, 7, 0));
}

float FloatBits(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// 7e3 (X xenos.h:303; edram_vk_plan.h): e = bits 7-9 (bias 3), m = bits 0-6; e > 0: 2^(e - 3) (1 + m / 128), e == 0:
// m / 512. Every expectation below is that definition evaluated by hand (or by std::ldexp from it), never this code.
void Test7e3()
{
    // Decode: each code from the definition.
    CHECK(Float7e3To32(0x000) == 0.0f);
    CHECK(Float7e3To32(0x001) == 0.001953125f);    // 1 / 512 = 2^-9, the smallest denormal
    CHECK(Float7e3To32(0x040) == 0.125f);          // 64 / 512
    CHECK(Float7e3To32(0x07F) == 127.0f / 512.0f); // the largest denormal
    CHECK(Float7e3To32(0x080) == 0.25f);           // e 1: 2^-2
    CHECK(Float7e3To32(0x180) == 1.0f);            // e 3
    CHECK(Float7e3To32(0x1C0) == 1.5f);            // e 3, m 64
    CHECK(Float7e3To32(0x200) == 2.0f);            // e 4
    CHECK(Float7e3To32(0x380) == 16.0f);           // e 7
    CHECK(Float7e3To32(0x3FF) == 31.875f);         // 16 * (1 + 127 / 128)
    CHECK(Float7e3To32(0x400 | 0x180) == 1.0f);    // bits above 9 are ignored
    // CONTROL: X's CPU xenos.cc:103-119 (mantissa << 3) would give 1 + 64 * 2^-20 for 0x1C0.
    CONTROL(Float7e3To32(0x1C0) == 1.0f + 64.0f * std::ldexp(1.0f, -20));
    // All 1024 codes against the definition, and each exact in float16 (the R16G16B16A16_SFLOAT host target).
    bool definition = true, half = true;
    for (uint32_t c = 0; c < 1024; c++)
    {
        const uint32_t e = c >> 7, m = c & 0x7F;
        const float expected = e ? std::ldexp(1.0f + float(m) / 128.0f, int(e) - 3) : float(m) / 512.0f;
        definition = definition && Float7e3To32(c) == expected;
        half = half && float(_Float16(expected)) == expected;
    }
    CHECK(definition);
    CHECK(half);

    // Encode (X spirv_shader_translator_rb.cc:25-137): the codes above back, saturation and the zero cases.
    CHECK(Float32To7e3(0.0f) == 0x000 && Float32To7e3(0.001953125f) == 0x001 && Float32To7e3(0.125f) == 0x040);
    CHECK(Float32To7e3(127.0f / 512.0f) == 0x07F && Float32To7e3(0.25f) == 0x080 && Float32To7e3(1.0f) == 0x180);
    CHECK(Float32To7e3(1.5f) == 0x1C0 && Float32To7e3(2.0f) == 0x200 && Float32To7e3(16.0f) == 0x380 && Float32To7e3(31.875f) == 0x3FF);
    CHECK(Float32To7e3(32.0f) == 0x3FF && Float32To7e3(100.0f) == 0x3FF && Float32To7e3(INFINITY) == 0x3FF);  // NClamp's upper bound
    CHECK(Float32To7e3(NAN) == 0 && Float32To7e3(-1.0f) == 0 && Float32To7e3(-INFINITY) == 0);
    CHECK(Float32To7e3(-0.0f) == 0);  // not 0x200: -0's bits (1 << 31) - (124 << 23) = 0x42000000 -> 0x200 (edram_vk_plan.h)
    // Round to nearest even. Normal: 1 + 1/256 lies halfway between 0x180 (1) and 0x181 (1 + 1/128): to even 0x180;
    // 1 + 3/256 between 0x181 and 0x182: to 0x182; 1 + 1/256 + 2^-20 is above the tie: 0x181.
    CHECK(Float32To7e3(1.0f + 1.0f / 256.0f) == 0x180 && Float32To7e3(1.0f + 3.0f / 256.0f) == 0x182);
    CHECK(Float32To7e3(1.0f + 1.0f / 256.0f + std::ldexp(1.0f, -20)) == 0x181);
    // Denormal: 1.5 / 512 between m 1 and 2: to 2; 0.5 / 512 between 0 and 1: to 0; 127.5 / 512 between 0x07F and
    // 0x080 (0.25): to the even 0x080, the carry into the exponent.
    CHECK(Float32To7e3(1.5f / 512.0f) == 0x002 && Float32To7e3(0.5f / 512.0f) == 0x000 && Float32To7e3(127.5f / 512.0f) == 0x080);
    // The inherited inexact case (X/DirectXTex, APPROXIMATE): 2^-10 (1 + 2^-23) (bits 0x3A800001) is just above the
    // 0 / 0x001 tie, but exponent 117 shifts the significand right by 125 - 117 = 8 first: (0x800001 >> 8) = 0x8000,
    // + 0x7FFF + 0 = 0xFFFF -> 0, where exact rounding gives 0x001.
    CHECK(Float32To7e3(FloatBits(0x3A800001u)) == 0x000);
    CONTROL(Float32To7e3(FloatBits(0x3A800001u)) == 0x001);
    // Every code survives decode -> encode (the 7e3 host round trip of a transfer is lossless).
    bool roundTrip = true;
    for (uint32_t c = 0; c < 1024; c++)
        roundTrip = roundTrip && Float32To7e3(Float7e3To32(c)) == c;
    CHECK(roundTrip);
}

void TestPack()
{
    // X :3467-3490: R bits 0-9 = uint(r * 1023 + 0.5), alpha bits 30-31 = uint(a * 3 + 0.5). (1, 2, 3, 1) ->
    // 1 << 30 | 3 << 20 | 2 << 10 | 1 = 0x40300801.
    const float v10[4] = {1.0f / 1023.0f, 2.0f / 1023.0f, 3.0f / 1023.0f, 1.0f / 3.0f};
    CHECK(PackTransferColor(v10, 2) == 0x40300801u);
    // X :3402-3449: bytes R, G, B, A at bits 0, 8, 16, 24.
    const float v8[4] = {0x11 / 255.0f, 0x22 / 255.0f, 0x33 / 255.0f, 0x44 / 255.0f};
    CHECK(PackTransferColor(v8, 0) == 0x44332211u);
    CONTROL(PackTransferColor(v10, 0) == 0x40300801u);
    // Unpack (X :3627-3668) then pack is the identity for every 10-bit and 2-bit value and every byte: the transfer
    // moves the guest's 32 bits unchanged, as an EDRAM reinterpretation must.
    bool exact10 = true, exact8 = true;
    for (uint32_t k = 0; k < 1024; k++)
    {
        const uint32_t w = k | (1023 - k) << 10 | (k * 7 & 0x3FF) << 20 | (k & 3) << 30;
        float c[4];
        UnpackTransferColor(w, 2, c);
        exact10 = exact10 && PackTransferColor(c, 2) == w;
    }
    for (uint32_t k = 0; k < 256; k++)
    {
        const uint32_t w = k | (255 - k) << 8 | (k * 7 & 0xFF) << 16 | (k ^ 0x5A) << 24;
        float c[4];
        UnpackTransferColor(w, 0, c);
        exact8 = exact8 && PackTransferColor(c, 0) == w;
    }
    CHECK(exact10);
    CHECK(exact8);
    // Storage 2 -> storage 0 (the 4x s2 -> 1x s0 transfer): the word 0x40300801 read as 8_8_8_8 is bytes 01 08 30 40.
    float s0[4];
    UnpackTransferColor(0x40300801u, 0, s0);
    CHECK(s0[0] == 1 * (1.0f / 255.0f) && s0[1] == 8 * (1.0f / 255.0f) && s0[2] == 0x30 * (1.0f / 255.0f) && s0[3] == 0x40 * (1.0f / 255.0f));
    CHECK(PackTransferColor(s0, 0) == 0x40300801u);

    // 7e3 (X :3497-3542): R, G, B as 7e3 at bits 0, 10, 20; alpha uint(NClamp(a, 0, 1) * 3 + 0.5) at bit 30.
    // (1.5, 0.25, 2^-9, 2/3) -> 0x1C0 | 0x080 << 10 | 0x001 << 20 | 2 << 30 = (2 << 30) | 0x001201C0.
    const float v7[4] = {1.5f, 0.25f, 0.001953125f, 2.0f / 3.0f};
    CHECK(PackTransferColor(v7, 3) == (2u << 30 | 0x001201C0u));
    CONTROL(PackTransferColor(v7, 2) == (2u << 30 | 0x001201C0u));
    // Clamps (a float16 host target can hold any of these after blending): R 40 -> 0x3FF, G -1 -> 0, B NaN -> 0, A 5 -> 3.
    const float wild[4] = {40.0f, -1.0f, NAN, 5.0f};
    CHECK(PackTransferColor(wild, 3) == 0xC00003FFu);
    const float wildAlpha[4] = {0.0f, 0.0f, 0.0f, NAN};
    CHECK(PackTransferColor(wildAlpha, 3) == 0);
    // Unpack (X :3669-3690): the shader-form 7e3 decode, alpha / 3.
    float u7[4];
    UnpackTransferColor((2u << 30 | 0x001201C0u), 3, u7);
    CHECK(u7[0] == 1.5f && u7[1] == 0.25f && u7[2] == 0.001953125f && u7[3] == 2 * (1.0f / 3.0f));
    // Every 32-bit word whose fields are 7e3 codes survives unpack -> pack: a transfer moves the guest's bits unchanged.
    bool exact7 = true;
    for (uint32_t k = 0; k < 1024; k++)
    {
        const uint32_t w = k | (1023 - k) << 10 | (k * 7 & 0x3FF) << 20 | (k & 3) << 30;
        float c[4];
        UnpackTransferColor(w, 3, c);
        exact7 = exact7 && PackTransferColor(c, 3) == w;
    }
    CHECK(exact7);
    // Across storages, through the guest word. 7e3 2.0 (R code 0x200, alpha 0) read as 2_10_10_10: R = 512 / 1023; as
    // 8_8_8_8: bytes 00 02 00 00.
    const float two[4] = {2.0f, 0.0f, 0.0f, 0.0f};
    float x[4];
    UnpackTransferColor(PackTransferColor(two, 3), 2, x);
    CHECK(x[0] == 512 * (1.0f / 1023.0f) && x[1] == 0.0f && x[2] == 0.0f && x[3] == 0.0f);
    UnpackTransferColor(PackTransferColor(two, 3), 0, x);
    CHECK(x[0] == 0.0f && x[1] == 2 * (1.0f / 255.0f) && x[2] == 0.0f && x[3] == 0.0f);
    // The 2_10_10_10 word 0x40300801 read as 7e3: R code 1 = 2^-9, G 2 = 2 / 512, B 3 = 3 / 512, alpha 1 / 3; and back.
    UnpackTransferColor(0x40300801u, 3, x);
    CHECK(x[0] == 1.0f / 512.0f && x[1] == 2.0f / 512.0f && x[2] == 3.0f / 512.0f && x[3] == 1.0f / 3.0f);
    CHECK(PackTransferColor(x, 3) == 0x40300801u);
}

void TestClears()
{
    float c[4];
    // X vulkan_render_target_cache.cc:5366-5382.
    CHECK(DecodeColorClear(0xFFFFFFFFu, 0, c) && c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f && c[3] == 1.0f);  // trace12's texture resolve
    CHECK(DecodeColorClear(0u, 2, c) && c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f);          // trace12's front-buffer resolve
    CHECK(DecodeColorClear(0x40300801u, 2, c) && c[0] == 1 * (1.0f / 1023) && c[1] == 2 * (1.0f / 1023) && c[2] == 3 * (1.0f / 1023) &&
          c[3] == 1 * (1.0f / 3));
    CHECK(DecodeColorClear(0x44332211u, 0, c) && c[0] == 0x11 * (1.0f / 255) && c[3] == 0x44 * (1.0f / 255));
    // 7e3 (X :5383-5390 with the shader-form decode): R 0x1C0 = 1.5, G 0x080 = 0.25, B 0x001 = 2^-9, alpha 2 -> 2/3.
    // Storage 4 (16_16) clears through its R16G16_UINT view (DecodeColorClearUint, TestK16_16), never as floats.
    CONTROL(DecodeColorClear(0x7FFF8001u, 4, c));
    CHECK(DecodeColorClear((2u << 30 | 0x001201C0u), 3, c) && c[0] == 1.5f && c[1] == 0.25f && c[2] == 0.001953125f && c[3] == 2 * (1.0f / 3));
    // CONTROL: X's CPU xenos.cc:117 form, (3 + 124) << 23 | 64 << 3 = 0x3F800200 = 1.00006104, is not what is decoded.
    CONTROL(c[0] == 1.0f + std::ldexp(1.0f, -14));
    CHECK(DecodeColorClear(0u, 3, c) && c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f);
    CHECK(DecodeColorClear(0xFFFFFFFFu, 3, c) && c[0] == 31.875f && c[1] == 31.875f && c[2] == 31.875f && c[3] == 1.0f);
    CONTROL(DecodeColorClear(0u, 14, c));  // 32_FLOAT clears through DecodeColorClearUint (TestFloat32)
    float d = -1.0f;
    uint32_t s = 99;
    DecodeDepthClear(0u, DepthFormat::D24FS8, d, s);  // trace12 RB_DEPTH_CLEAR 00000000
    CHECK(d == 0.0f && s == 0);
    // X xenos.cc:150-166: f24 0x800000 = exponent 8, mantissa 0 -> (8 + 112) << 23 = 2^-7.
    DecodeDepthClear(1u << 31, DepthFormat::D24FS8, d, s);
    CHECK(d == 0.0078125f && s == 0);
    // 0xF00000: exponent 15 -> 127 << 23 = 1.0 (edram::PrepareResolve's limit); stencil 0xFF.
    DecodeDepthClear(0xF00000FFu, DepthFormat::D24FS8, d, s);
    CHECK(d == 1.0f && s == 0xFF);
    // Denormal f24 1: mantissa 1, shift 31 - 11 = 20, exponent 1 - 20 = -19 -> 93 << 23 = 2^-34 (= 2^-20 * 2^(1 - 15)).
    DecodeDepthClear(0x0000015Au, DepthFormat::D24FS8, d, s);
    CHECK(d == std::ldexp(1.0f, -34) && s == 0x5A);
    // X xenos.h:432-438: UNorm24To32(n) = (n + (n >> 23)) / 2^24.
    DecodeDepthClear(0xFFFFFF00u, DepthFormat::D24S8, d, s);
    CHECK(d == 1.0f && s == 0);
    DecodeDepthClear(0xC0000000u, DepthFormat::D24S8, d, s);
    CHECK(d == 12582913.0f / 16777216.0f);
    CONTROL(d == 0.75f);  // X's comment: n / 0xFFFFFF would be wrong; the + (n >> 23) term is what makes 0xFFFFFF exactly 1
}

// A register file with a distinct value at every index (v(i) = 7 i + 1).
uint32_t FakeRead(const void*, uint32_t index) { return index * 7 + 1; }
uint32_t V(uint32_t index) { return index * 7 + 1; }

void TestGather()
{
    const edram::ResolveRegisters r = GatherResolveRegisters(FakeRead, nullptr);
    // X register_table.inc indices (as edram.h ResolveRegisters lists them).
    CHECK(r.surfaceInfo == V(0x2000) && r.colorInfo[0] == V(0x2001) && r.depthInfo == V(0x2002));
    CHECK(r.colorInfo[1] == V(0x2003) && r.colorInfo[2] == V(0x2004) && r.colorInfo[3] == V(0x2005));
    CHECK(r.modeControl == V(0x2208) && r.copyControl == V(0x2318) && r.copyDestBase == V(0x2319) && r.copyDestPitch == V(0x231A) &&
          r.copyDestInfo == V(0x231B));
    CHECK(r.depthClear == V(0x231D) && r.colorClear == V(0x231E) && r.colorClearLo == V(0x231F));
    CHECK(r.vtxCntl == V(0x2302) && r.suScModeCntl == V(0x2205) && r.windowOffset == V(0x2080) && r.windowScissorTl == V(0x2081) &&
          r.windowScissorBr == V(0x2082) && r.screenScissorTl == V(0x200E) && r.screenScissorBr == V(0x200F));
    CHECK(r.fetch0 == V(0x4800) && r.fetch1 == V(0x4801));
    CHECK(r.vertexWords[0] == 0 && r.vertexWords[5] == 0);
    CHECK(ResolveVertexAddress(0x1160BDF3u) == 0x1160BDF0u);  // trace12 cp.txt:26351 "fetch0 1160BDF3": type 3, address bits 2-31
}

// A float as the guest stores it (big-endian), loaded as a little-endian word; the fetch constant's k8in32 (2) swap
// in DecodeResolve turns it back (X draw_util.cc:789-800).
uint32_t GuestFloat(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return __builtin_bswap32(bits);
}

// trace12's resolves (edram_NOTES_2.md): the 1x surface 14000500, the rectangle 0,0-1280,720 (vertices (0,0), (1280,0),
// (0,720)), window scissor BR 02D00500, fetch1 1000001A.
edram::ResolveRegisters Trace12Resolve(uint32_t colorInfo, uint32_t copyControl, uint32_t destBase, uint32_t destInfo, uint32_t colorClear)
{
    edram::ResolveRegisters r;
    r.surfaceInfo = 0x14000500, r.colorInfo[0] = colorInfo, r.depthInfo = 0x000102D0, r.modeControl = 6;
    r.copyControl = copyControl, r.copyDestBase = destBase, r.copyDestPitch = 0x02D00500, r.copyDestInfo = destInfo;
    r.colorClear = r.colorClearLo = colorClear;
    r.windowScissorBr = 0x02D00500, r.screenScissorBr = 0x20002000;
    r.fetch0 = 0x1160BDF3, r.fetch1 = 0x1000001A;
    const float v[6] = {0.0f, 0.0f, 1280.0f, 0.0f, 0.0f, 720.0f};
    for (int i = 0; i < 6; i++)
        r.vertexWords[i] = GuestFloat(v[i]);
    return r;
}

uint32_t WordAt(const std::vector<uint8_t>& b, uint32_t offset)
{
    uint32_t w;
    std::memcpy(&w, b.data() + offset, 4);
    return w;
}

// FM4 Recomp (M7, edram_vk.cpp Resolve: FM4_RESOLVE_RESIDENT converts fast resolves too): for the fast-path pairs the resident
// conversion accepts (storage 0 k_8_8_8_8 -> format 6, storage 2 k_2_10_10_10 -> 7, 1x, exp bias 0) the converting path's word,
// ResolveConvertPixel (packed before the endian swap), equals the fast path's (ResolveToGuest: the texel, swapped when the
// destination asks, before the endian swap; X resolve.xesli:769-797). Hand vector: 0x44332211 with swap -> 0x44112233 (the swap
// exchanges bytes 0 and 2). Every 8-bit channel value, and every 10-bit value of the 2_10_10_10 colour channels, with alpha.
void TestFastResidentWords()
{
    std::string why;
    const edram::ResolveInfo tex = edram::DecodeResolve(Trace12Resolve(0x00000000, 0x00100100, 0x13F2B000, 0x01000302, 0xFFFFFFFF));
    const edram::ResolveInfo fb = edram::DecodeResolve(Trace12Resolve(0x00020000, 0x00100300, 0x1B441000, 0x01000380, 0));
    CHECK(ResolveUsesFastPath(tex, 0) && ResolveFullConvertible(tex, 0, why) && ResolveUsesFastPath(fb, 2) && ResolveFullConvertible(fb, 2, why));
    auto fast = [](const edram::ResolveInfo& info, bool is8888, uint32_t w) {
        if (info.destSwap)
            w = is8888 ? (w & ~0x00FF00FFu) | (w & 0xFFu) << 16 | ((w >> 16) & 0xFFu) : (w & ~0x3FF003FFu) | (w & 0x3FFu) << 20 | ((w >> 20) & 0x3FFu);
        return w;
    };
    const uint32_t hand = 0x44332211u;
    CHECK(ResolveConvertPixel(tex, 0, &hand) == 0x44112233u && fast(tex, true, hand) == 0x44112233u);
    bool same8 = true, same10 = true;
    for (uint32_t v = 0; v < 256; v++)
        for (uint32_t channel = 0; channel < 4; channel++)
        {
            const uint32_t texel = (v << (8 * channel)) | (0x5Au << (8 * ((channel + 1) % 4)));
            same8 = same8 && ResolveConvertPixel(tex, 0, &texel) == fast(tex, true, texel);
        }
    for (uint32_t v = 0; v < 1024; v++)
        for (uint32_t channel = 0; channel < 3; channel++)
        {
            const uint32_t texel = (v << (10 * channel)) | (uint32_t(v & 3) << 30);
            same10 = same10 && ResolveConvertPixel(fb, 2, &texel) == fast(fb, false, texel);
        }
    CHECK(same8 && same10);
}

// FM4_RESOLVE_GPU_WORDS: the converted words stored at the hand-computed tiled offsets of TestResolveToGuest (front buffer,
// pitch 1280, endian 0: the word itself), and the same bytes as ResolveConvertToGuest when the words are ResolveConvertPixel's.
void TestResolveWordsToGuest()
{
    std::string why;
    const edram::ResolveInfo fb = edram::DecodeResolve(Trace12Resolve(0x00020000, 0x00100300, 0x1B441000, 0x01000380, 0));
    const uint32_t w = 1280, h = 720;
    std::vector<uint32_t> words(size_t(w) * h), texels(size_t(w) * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            words[size_t(y) * w + x] = 0xA0000000u | y << 12 | x, texels[size_t(y) * w + x] = 1u << 30 | (y & 0x3FF) << 20 | (x & 0x3FF);
    std::vector<uint8_t> dest(fb.destExtentLength, 0xEE);
    CHECK(ResolveWordsToGuest(fb, 2, words.data(), dest.data(), uint32_t(dest.size()), why));
    CHECK(WordAt(dest, 0) == 0xA0000000u && WordAt(dest, 4) == 0xA0000001u && WordAt(dest, 16) == 0xA0001000u && WordAt(dest, 64) == 0xA0000008u);
    CHECK(WordAt(dest, 280) == (0xA0000000u | 3u << 12 | 2u) && WordAt(dest, 4096) == 0xA0000020u && WordAt(dest, 163840) == 0xA0020000u);
    CHECK(WordAt(dest, 3766140) == (0xA0000000u | 719u << 12 | 1279u));
    std::vector<uint32_t> converted(texels.size());
    for (size_t i = 0; i < texels.size(); i++)
        converted[i] = ResolveConvertPixel(fb, 2, &texels[i]);
    std::vector<uint8_t> a(fb.destExtentLength, 0xEE), b(fb.destExtentLength, 0xEE);
    CHECK(ResolveWordsToGuest(fb, 2, converted.data(), a.data(), uint32_t(a.size()), why) &&
          ResolveConvertToGuest(fb, 2, texels.data(), b.data(), uint32_t(b.size()), why) && a == b);
    std::vector<uint8_t> untouched(fb.destExtentLength, 0xEE), small(16, 0xEE);
    CONTROL(ResolveWordsToGuest(fb, 2, words.data(), small.data(), uint32_t(small.size()), why));  // past the end: refused
    CHECK(small == std::vector<uint8_t>(16, 0xEE));                                                 // ... and nothing written
    edram::ResolveInfo half = fb;
    half.destBytesLog2 = 1;
    CONTROL(ResolveWordsToGuest(half, 2, words.data(), untouched.data(), uint32_t(untouched.size()), why));  // not 32 bpp
}

void TestResolveToGuest()
{
    std::string why;
    // The front-buffer resolve: RB_COPY_CONTROL 00100300, DEST_INFO 01000380 (format 7, endian 0, swap 1).
    const edram::ResolveInfo fb = edram::DecodeResolve(Trace12Resolve(0x00020000, 0x00100300, 0x1B441000, 0x01000380, 0));
    CHECK(fb.ok && fb.copy && fb.x0 == 0 && fb.y0 == 0 && fb.x1 == 1280 && fb.y1 == 720 && fb.destFormat == 7 && fb.destSwap && fb.destEndian == 0);
    CHECK(fb.destExtentStart == 0x1B441000u && fb.destExtentLength == 3768320u);  // 1280 x 736 x 4 (edram_NOTES_2.md)
    CHECK(ResolveConvertible(fb, 2, why));
    // Texel (x, y) of the source: R = x, B = y (10 bits), alpha 1. After the swap (X resolve.xesli:774-777): R = y, B = x.
    const uint32_t w = 1280, h = 720;
    std::vector<uint32_t> texels(size_t(w) * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            texels[size_t(y) * w + x] = 1u << 30 | (y & 0x3FF) << 20 | (x & 0x3FF);
    std::vector<uint8_t> dest(fb.destExtentLength, 0xEE);
    CHECK(ResolveToGuest(fb, 2, texels.data(), dest.data(), uint32_t(dest.size()), why));
    // Offsets by hand from X texture_util.cc:443-459 GetTiledOffset2D (pitch 1280, 4 bytes): (0,0) 0; (1,0): micro 4 -> 4;
    // (0,1): (y & 1) << 4 -> 16; (8,0): ((x >> 3) & 3) << 6 -> 64; (2,3): micro (2 + 8) << 2 = 40 -> 64 + 8 + 16 = 88 ->
    // (88 & 0x1C0) << 2 = 256 + (88 & 0x3F) = 24 -> 280; (32,0): macro 1 << 9 = 512 -> 512 << 3 = 4096; (0,32): macro
    // 40 << 9 = 20480 -> 163840; (1279,719): macro 919 << 9 = 470528, micro 63 << 2 = 252 -> 471036 -> 3764224 + 1792 + 64
    // + 60 = 3766140.
    CHECK(WordAt(dest, 0) == 0x40000000u);
    CHECK(WordAt(dest, 4) == (1u << 30 | 1u << 20));          // (1,0): B = 1 after the swap
    CHECK(WordAt(dest, 16) == (1u << 30 | 1u));               // (0,1): R = 1
    CHECK(WordAt(dest, 64) == (1u << 30 | 8u << 20));         // (8,0)
    CHECK(WordAt(dest, 280) == 0x40200003u);                  // (2,3)
    CHECK(WordAt(dest, 4096) == 0x42000000u);                 // (32,0)
    CHECK(WordAt(dest, 163840) == 0x40000020u);               // (0,32)
    CHECK(WordAt(dest, 3766140) == 0x4FF002CFu);              // (1279,719): R = 719, B = 1279 & 0x3FF = 255
    // Bytes in memory are little-endian (endian 0, X XeEndianSwap32 kNone): (2,3) is 03 00 20 40.
    CHECK(dest[280] == 0x03 && dest[281] == 0x00 && dest[282] == 0x20 && dest[283] == 0x40);
    // CONTROL: the padding rows 720-735 are not written. (0,720): macro 22 * 40 << 9 = 450560 -> 3604480, (y & 16) << 7
    // = 2048 -> 3606528.
    CONTROL(WordAt(dest, 3606528) != 0xEEEEEEEEu);
    // Exactly the rectangle's bytes were written: the same conversion over a buffer filled with another byte agrees
    // with the first one exactly where it wrote.
    std::vector<uint8_t> other(fb.destExtentLength, 0x11);
    CHECK(ResolveToGuest(fb, 2, texels.data(), other.data(), uint32_t(other.size()), why));
    size_t written = 0;
    for (size_t i = 0; i < dest.size(); i++)
        written += dest[i] == other[i];
    CHECK(written == size_t(w) * h * 4);

    // The texture resolve: RB_COLOR_INFO 0 (storage 0), COPY_CONTROL 00100100, DEST_INFO 01000302 (format 6, endian 2,
    // swap 1), clear FFFFFFFF.
    const edram::ResolveInfo tex = edram::DecodeResolve(Trace12Resolve(0x00000000, 0x00100100, 0x13F2B000, 0x01000302, 0xFFFFFFFF));
    CHECK(tex.ok && tex.destFormat == 6 && tex.destEndian == 2 && tex.destSwap && tex.source == Key::Color(0, 16, Msaa::X1, ColorFormat::k8_8_8_8));
    for (auto& t : texels)
        t = 0x44332211u;  // R8G8B8A8 bytes 11 22 33 44
    std::vector<uint8_t> dest2(tex.destExtentLength, 0xEE);
    CHECK(ResolveToGuest(tex, 0, texels.data(), dest2.data(), uint32_t(dest2.size()), why));
    // Swap (X resolve.xesli:769-772): 0x44112233; k8in32 (X xenos.h:1070-1086): 0x33221144; stored little-endian: 44 11 22 33.
    CHECK(dest2[280] == 0x44 && dest2[281] == 0x11 && dest2[282] == 0x22 && dest2[283] == 0x33);
    CHECK(WordAt(dest2, 3766140) == 0x33221144u);

    // Refusals (nothing written).
    std::vector<uint8_t> dest3(fb.destExtentLength, 0xEE);
    CONTROL(ResolveToGuest(tex, 2, texels.data(), dest3.data(), uint32_t(dest3.size()), why));  // storage disagrees with the key
    const edram::ResolveInfo cross = edram::DecodeResolve(Trace12Resolve(0x00020000, 0x00100100, 0x13F2B000, 0x01000302, 0));
    CHECK(cross.ok && !ResolveConvertible(cross, 2, why) && why.find("converting resolve") != std::string::npos);  // storage 2 -> format 6
    CONTROL(ResolveToGuest(fb, 2, texels.data(), dest3.data(), 3766140, why));  // the last texel would land past the buffer
    // FM4 Recomp (M7, parallel_rows.h): the bound check runs by rows in parallel; the refusal still names the first texel in
    // row-major order that does not fit, found here by a serial scan over TiledOffset (several texels of rows 718 and 719 land
    // past 3766140 bytes in the tiled layout, so the first is not the last texel).
    {
        std::string first;
        for (uint32_t y = 0; y < 720 && first.empty(); y++)
            for (uint32_t x = 0; x < 1280 && first.empty(); x++)
                if (uint64_t(TiledOffset(x, y, 1280, 2)) + 4 > 3766140)
                    first = "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
        CHECK(!first.empty() && why.find(first) != std::string::npos);
    }
    size_t touched = 0;
    for (uint8_t b : dest3)
        touched += b != 0xEE;
    CHECK(touched == 0);
    // A 4x source (the scene surface 0A020280, rectangle 0,0-640,360) is refused.
    edram::ResolveRegisters r4 = Trace12Resolve(0x00020000, 0x00100300, 0x1B441000, 0x01000380, 0);
    r4.surfaceInfo = 0x0A020280;
    const float v[6] = {0.0f, 0.0f, 640.0f, 0.0f, 0.0f, 360.0f};
    for (int i = 0; i < 6; i++)
        r4.vertexWords[i] = GuestFloat(v[i]);
    const edram::ResolveInfo four = edram::DecodeResolve(r4);
    CHECK(four.ok && !ResolveConvertible(four, 2, why) && why.find("4x source") != std::string::npos);
    // A 7e3 source (RB_COLOR_INFO format 3) into texture format 7 decodes (DecodeResolve keys storage 3), but its copy is
    // always X's converting resolve (no bitwise-equivalent texture format, X xenos.h:616-641): refused by name, with the
    // shape, before anything is recorded or written.
    const edram::ResolveInfo f7 = edram::DecodeResolve(Trace12Resolve(0x00030000, 0x00100300, 0x1B441000, 0x01000380, 0));
    CHECK(f7.ok && f7.source == kB7 && !ResolveConvertible(f7, 3, why) && why.find("7e3 source") != std::string::npos &&
          why.find("sample select 0 to texture format 7 (exp bias 0, swap 1, endian 0)") != std::string::npos);
    std::vector<uint8_t> dest7(f7.destExtentLength, 0xEE);
    CONTROL(ResolveToGuest(f7, 3, texels.data(), dest7.data(), uint32_t(dest7.size()), why));
    touched = 0;
    for (uint8_t b : dest7)
        touched += b != 0xEE;
    CHECK(touched == 0);
}

// trace12's per-frame sequence (edram_NOTES_2.md; cp.txt e.g. 26340-26360) through the CPU model with the bindings the
// unit builds (BindRegistersFor), every transfer classified: 0 refusals, and exactly the transfers X's ChangeOwnership
// (render_target_cache.cc:1539-1699) implies for it.
RenderTarget Target4x()  // "surface 0A020280 color 00020000 depth 000102D0", scissor 8192x8192: rows min(360, 8192)
{
    RenderTarget t;
    t.pitch = 640, t.msaa = 2, t.samples = 4, t.hizPitch = 640, t.colorBase = 0, t.colorFormatRaw = 2, t.colorFormat = 2, t.depthBase = 720,
    t.depthFormat = 1, t.targetHeight = 360;
    return t;
}
RenderTarget Target1x(uint32_t rawFormat)  // "surface 14000500 color 000A0000 / 00010000", scissor 1280x720
{
    RenderTarget t;
    t.pitch = 1280, t.hizPitch = 1280, t.colorBase = 0, t.colorFormatRaw = rawFormat, t.colorFormat = rawFormat == 10 ? 2 : rawFormat, t.depthBase = 720,
    t.depthFormat = 1, t.targetHeight = 720;
    return t;
}

bool AllClassified(const std::vector<edram::Transfer>& list, Key dest, TransferKind kind)
{
    std::string why;
    for (const edram::Transfer& t : list)
        if (ClassifyTransfer(t.source, dest, why) != kind)
            return false;
    return true;
}

void TestTrace12Frame()
{
    edram::Ownership o;
    using T = edram::Transfer;
    // The register words BindRegistersFor rebuilds (X registers.h).
    const edram::BindRegisters r4 = BindRegistersFor(Target4x(), true, true);
    CHECK(r4.surfaceInfo == 0x0A020280u && r4.colorInfo[0] == 0x00020000u && r4.depthInfo == 0x000102D0u && r4.colorMask == 0xF && r4.depthControl == 1);
    const edram::BindRegisters r1 = BindRegistersFor(Target1x(10), true, false);
    CHECK(r1.surfaceInfo == 0x14000500u && r1.colorInfo[0] == 0x000A0000u && r1.depthControl == 0);
    // 1. The 4x fast clear on an empty map: 720 + 720 tiles claimed, nothing transferred.
    edram::Binding b = edram::Bind(o, r4, 360);
    CHECK(b.ok && b.color[0] == kAc && b.depth == kAd && b.transfers[0].empty() && b.transfers[1].empty());
    // 2. The UI draw (format 10 -> storage 2): tiles 0-719 from the 4x colour key.
    b = edram::Bind(o, r1, 720);
    CHECK(b.ok && b.color[0] == kBc2 && b.depth.Empty() && b.transfers[1] == std::vector<T>{{0, 720, kAc}} && AllClassified(b.transfers[1], kBc2, TransferKind::Color));
    // 3. The front-buffer resolve with both clears: no copy transfer (the source owns its span), the depth clear's
    //    tiles are wholly inside the clear rectangle (the cutout), so no depth transfer either.
    edram::ResolvePlan p = edram::PrepareResolve(o, edram::DecodeResolve(Trace12Resolve(0x00020000, 0x00100300, 0x1B441000, 0x01000380, 0)));
    CHECK(p.ok && p.copy && p.copySource == kBc2 && p.copyTransfers.empty() && p.hasDepthClear && p.depthClear.target == kBd &&
          p.depthClear.transfers.empty() && p.hasColorClear && p.colorClear.target == kBc2 && p.colorClear.transfers.empty());
    // 4. The next 4x fast clear: colour 0-719 back from the 1x key, depth 720-1439 from the 1x depth key.
    b = edram::Bind(o, r4, 360);
    CHECK(b.ok && b.transfers[1] == std::vector<T>{{0, 720, kBc2}} && b.transfers[0] == std::vector<T>{{720, 1440, kBd}});
    CHECK(AllClassified(b.transfers[1], kAc, TransferKind::Color) && AllClassified(b.transfers[0], kAd, TransferKind::Depth));
    // 5. The GAMMA draw (format 1 -> storage 0): 4x storage 2 -> 1x storage 0.
    b = edram::Bind(o, BindRegistersFor(Target1x(1), true, false), 720);
    CHECK(b.ok && b.color[0] == kBc0 && b.transfers[1] == std::vector<T>{{0, 720, kAc}} && AllClassified(b.transfers[1], kBc0, TransferKind::Color));
    // 6. The texture resolve: from the storage-0 key, colour clear only.
    p = edram::PrepareResolve(o, edram::DecodeResolve(Trace12Resolve(0x00000000, 0x00100100, 0x13F2B000, 0x01000302, 0xFFFFFFFF)));
    CHECK(p.ok && p.copySource == kBc0 && p.copyTransfers.empty() && !p.hasDepthClear && p.hasColorClear && p.colorClear.transfers.empty());
    // 7. The 4x fast clear: colour from the storage-0 key (1x s0 -> 4x s2); the depth tiles are still the 4x key's.
    b = edram::Bind(o, r4, 360);
    CHECK(b.ok && b.transfers[1] == std::vector<T>{{0, 720, kBc0}} && b.transfers[0].empty() && AllClassified(b.transfers[1], kAc, TransferKind::Color));
    // A depth-only binding of the 1x depth key after step 7 needs 4x -> 1x depth: a multisampled depth source, accepted.
    b = edram::Bind(o, BindRegistersFor(Target1x(10), false, true), 720);
    CHECK(b.ok && b.transfers[0] == std::vector<T>{{720, 1440, kAd}} && AllClassified(b.transfers[0], kBd, TransferKind::Depth));
}

// The 7e3 scene (not traced on our disc: trace13 has no RB_COLOR_INFO format 3 or 12 before swap 400). ForzaRecomp
// OBSERVED FM4's first 7e3 draw as a 640x360 4x clear into RB_COLOR_INFO 0x00030000 with depth base 0x400:
// a HYPOTHESIS here. The transfers X's ChangeOwnership implies, each classified as the unit executes it.
void Test7e3Frame()
{
    edram::Ownership o;
    using T = edram::Transfer;
    RenderTarget t7 = Target4x();
    t7.colorFormatRaw = t7.colorFormat = 3;
    // 1. The 2_10_10_10 scene, then the 7e3 target on the same tiles: 4x s2 -> 4x s3 (a format change at 4x).
    CHECK(edram::Bind(o, BindRegistersFor(Target4x(), true, true), 360).ok);
    edram::Binding b = edram::Bind(o, BindRegistersFor(t7, true, true), 360);
    CHECK(b.ok && b.color[0] == kA7 && b.transfers[1] == std::vector<T>{{0, 720, kAc}} && b.transfers[0].empty() &&
          AllClassified(b.transfers[1], kA7, TransferKind::Color));
    // 2. The 1x UI (format 10): 4x s3 -> 1x s2.
    b = edram::Bind(o, BindRegistersFor(Target1x(10), true, false), 720);
    CHECK(b.ok && b.transfers[1] == std::vector<T>{{0, 720, kA7}} && AllClassified(b.transfers[1], kBc2, TransferKind::Color));
    // 3. Format 12 at 1x: the 1x 7e3 key, 1x s2 -> 1x s3.
    b = edram::Bind(o, BindRegistersFor(Target1x(12), true, false), 720);
    CHECK(b.ok && b.color[0] == kB7 && b.transfers[1] == std::vector<T>{{0, 720, kBc2}} && AllClassified(b.transfers[1], kB7, TransferKind::Color));
    // 4. ForzaRecomp's depth base 0x400 (1024) under the 4x 7e3 clear: tiles 1024-1439 still belong to the 4x depth key
    //    at base 720: 4x -> 4x at another base, a multisampled depth source (accepted). The colour side is accepted (1x s3 ->
    //    4x s3). (On our disc the owner is the 1x depth key of the resolve's clear: the local capture draw_xaspect1 (not distributed), TestBases.)
    t7.depthBase = 1024;
    b = edram::Bind(o, BindRegistersFor(t7, true, true), 360);
    const Key d1024 = Key::Depth(1024, 16, Msaa::X4, DepthFormat::D24FS8);
    std::string why;
    CHECK(b.ok && b.depth == d1024 && b.transfers[0] == std::vector<T>{{1024, 1440, kAd}} && ClassifyTransfer(kAd, d1024, why) == TransferKind::Depth);
    CHECK(AllClassified(b.transfers[1], kA7, TransferKind::Color) && b.transfers[1] == std::vector<T>{{0, 720, kB7}});
}

uint32_t FloatBits(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// Colour -> depth (X kColorToDepth + kColorToStencilBit), as the local capture draw_hdr4 (not distributed) draw 25372 needs it (cp.txt:64495:
// the regular 4x clear, colour 00030000, depth 000102D0, after the HDR resolves): tiles 720-1023 of the 2x 7e3 colour key
// C88000 (base 0) into the 4x kD24FS8 key 7082D0 (base 720), and tiles 1024-1439 of the 2x depth key 688400 (base 1024,
// the HDR resolve's depth clear) into the same key.
void TestColorToDepth()
{
    std::string why;
    const Key c2 = Key::Color(0, 16, Msaa::X2, ColorFormat::k2_10_10_10_Float), d2 = Key::Depth(1024, 16, Msaa::X2, DepthFormat::D24FS8);
    CHECK(c2.packed == 0xC88000u && d2.packed == 0x688400u && kAd.packed == 0x7082D0u);  // the keys the replay names
    CHECK(ClassifyTransfer(c2, kAd, why) == TransferKind::ColorToDepth);
    CHECK(ClassifyTransfer(d2, kAd, why) == TransferKind::Depth);  // a 2x depth source (usampler2DMS / sampler2DMS)
    CHECK(ClassifyTransfer(kA7, kAd, why) == TransferKind::ColorToDepth && ClassifyTransfer(kBc2, kBd, why) == TransferKind::ColorToDepth);
    CHECK(ClassifyTransfer(c2, Key::Depth(720, 16, Msaa::X4, DepthFormat::D24S8), why) == TransferKind::ColorToDepth);  // TestColorToUnorm24
    // Colour -> 2x depth: 4x -> 2x and 1x -> 2x through MapTransferTexel.
    CHECK(ClassifyTransfer(kA7, d2, why) == TransferKind::ColorToDepth);
    CHECK(ClassifyTransfer(kB7, Key::Depth(0, 16, Msaa::X2, DepthFormat::D24FS8), why) == TransferKind::ColorToDepth);
    CHECK(ClassifyTransfer(Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16), kAd, why) == TransferKind::ColorToDepth);  // TestK16_16
    CHECK(ClassifyTransfer(Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16_Float), kAd, why) == TransferKind::Refused);

    // Which tiles move (X ChangeOwnership): c2 holds 0-1023 (the HDR resolve's span), d2 1024-1439; the 4x clear (360 rows:
    // 720 tiles each) takes 0-719 for the 4x 7e3 colour key from c2, 720-1023 for kAd from c2 and 1024-1439 from d2.
    edram::Ownership o;
    std::vector<edram::Transfer> t;
    CHECK(o.Change(c2, 0, 1024, t, why) && o.Change(d2, 0, 416, t, why) && t.empty());
    RenderTarget clear = Target4x();
    clear.colorFormatRaw = clear.colorFormat = 3;
    const edram::Binding b = edram::Bind(o, BindRegistersFor(clear, true, true), 360);
    using T = edram::Transfer;
    CHECK(b.ok && b.color[0] == kA7 && b.depth == kAd && b.transfers[1] == std::vector<T>{{0, 720, c2}} &&
          b.transfers[0] == std::vector<T>{{720, 1024, c2}, {1024, 1440, d2}});
    CHECK(ClassifyTransfer(c2, kA7, why) == TransferKind::Color);

    // The texel mapping (X :2516-2577; 2x -> 4x :2823-2828, :2843-2849; the column swap :2916-2936 with half = 80 >> 1 =
    // 40 for a 2x source; :2938-2954 the tile offset + 720 - 0): destination (0, 0) of kAd is EDRAM tile 720 = c2's row 45
    // (2x tiles 80x8 pixels: y 360); host sample s -> tile pixel ((0 << 1) | s & 1, 0), host sample ((s >> 1) & 1) ^ 1;
    // swapped x + 40.
    auto is = [](SourceTexel s, uint32_t x, uint32_t y, uint32_t sample) { return s.x == x && s.y == y && s.sample == sample; };
    CHECK(is(MapEdramTexel(0, 0, 0, c2, kAd), 40, 360, 1));
    CHECK(is(MapEdramTexel(0, 0, 3, c2, kAd), 41, 360, 0));
    CHECK(is(MapEdramTexel(39, 7, 3, c2, kAd), 39, 367, 0));   // tile pixel (79, 15 >> 1 = 7) -> 79 - 40
    CHECK(is(MapEdramTexel(40, 0, 0, c2, kAd), 120, 360, 1));  // tile 721 -> c2 tile 721: (80 + 40, 360)
    // EDRAM cross-check (X shaders/edram.xesli:23-59): kAd (0, 0) host 0 = guest 0 -> depth sample (0, 0) of tile 720, stored
    // at column 0 + 40 (the depth half swap); c2 (40, 360) host 1 = guest 0 -> colour sample (40, 0) of tile 720.
    CONTROL(is(MapEdramTexel(0, 0, 0, c2, kAd), 0, 360, 1));  // without the swap
    // The depth -> depth part: d2 (base 1024, 2x) -> kAd (base 720): kAd's local tile 304 (row 19: y 152) is EDRAM tile 1024
    // = d2's tile 0: (0, 152) host 0 -> d2 (0, 0) host ((0 >> 1) & 1) ^ 1 = 1; no swap between two depth keys.
    CHECK(is(MapEdramTexel(0, 152, 0, d2, kAd), 0, 0, 1));
    CHECK(is(MapEdramTexel(0, 152, 3, d2, kAd), 1, 0, 0));

    // The values (X :3385-3586 pack, :3736-3741 bits 8-31, :4120-4124 Depth20e4To32 (remap off here), :4145-4170 stencil):
    // the HDR clear colour 0x08521485 (R = G = B = 7e3 0x085 = 0.259765625, A 0): depth bits 0x085214: exponent 0, mantissa
    // 0x85214, MSB 19 -> shift 1, exponent 0, mantissa 0x0A428 -> float bits 112 << 23 | 0x0A428 << 3 = 0x38052140; stencil
    // 0x85. 2_10_10_10 0x40300801: 0x403008 -> exponent 4, mantissa 0x03008 -> 116 << 23 | 0x18040 = 0x3A018040; stencil 1.
    // 0xF000005A: 0xF00000 -> 1.0; stencil 0x5A. 0 -> 0.
    const float clearColor[4] = {0.259765625f, 0.259765625f, 0.259765625f, 0.0f};
    CHECK(PackTransferColor(clearColor, 3) == 0x08521485u);
    CHECK(FloatBits(ColorWordDepth(0x08521485u)) == 0x38052140u && ColorWordStencil(0x08521485u) == 0x85);
    CHECK(FloatBits(ColorWordDepth(0x40300801u)) == 0x3A018040u && ColorWordStencil(0x40300801u) == 0x01);
    CHECK(ColorWordDepth(0xF000005Au) == 1.0f && ColorWordStencil(0xF000005Au) == 0x5A && ColorWordDepth(0) == 0.0f);
    // Round trip with the depth -> colour word (X :3605-3617 then :3736-4142): exact for 20e4-representable depths.
    CHECK(ColorWordDepth(PackDepthStencilWord(0.75f, 3)) == 0.75f && ColorWordStencil(PackDepthStencilWord(0.75f, 3)) == 3);
    CONTROL(FloatBits(ColorWordDepth(0x08521485u)) == FloatBits(Float20e4To32(0x08521485u)));  // the depth is bits 8-31, not 0-23
}

// Same-aspect transfers between keys of different bases (X vulkan_render_target_cache.cc:2938-2954: source tile =
// destination tile + (destination base - source base), & 2047, for every transfer mode). The case of
// the local capture draw_xaspect1 (not distributed) (draw 25887): the 1x kD24FS8 key 6082D0 at base 720 -> the 4x kD24FS8 key 708400
// at base 1024.
void TestBases()
{
    std::string why;
    const Key d1024 = Key::Depth(1024, 16, Msaa::X4, DepthFormat::D24FS8);
    CHECK(kBd.packed == 0x6082D0u && d1024.packed == 0x708400u);  // the keys the stop names (edram.h Key layout)
    CHECK(ClassifyTransfer(kBd, d1024, why) == TransferKind::Depth);
    CHECK(ClassifyTransfer(Key::Depth(1024, 8, Msaa::X4, DepthFormat::D24FS8), kAd, why) == TransferKind::Depth);  // another pitch (TestDepthPitch)
    CHECK(ClassifyTransfer(kAd, d1024, why) == TransferKind::Depth);  // 4x -> 4x at another base (sampler2DMS)
    CHECK(ClassifyTransfer(kAd, Key::Depth(1024, 16, Msaa::X2, DepthFormat::D24FS8), why) == TransferKind::Depth);  // 4x -> 2x (TestDepth4xTo2x)

    // Which tiles move (X ChangeOwnership, render_target_cache.cc:1539-1699): the front-buffer resolve's depth clear gives
    // tiles 720-1439 to kBd; the 7e3 clear's colour (draw 25343, 512 rows) takes 720-1023 (depth -> colour); a 4x
    // binding of d1024 with 360 rows claims (360 << 1) / 16 * 16 = 720 tiles, 1024-1743: 1024-1439 come from kBd.
    edram::Ownership o;
    std::vector<edram::Transfer> t;
    CHECK(o.Change(kBd, 0, 720, t, why) && t.empty());
    CHECK(o.Change(kA7, 0, 1024, t, why) && t == std::vector<edram::Transfer>{{720, 1024, kBd}});
    t.clear();
    CHECK(o.Change(d1024, 0, 720, t, why) && t == std::vector<edram::Transfer>{{1024, 1440, kBd}});
    // The rectangles of {1024, 1440} in d1024's layout (40x8-pixel tiles, 16 a row, from its base 1024): local tiles
    // 0-415 = rows 0-25 (416 = 26 * 16): one rectangle 640 x 208 at (0, 0).
    edram::Rect r[3];
    CHECK(edram::RangeRectangles(1024, 1440, d1024, r) == 1 && r[0] == edram::Rect{0, 0, 640, 208});

    // The sample mapping (X :2516-2577, :2857-2864, :2823-2828, :2938-2954; no column swap between two depth keys):
    // destination (x, y, s): tile (y / 8) * 16 + x / 40, tile pixel ((x % 40) << 1 | s & 1, (y % 8) << 1 | s >> 1);
    // source tile = destination tile + 1024 - 720 = + 304; source pixel = (tile % 16 * 80, tile / 16 * 16) + tile pixel.
    auto is = [](SourceTexel t, uint32_t x, uint32_t y) { return t.x == x && t.y == y && t.sample == 0; };
    CHECK(is(MapEdramTexel(0, 0, 0, kBd, d1024), 0, 304));      // tile 0 -> 304 = row 19, column 0
    CHECK(is(MapEdramTexel(0, 0, 3, kBd, d1024), 1, 305));
    CHECK(is(MapEdramTexel(39, 7, 3, kBd, d1024), 79, 319));
    CHECK(is(MapEdramTexel(40, 0, 0, kBd, d1024), 80, 304));    // tile 1 -> 305
    CHECK(is(MapEdramTexel(639, 207, 3, kBd, d1024), 1279, 719));  // tile 415 -> 719 = row 44, column 15: (1200 + 79, 704 + 15)
    // Cross-check with X shaders/edram.xesli:23-59 (both depth: the same swap on both sides, so it cancels): destination
    // (0, 0) sample 1 is EDRAM sample (1, 0) of tile 1024; source (1, 304) of base 720 is tile 720 + 19 * 16 = 1024,
    // sample (1, 0).
    CHECK(is(MapEdramTexel(0, 0, 1, kBd, d1024), 1, 304));
    CONTROL(is(MapEdramTexel(0, 0, 0, kBd, d1024), 40, 304));  // a depth -> depth transfer has no column swap
    // Equal bases: MapEdramTexel equals MapTransferTexel (the old mapping), 4x -> 1x and 1x -> 4x.
    bool same = true;
    for (uint32_t y = 0; y < 40; y += 3)
        for (uint32_t x = 0; x < 200; x += 7)
            for (uint32_t s = 0; s < 4; s++)
            {
                const SourceTexel a = MapEdramTexel(x, y, s, kBc2, kAc), b = MapTransferTexel(x, y, s, Msaa::X1, Msaa::X4);
                const SourceTexel c = MapEdramTexel(x, y, 0, kAc, kBc2), e = MapTransferTexel(x, y, 0, Msaa::X4, Msaa::X1);
                same = same && a.x == b.x && a.y == b.y && a.sample == b.sample && c.x == e.x && c.y == e.y && c.sample == e.sample;
            }
    CHECK(same);
    // Colour at different bases, 4x -> 1x: a 4x colour key at base 16 -> a 1x colour key at base 0. Destination (0, 16)
    // is tile 16 -> source tile 0 -> 4x pixel (0, 0), sample (0 & 1) | (16 % 16 & 1) << 1 = 0; (81, 17): tile 17 -> 1,
    // tile pixel (1, 1) -> 4x (40 + 0, 0), sample 3.
    const Key c16 = Key::Color(16, 16, Msaa::X4, ColorFormat::k2_10_10_10);
    CHECK(ClassifyTransfer(c16, kBc2, why) == TransferKind::Color);
    const SourceTexel m0 = MapEdramTexel(0, 16, 0, c16, kBc2), m1 = MapEdramTexel(81, 17, 0, c16, kBc2);
    CHECK(m0.x == 0 && m0.y == 0 && m0.sample == 0 && m1.x == 40 && m1.y == 0 && m1.sample == 3);
}

// FM4's HDR resolve (the local capture draw_bases2 (not distributed), draw 25570): surface 14010500 (pitch 1280, 2x), colour
// 000C0000 (base 0, format 12: storage 3, 7e3), depth 00010400, scissor BR 02000500 (1280x512), RB_COPY_CONTROL 00100340,
// DEST_BASE 1BB71000, DEST_PITCH 02D00500, DEST_INFO 013F0382, RB_DEPTH_CLEAR 0, RB_COLOR_CLEAR(_LO) 08521485. The
// vertices are not traced: the whole 1280x512 surface stands in, as the replay does.
edram::ResolveRegisters HdrResolve()
{
    edram::ResolveRegisters r;
    r.surfaceInfo = 0x14010500, r.colorInfo[0] = 0x000C0000, r.depthInfo = 0x00010400, r.modeControl = 6;
    r.copyControl = 0x00100340, r.copyDestBase = 0x1BB71000, r.copyDestPitch = 0x02D00500, r.copyDestInfo = 0x013F0382;
    r.depthClear = 0, r.colorClear = r.colorClearLo = 0x08521485;
    r.windowScissorBr = 0x02000500, r.screenScissorBr = 0x20002000;
    r.fetch0 = 0x116B4783, r.fetch1 = 0x1000001A;
    const float v[6] = {0.0f, 0.0f, 1280.0f, 0.0f, 0.0f, 512.0f};
    for (int i = 0; i < 6; i++)
        r.vertexWords[i] = GuestFloat(v[i]);
    return r;
}

void TestHdrResolve()
{
    std::string why;
    // The decode by X registers.h:796-833: RB_COPY_CONTROL 00100340: copy_src_select bits 0-2 = 0 (colour 0),
    // copy_sample_select bits 4-6 = 4 (k01, xenos.h CopySampleSelect), color_clear_enable bit 8 = 1, depth_clear_enable
    // bit 9 = 1, copy_command bits 20-21 = 1 (convert). DEST_INFO 013F0382: endian bits 0-2 = 2 (k8in32), array bit 3 = 0,
    // slice 4-6 = 0, format 7-12 = 0x382 >> 7 = 7 (k_2_10_10_10), number 13-15 = 0, exp bias 16-21 = 0x3F = -1, swap bit 24.
    // DEST_PITCH 02D00500: 1280 x 720. ForzaRecomp's "k0123, format 7, bias -1, swap, endian 2" differs in the samples:
    // k01 of a 2x surface (both of its samples), not k0123 of a 4x one.
    const edram::ResolveInfo info = edram::DecodeResolve(HdrResolve());
    CHECK(info.ok && info.copy && info.msaa == Msaa::X2 && info.sampleSelect == edram::SampleSelect::k01 && info.destFormat == 7 &&
          info.destEndian == 2 && info.destExpBias == -1 && info.destSwap && info.clearColor && info.clearDepth);
    CHECK(info.x0 == 0 && info.y0 == 0 && info.x1 == 1280 && info.y1 == 512 && info.destPitch == 1280 && info.destHeight == 720);
    const Key k2 = Key::Color(0, 16, Msaa::X2, ColorFormat::k2_10_10_10_Float);  // 0 | 16 << 11 | 1 << 19 | 3 << 22 = 0xC88000
    CHECK(info.source == k2 && k2.packed == 0xC88000u);
    // Extent (X texture_util.cc:443-459 at pitch 1280): the last tile (1248, 480): macro (39 + 15 * 40) << 9 = 327168
    // -> << 3 = 2617344; + 0x400 << 2 = 2621440.
    CHECK(info.destExtentStart == 0x1BB71000u && info.destExtentLength == 2621440u);
    // The colour clear (X :5383-5390): 08521485 -> R = G = B = 0x085 (7e3 e 1, m 5: 2^-2 * (1 + 5 / 128) = 0.259765625), A 0.
    float c[4];
    CHECK(DecodeColorClear(0x08521485u, 3, c) && c[0] == 0.259765625f && c[1] == 0.259765625f && c[2] == 0.259765625f && c[3] == 0.0f);

    // The full path, not the fast one (X draw_util.cc:1312-1318: bias, two samples, 7e3 never bitwise equivalent).
    CHECK(!ResolveUsesFastPath(info, 3) && ResolveFullConvertible(info, 3, why));
    uint32_t idx[4], n = 0;
    CHECK(ResolveSamples(info, idx, n) && n == 2 && idx[0] == 0 && idx[1] == 1);

    // 2x keys and the 4x <-> 2x transfers (the copy needs the 4x 7e3 target's 1024 tiles in the 2x key).
    CHECK(ClassifyTransfer(kA7, k2, why) == TransferKind::Color && ClassifyTransfer(k2, kA7, why) == TransferKind::Color);
    // 1x <-> 2x (X :2868-2898). EDRAM check as below: 2x pixel (x, y) guest sample g is
    // EDRAM sample (x, 2y + g), 1x pixel (x, y) is (x, y), host 2x sample = guest ^ 1.
    CHECK(ClassifyTransfer(kB7, k2, why) == TransferKind::Color && ClassifyTransfer(k2, kB7, why) == TransferKind::Color);
    {
        auto at = [](SourceTexel t, uint32_t x, uint32_t y, uint32_t s) { return t.x == x && t.y == y && t.sample == s; };
        CHECK(at(MapTransferTexel(5, 3, 0, Msaa::X1, Msaa::X2), 5, 7, 0));  // host 0 = guest 1: EDRAM (5, 2 * 3 + 1) = 1x (5, 7)
        CHECK(at(MapTransferTexel(5, 3, 1, Msaa::X1, Msaa::X2), 5, 6, 0));  // host 1 = guest 0: (5, 6)
        CHECK(at(MapTransferTexel(5, 7, 0, Msaa::X2, Msaa::X1), 5, 3, 0));  // 1x (5, 7) = 2x (5, 3) guest 1 = host 0
        CHECK(at(MapTransferTexel(5, 6, 0, Msaa::X2, Msaa::X1), 5, 3, 1));  // 1x (5, 6) = 2x (5, 3) guest 0 = host 1
        CHECK(at(MapTransferTexel(79, 15, 0, Msaa::X2, Msaa::X1), 79, 7, 0));  // the tile's last 1x row is the 2x tile's last row
        bool round = true;
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t h = 0; h < 2; h++)
            {
                const SourceTexel s = MapTransferTexel(3, y, h, Msaa::X1, Msaa::X2);
                const SourceTexel back = MapTransferTexel(s.x, s.y, s.sample, Msaa::X2, Msaa::X1);
                round = round && s.y < 16 && at(back, 3, y, h);
            }
        CHECK(round);
        CONTROL(at(MapTransferTexel(5, 3, 0, Msaa::X1, Msaa::X2), 5, 6, 0));  // without the native ^ 1 the rows swap
    }
    CHECK(ClassifyTransfer(Key::Depth(1024, 16, Msaa::X4, DepthFormat::D24FS8), Key::Depth(1024, 16, Msaa::X2, DepthFormat::D24FS8), why) ==
          TransferKind::Depth);  // TestDepth4xTo2x
    // 4x -> 2x (X :2786-2802, native 2x: guest g = host ^ 1). EDRAM check by shaders/edram.xesli:23-33: 2x pixel (x, y)
    // guest sample g is EDRAM sample (x, 2y + g); 4x pixel (X, Y) host sample h is (2X + (h & 1), 2Y + (h >> 1)).
    auto is = [](SourceTexel t, uint32_t x, uint32_t y, uint32_t s) { return t.x == x && t.y == y && t.sample == s; };
    CHECK(is(MapTransferTexel(0, 0, 0, Msaa::X4, Msaa::X2), 0, 0, 2));  // host 0 = guest 1: (0, 1) = 4x (0, 0) host 2
    CHECK(is(MapTransferTexel(1, 0, 1, Msaa::X4, Msaa::X2), 0, 0, 1));  // host 1 = guest 0: (1, 0) = 4x (0, 0) host 1
    CHECK(is(MapTransferTexel(5, 3, 0, Msaa::X4, Msaa::X2), 2, 3, 3));  // (5, 7) = 4x (2, 3) host 3
    // 2x -> 4x (X :2823-2828, :2843-2849).
    CHECK(is(MapTransferTexel(0, 0, 0, Msaa::X2, Msaa::X4), 0, 0, 1));  // (0, 0) = 2x (0, 0) guest 0 = host 1
    CHECK(is(MapTransferTexel(0, 0, 3, Msaa::X2, Msaa::X4), 1, 0, 0));  // (1, 1) = 2x (1, 0) guest 1 = host 0
    CHECK(is(MapTransferTexel(2, 3, 2, Msaa::X2, Msaa::X4), 4, 3, 0));  // (4, 7) = 2x (4, 3) guest 1 = host 0
    // Round trip 2x -> 4x -> 2x over a block.
    bool round = true;
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 8; x++)
            for (uint32_t s = 0; s < 2; s++)
            {
                const SourceTexel a = MapTransferTexel(x, y, s, Msaa::X4, Msaa::X2), b = MapTransferTexel(a.x, a.y, a.sample, Msaa::X2, Msaa::X4);
                round = round && b.x == x && b.y == y && b.sample == s;
            }
    CHECK(round);
    // Through the keys (tiles: 2x 80x8, 4x 40x8 pixels): 2x (100, 9) host 0 is tile 17, tile pixel (20, 1) -> 4x tile pixel
    // (10, 1), host 2 -> (40 + 10, 8 + 1).
    CHECK(is(MapEdramTexel(100, 9, 0, kA7, k2), 50, 9, 2));
    CONTROL(is(MapTransferTexel(0, 0, 0, Msaa::X4, Msaa::X2), 0, 0, 0));  // without the native 2x host sample order

    // Host <-> guest samples and the dump layout.
    CHECK(GuestSampleOfHost(Msaa::X4, 0) == 0 && GuestSampleOfHost(Msaa::X4, 1) == 2 && GuestSampleOfHost(Msaa::X4, 2) == 1 &&
          GuestSampleOfHost(Msaa::X4, 3) == 3);
    CHECK(GuestSampleOfHost(Msaa::X2, 0) == 1 && GuestSampleOfHost(Msaa::X2, 1) == 0 && GuestSampleOfHost(Msaa::X1, 0) == 0);
    const DumpLayout l = DumpLayoutFor(Msaa::X2, 1280, 512);
    CHECK(l.samples == 2 && l.columns == 2 && l.rows == 1 && l.Word(0, 0, 0) == 0 && l.Word(0, 0, 1) == 1280 && l.Word(3, 2, 1) == 2 * 2560 + 3 + 1280);
    const DumpLayout l4 = DumpLayoutFor(Msaa::X4, 4, 2);
    CHECK(l4.Word(1, 1, 3) == (1 + 2) * 8 + 1 + 4 && l4.Word(0, 0, 2) == 2 * 8);

    // One pixel by hand (X resolve.xesli:632-704, pixel_formats.xesli:121-125, 328-349): guest sample 0 = 7e3 R 0x200 (e 4:
    // 2.0), G 0x180 (1.0), B 0x100 (0.5), A 3 -> 0xD0060200; sample 1 = R 0x180 (1.0), G 0x180 (1.0), B 0, A 1 -> 0x40060180.
    // Sum (3, 2, 0.5, 4/3) * (2^-1 * 0.5) = (0.75, 0.5, 0.125, 1/3); .bgra -> (0.125, 0.5, 0.75, 1/3); * 1023 + 0.5 truncated:
    // 128, 512, 767; alpha 1.5 -> 1: 128 | 512 << 10 | 767 << 20 | 1 << 30 = 0x6FF80080; k8in32 -> (1 << 31) | 0xF86F.
    const uint32_t px[2] = {0xD0060200u, 0x40060180u};
    CHECK(ResolveConvertPixel(info, 3, px) == 0x6FF80080u);
    const uint32_t sat[2] = {0x3FFu, 0x3FFu};  // R 31.875 twice: 63.75 * 0.25 = 15.9375 -> saturated, moved to blue by the swap
    CHECK(ResolveConvertPixel(info, 3, sat) == 0x3FF00000u);
    const uint32_t den[2] = {0x040u, 0x040u};  // R 64 / 512 = 0.125 (a 7e3 denormal) twice: 0.0625 * 1023 + 0.5 = 64.4375 -> 64
    CHECK(ResolveConvertPixel(info, 3, den) == 64u << 20);
    CONTROL(ResolveConvertPixel(info, 3, px) == 0x6FF80080u + 1);

    // The whole rectangle into guest bytes: pixel (0, 0) at 0 and (2, 3) at 280 (TiledOffset by hand, pitch 1280: see
    // TestResolveToGuest), every other pixel 0 (all samples 0 -> 0, alpha 0).
    std::vector<uint32_t> words(size_t(l.width) * l.columns * l.height * l.rows, 0);
    words[l.Word(0, 0, 0)] = px[0], words[l.Word(0, 0, 1)] = px[1];
    words[l.Word(2, 3, 0)] = sat[0], words[l.Word(2, 3, 1)] = sat[1];
    std::vector<uint8_t> dest(info.destExtentLength, 0xEE);
    CHECK(ResolveConvertToGuest(info, 3, words.data(), dest.data(), uint32_t(dest.size()), why));
    CHECK(dest[0] == 0x6F && dest[1] == 0xF8 && dest[2] == 0x00 && dest[3] == 0x80);  // (1 << 31) | 0xF86F, little-endian
    CHECK(WordAt(dest, 280) == __builtin_bswap32(0x3FF00000u) && WordAt(dest, 4) == 0u);
    CONTROL(ResolveConvertToGuest(info, 3, words.data(), dest.data(), 2617344, why));  // the last tile would land past the end

    // Consistency with the fast path where X says both apply (1x, k0, no bias, equivalent): the full conversion of trace12's
    // front-buffer resolve gives the fast path's bytes.
    const edram::ResolveInfo fb = edram::DecodeResolve(Trace12Resolve(0x00020000, 0x00100300, 0x1B441000, 0x01000380, 0));
    CHECK(ResolveUsesFastPath(fb, 2) && ResolveFullConvertible(fb, 2, why));
    std::vector<uint32_t> texels(size_t(1280) * 720);
    for (size_t i = 0; i < texels.size(); i++)
        texels[i] = uint32_t(i * 2654435761u);
    std::vector<uint8_t> a(fb.destExtentLength, 0), b(fb.destExtentLength, 0);
    CHECK(ResolveToGuest(fb, 2, texels.data(), a.data(), uint32_t(a.size()), why) && ResolveConvertToGuest(fb, 2, texels.data(), b.data(), uint32_t(b.size()), why));
    CHECK(a == b);
}

// Colour format 14 (k_32_FLOAT), the local capture draw_loop1 (not distributed) draw 22725: surface 00800050 (pitch 80: 1 tile a row,
// msaa_samples 0: 1x), RB_COLOR_INFO 000E0000 (base 0, format 14), scissor 32x32. The replay names its one transfer: tiles
// 0-1 from C08000 (the 1280-wide 1x 7e3 key, 16 tiles a row) to 3800800 (the 32_FLOAT key, 1 tile a row).
float BitsFloat(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

edram::ResolveRegisters Float32Resolve(uint32_t destInfo)  // a hypothetical resolve of that surface (none is in the trace)
{
    edram::ResolveRegisters r;
    r.surfaceInfo = 0x00800050, r.colorInfo[0] = 0x000E0000, r.depthInfo = 0x000102D0, r.modeControl = 6;
    r.copyControl = 0x00100000, r.copyDestBase = 0x10000000, r.copyDestPitch = 32 | 32u << 16, r.copyDestInfo = destInfo;
    r.windowScissorBr = 0x00200020, r.screenScissorBr = 0x20002000;
    r.fetch0 = 3, r.fetch1 = 0x1000001A;
    const float v[6] = {0.0f, 0.0f, 32.0f, 0.0f, 0.0f, 32.0f};
    for (int i = 0; i < 6; i++)
        r.vertexWords[i] = GuestFloat(v[i]);
    return r;
}

void TestFloat32()
{
    std::string why;
    const Key s7 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float), f32 = Key::Color(0, 1, Msaa::X1, ColorFormat::k32_Float);
    CHECK(s7.packed == 0xC08000u && f32.packed == 0x3800800u);  // the replay's keys: 0x8000 | 3 << 22; 1 << 11 | 14 << 22
    CHECK(HostColorFormat(ColorFormat::k32_Float) == VK_FORMAT_R32_SFLOAT);  // X vulkan_render_target_cache.cc:1618-1619
    std::string supported;
    CHECK(edram::SupportedKey(f32, supported) && f32.Width() == 80);
    CHECK(ClassifyTransfer(s7, f32, why) == TransferKind::Color && ClassifyTransfer(f32, s7, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(f32, Key::Depth(0, 1, Msaa::X1, DepthFormat::D24FS8), why) == TransferKind::Refused);  // 32_FLOAT -> depth

    // Different pitches (X :2938-2987): the destination's tile index by its own pitch (1), the source tile split by the
    // source's (16). Destination (0, 16) is tile 1 -> the source's tile 1 = column 1, row 0 -> (80, 0); (5, 20) -> (85, 4).
    auto is = [](SourceTexel t, uint32_t x, uint32_t y) { return t.x == x && t.y == y && t.sample == 0; };
    CHECK(is(MapEdramTexel(0, 0, 0, s7, f32), 0, 0));
    CHECK(is(MapEdramTexel(79, 15, 0, s7, f32), 79, 15));
    CHECK(is(MapEdramTexel(0, 16, 0, s7, f32), 80, 0));
    CHECK(is(MapEdramTexel(5, 20, 0, s7, f32), 85, 4));
    CHECK(is(MapEdramTexel(80, 0, 0, f32, s7), 0, 16));  // back: s7's tile 1 is f32's tile 1 = row 1
    CONTROL(is(MapEdramTexel(0, 16, 0, s7, f32), 0, 16));  // splitting by the destination's pitch

    // The word (X :3580-3585 pack 32_FLOAT, :3713-3726 unpack: the raw bits through R32_UINT views): the 7e3 word of
    // R = G = B = 0x085, A 0 (0x08521485) becomes the float with those bits, and back.
    const float c7[4] = {0.259765625f, 0.259765625f, 0.259765625f, 0.0f};
    const uint32_t w = PackTransferColor(c7, 3);
    float f[4];
    UnpackTransferColor(w, 14, f);
    CHECK(w == 0x08521485u && FloatBits(f[0]) == 0x08521485u && f[1] == 0.0f && PackTransferColor(f, 14) == 0x08521485u);
    // A NaN's payload survives the word (the reason X uses R32_UINT views).
    float nan[4] = {BitsFloat(0x7FC01234u), 0, 0, 0};
    CHECK(PackTransferColor(nan, 14) == 0x7FC01234u);
    // Clears (X :5417-5421): the raw word as uint32[0].
    uint32_t u[4] = {9, 9, 9, 9};
    CHECK(DecodeColorClearUint(0x3F800000u, 14, u) && u[0] == 0x3F800000u && u[1] == 0 && u[3] == 0);
    float fc[4];
    CONTROL(DecodeColorClear(0x3F800000u, 14, fc));  // storage 14 never clears through floats
    CONTROL(DecodeColorClearUint(0u, 3, u));

    // Resolves (none traced): into k_32_FLOAT (36) the fast path (X xenos.h:634-635 bitwise equivalent), no red/blue swap for
    // 32_FLOAT (resolve.xesli:769-797), the endian swap: 1.0 (0x3F800000) with k8in32 -> 0x0000803F, bytes 3F 80 00 00.
    const edram::ResolveInfo fast = edram::DecodeResolve(Float32Resolve(2 | 36u << 7 | 1u << 24));
    CHECK(fast.ok && fast.source == f32 && fast.destFormat == 36 && fast.x1 == 32 && fast.y1 == 32 && ResolveUsesFastPath(fast, 14));
    std::vector<uint32_t> texels(32 * 32, 0);
    texels[0] = 0x3F800000u;
    std::vector<uint8_t> dest(fast.destExtentLength, 0xEE);
    CHECK(ResolveToGuest(fast, 14, texels.data(), dest.data(), uint32_t(dest.size()), why));
    CHECK(dest[0] == 0x3F && dest[1] == 0x80 && dest[2] == 0x00 && dest[3] == 0x00);
    // Into 2_10_10_10 (7) with bias -2 and swap (the loop's other resolves, DEST_INFO 013E0382): the full path, unpack (f, 0,
    // 0, 0) (resolve.xesli:313-318), * 2^-2, .bgra, pack: 0.5 -> 0.125 * 1023 + 0.5 = 128.375 -> 128 in blue: 128 << 20.
    const edram::ResolveInfo full = edram::DecodeResolve(Float32Resolve(0x013E0382u));
    const uint32_t half[1] = {0x3F000000u};
    CHECK(full.ok && !ResolveUsesFastPath(full, 14) && ResolveFullConvertible(full, 14, why) && ResolveConvertPixel(full, 14, half) == 128u << 20);
    // Into 36 through the full path (pixel_formats.xesli:247-252: the red float's bits): with bias -1, 1.0 -> 0.5 = 0x3F000000.
    const edram::ResolveInfo biased = edram::DecodeResolve(Float32Resolve(36u << 7 | 0x3Fu << 16));
    const uint32_t one[1] = {0x3F800000u};
    CHECK(biased.ok && !ResolveUsesFastPath(biased, 14) && ResolveConvertPixel(biased, 14, one) == 0x3F000000u);
}

// Colour -> kD24S8 depth (X kColorToDepth with the unorm24 conversion), the local capture draw_pred2 (not distributed) draw 33383: surface
// 0A020280 (640, 4x), RB_DEPTH_INFO 00000000 (base 0, kD24S8), RB_MODECONTROL 5 (depth only), depthctl 00008777: tiles 0-95 of
// the 1x 7e3 key C08000 into the 4x kD24S8 key 308000.
void TestColorToUnorm24()
{
    std::string why;
    const Key s7 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float), d24 = Key::Depth(0, 16, Msaa::X4, DepthFormat::D24S8);
    CHECK(s7.packed == 0xC08000u && d24.packed == 0x308000u);  // 0 | 16 << 11 | 2 << 19 | 1 << 21 | 0 << 22
    CHECK(ClassifyTransfer(s7, d24, why) == TransferKind::ColorToDepth);
    CHECK(HostDepthFormat(DepthFormat::D24S8, false) == VK_FORMAT_D32_SFLOAT_S8_UINT);
    // Every distinct transfer pair of the draw_pred2 replay (test_edram <cp.txt> "pairs:", 16 of them) is one the unit executes.
    const uint32_t pairs[16][2] = {{0x908000, 0x808000}, {0x6082D0, 0x7082D0}, {0x808000, 0x908000}, {0x908000, 0x008000}, {0x008000, 0x908000},
                                   {0x6082D0, 0x708400}, {0x808000, 0xD08000}, {0x6082D0, 0xD08000}, {0xD08000, 0xC88000}, {0xC88000, 0x7082D0},
                                   {0x688400, 0x7082D0}, {0xC88000, 0xD08000}, {0xD08000, 0xC08000}, {0xC08000, 0x3800800}, {0x3800800, 0xC08000},
                                   {0xC08000, 0x308000}};
    bool all = true;
    for (const auto& p : pairs)
        all = all && ClassifyTransfer(Key{p[0]}, Key{p[1]}, why) != TransferKind::Refused;
    CHECK(all);

    // The texel mapping (1x colour -> 4x depth, X :2823-2828, :2857-2864, the column swap :2916-2936 with half 40 for a 1x source):
    // destination (0, 0) host s -> tile pixel (s & 1, s >> 1) -> x + 40.
    auto is = [](SourceTexel t, uint32_t x, uint32_t y) { return t.x == x && t.y == y && t.sample == 0; };
    CHECK(is(MapEdramTexel(0, 0, 0, s7, d24), 40, 0));
    CHECK(is(MapEdramTexel(0, 0, 3, s7, d24), 41, 1));
    CHECK(is(MapEdramTexel(20, 0, 0, s7, d24), 0, 0));
    CHECK(is(MapEdramTexel(40, 8, 0, s7, d24), 120, 16));  // tile 17 -> (80 + 40, 16)

    // The depth (X :4102-4119: float(n + (n >> 23)) * 2^-24, n = word >> 8) and stencil (word & 0xFF):
    // 0x08521485 (the 7e3 HDR clear colour): n = 0x085214 = 545300 -> 545300 / 2^24 = 0.0325024127960205078125 (float bits
    // 0x3D052140), stencil 0x85; 0xFFFFFF00: n = 0xFFFFFF -> (0xFFFFFF + 1) / 2^24 = 1.0, stencil 0; 0xC0000000: n = 0xC00000 ->
    // (0xC00000 + 1) / 2^24 = 12582913 / 16777216; 0 -> 0.
    CHECK(FloatBits(ColorWordDepth(0x08521485u, DepthFormat::D24S8)) == 0x3D052140u && ColorWordStencil(0x08521485u) == 0x85);
    CHECK(ColorWordDepth(0xFFFFFF00u, DepthFormat::D24S8) == 1.0f && ColorWordStencil(0xFFFFFF00u) == 0);
    CHECK(ColorWordDepth(0xC0000000u, DepthFormat::D24S8) == 12582913.0f / 16777216.0f);
    CHECK(ColorWordDepth(0u, DepthFormat::D24S8) == 0.0f);
    // Without X's (n >> 23) term 0xFFFFFF would map to 16777215 / 2^24 = 0.99999994 (float bits 0x3F7FFFFF), not 1.0. (The
    // n / 0xFFFFFF form X calls incorrect rounds to the same float32 at 0xC00000, so it is no control here.)
    CONTROL(FloatBits(ColorWordDepth(0xFFFFFF00u, DepthFormat::D24S8)) == 0x3F7FFFFFu);
    CONTROL(FloatBits(ColorWordDepth(0x08521485u, DepthFormat::D24S8)) == FloatBits(ColorWordDepth(0x08521485u)));  // not the 20e4 path
    // Consistent with the clear decode of the same format (X :5352-5355 UNorm24To32): the same word, the same depth.
    float d = 0;
    uint32_t s = 0;
    DecodeDepthClear(0xC0000005u, DepthFormat::D24S8, d, s);
    CHECK(d == ColorWordDepth(0xC0000005u, DepthFormat::D24S8) && s == 5);
}

// Depth -> colour from a multisampled kD24S8 source (X kDepthToColor), the local capture draw_pred3 (not distributed) draw 33440:
// surface 14000500 (1280, 1x), RB_COLOR_INFO 00030000 (7e3 at 0): tiles 0-95 of the 4x kD24S8 key 308000 (draw 33439's
// depth-only clear) into the 1x 7e3 key C08000.
void TestDepthToColorMs()
{
    std::string why;
    const Key d24 = Key::Depth(0, 16, Msaa::X4, DepthFormat::D24S8), c7 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float);
    CHECK(d24.packed == 0x308000u && c7.packed == 0xC08000u);
    CHECK(ClassifyTransfer(d24, c7, why) == TransferKind::DepthToColor);
    CHECK(ClassifyTransfer(Key::Depth(1024, 16, Msaa::X2, DepthFormat::D24FS8), kA7, why) == TransferKind::DepthToColor);  // 2x -> 4x
    CHECK(ClassifyTransfer(kAd, Key::Color(0, 16, Msaa::X2, ColorFormat::k2_10_10_10_Float), why) == TransferKind::DepthToColor);  // 4x -> 2x
    // FM4 Recomp: 1x depth -> 2x colour is accepted (MapTransferTexel 1x -> 2x), its texel mapping as the
    // colour pairs': 2x destination (5, 3) host sample 0 = guest 1 -> 1x source (5, 7) before the colour <-> depth column swap.
    CHECK(ClassifyTransfer(kBd, Key::Color(0, 16, Msaa::X2, ColorFormat::k2_10_10_10_Float), why) == TransferKind::DepthToColor);
    CHECK(MapTransferTexel(5, 3, 0, Msaa::X1, Msaa::X2).y == 7);
    // Every distinct transfer pair of the draw_pred3 replay (test_edram <cp.txt> "pairs:", 17 of them) is one the unit executes.
    const uint32_t pairs[17][2] = {{0x908000, 0x808000}, {0x6082D0, 0x7082D0}, {0x808000, 0x908000}, {0x908000, 0x008000}, {0x008000, 0x908000},
                                   {0x6082D0, 0x708400}, {0x808000, 0xD08000}, {0x6082D0, 0xD08000}, {0xD08000, 0xC88000}, {0xC88000, 0x7082D0},
                                   {0x688400, 0x7082D0}, {0xC88000, 0xD08000}, {0xD08000, 0xC08000}, {0xC08000, 0x3800800}, {0x3800800, 0xC08000},
                                   {0xC08000, 0x308000}, {0x308000, 0xC08000}};
    bool all = true;
    for (const auto& p : pairs)
        all = all && ClassifyTransfer(Key{p[0]}, Key{p[1]}, why) != TransferKind::Refused;
    CHECK(all);

    // The texel mapping by hand (X :2804-2818 4x -> 1x: sample = (x & 1) | (y & 1) << 1, pixel = tile pixel >> 1; :2916-2936
    // the column swap with half = 80 >> (1 + 1) = 20 for a 4x source; :2938-2954 source tile = destination tile, the
    // source's 40x8 pixel tiles). Destination 1x tiles are 80x16.
    auto is = [](SourceTexel t, uint32_t x, uint32_t y, uint32_t s) { return t.x == x && t.y == y && t.sample == s; };
    CHECK(is(MapEdramTexel(0, 0, 0, d24, c7), 20, 0, 0));
    CHECK(is(MapEdramTexel(1, 0, 0, d24, c7), 20, 0, 1));
    CHECK(is(MapEdramTexel(0, 1, 0, d24, c7), 20, 0, 2));
    CHECK(is(MapEdramTexel(3, 1, 0, d24, c7), 21, 0, 3));
    CHECK(is(MapEdramTexel(40, 0, 0, d24, c7), 0, 0, 0));      // tile pixel 20 -> 0
    CHECK(is(MapEdramTexel(79, 15, 0, d24, c7), 19, 7, 3));    // (39, 7) -> 19
    CHECK(is(MapEdramTexel(80, 0, 0, d24, c7), 60, 0, 0));     // tile 1: 40 + 20
    CHECK(is(MapEdramTexel(0, 16, 0, d24, c7), 20, 8, 0));     // tile 16 = source row 1
    CHECK(is(MapEdramTexel(159, 95, 0, d24, c7), 59, 47, 3));  // tile 81 (row 5, column 1): (40 + 19, 40 + 7)
    // 4x -> 4x: the same pixel and sample, the swap by 20.
    CHECK(is(MapEdramTexel(5, 3, 2, d24, Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float)), 25, 3, 2));
    CONTROL(is(MapEdramTexel(0, 0, 0, d24, c7), 40, 0, 0));  // the 1x source's half (40) is not a 4x source's

    // kD24S8 packing (X :3593-3604: uint(RoundEven(depth * float(0xFFFFFF))), the product a float32):
    // 1.0 -> 16777215 = 0xFFFFFF; 0 -> 0;
    // 545300 / 2^24 (0.0325024127960205078125, the unorm24 depth of the 7e3 clear word 08521485): the product is exactly
    // 545300 - 545300 / 2^24 = 545299.96749..., float32 spacing 2^-4 in [2^19, 2^20): 545299.9375 (0.0300 away) not 545300
    // (0.0325 away); RoundEven -> 545300 = 0x085214;
    // 0.5 -> 8388607.5 (exact; spacing 0.5) -> the even neighbour 8388608 = 0x800000 (truncation would give 0x7FFFFF);
    // 0x600001 / 2^24: 6291457 - 0.37500006 = 6291456.62499994 -> float32 (spacing 0.5) 6291456.5 -> RoundEven 6291456 =
    // 0x600000 (round half up would give 0x600001): X's conversion is not the exact inverse of its unorm24 decode;
    // 12582913 / 2^24 (the decode of 0xC00000): 12582913 - 0.75000006 = 12582912.24999994 -> 12582912 (spacing 1) -> 0xC00000.
    CHECK(Depth32ToUnorm24(1.0f) == 0xFFFFFFu && Depth32ToUnorm24(0.0f) == 0u);
    CHECK(Depth32ToUnorm24(545300.0f / 16777216.0f) == 0x085214u);
    CHECK(Depth32ToUnorm24(0.5f) == 0x800000u);
    CHECK(Depth32ToUnorm24(6291457.0f / 16777216.0f) == 0x600000u);
    CHECK(Depth32ToUnorm24(12582913.0f / 16777216.0f) == 0xC00000u);
    // FM4 Recomp's clamp (edram_vk_plan.h): out of range and NaN.
    CHECK(Depth32ToUnorm24(-1.0f) == 0u && Depth32ToUnorm24(2.0f) == 0xFFFFFFu && Depth32ToUnorm24(std::nanf("")) == 0u);
    CONTROL(Depth32ToUnorm24(0.5f) == 0x7FFFFFu);
    // The word (X :3614-3617): stencil | depth << 8; the round trip of the 7e3 clear word through colour -> depth -> colour.
    CHECK(PackDepthStencilWord(1.0f, 0x5A, DepthFormat::D24S8) == 0xFFFFFF5Au);
    CHECK(PackDepthStencilWord(ColorWordDepth(0x08521485u, DepthFormat::D24S8), ColorWordStencil(0x08521485u), DepthFormat::D24S8) == 0x08521485u);
    CHECK(PackDepthStencilWord(1.0f, 0x5A) == 0xF000005Au);  // the default stays kD24FS8
    CONTROL(PackDepthStencilWord(1.0f, 0x5A, DepthFormat::D24S8) == PackDepthStencilWord(1.0f, 0x5A));
}

// Depth -> colour between pitches, the local capture draw_pred4 (not distributed) draw 34192: surface 14020500 (pitch 1280, 4x: 32
// tiles a row), RB_COLOR_INFO 00020000 (2_10_10_10 at 0), scissor 1280x256: tiles 0-191 from the 1x 7e3 key C08000, 192-719
// from the 4x 7e3 key D08000 and 720-1023 from the 4x kD24FS8 key 7082D0 (all 16 tiles a row) into the 4x key 910000.
void TestDepthToColorPitch()
{
    std::string why;
    const Key d = Key::Depth(720, 16, Msaa::X4, DepthFormat::D24FS8), c = Key::Color(0, 32, Msaa::X4, ColorFormat::k2_10_10_10);
    CHECK(d.packed == 0x7082D0u && c.packed == 0x910000u);  // 720 | 16 << 11 | 2 << 19 | 1 << 21 | 1 << 22; 0 | 32 << 11 | 2 << 19 | 2 << 22
    CHECK(ClassifyTransfer(d, c, why) == TransferKind::DepthToColor);
    // Into a depth key too (TestDepthPitch).
    CHECK(ClassifyTransfer(Key::Depth(720, 32, Msaa::X4, DepthFormat::D24FS8), d, why) == TransferKind::Depth);
    CHECK(ClassifyTransfer(c, d, why) == TransferKind::ColorToDepth);
    // Every distinct transfer pair of the draw_pred4 replay (test_edram <cp.txt> "pairs:", 20 of them) is one the unit executes.
    const uint32_t pairs[20][2] = {{0x908000, 0x808000}, {0x6082D0, 0x7082D0}, {0x808000, 0x908000}, {0x908000, 0x008000}, {0x008000, 0x908000},
                                   {0x6082D0, 0x708400}, {0x808000, 0xD08000}, {0x6082D0, 0xD08000}, {0xD08000, 0xC88000}, {0xC88000, 0x7082D0},
                                   {0x688400, 0x7082D0}, {0xC88000, 0xD08000}, {0xD08000, 0xC08000}, {0xC08000, 0x3800800}, {0x3800800, 0xC08000},
                                   {0xC08000, 0x308000}, {0x308000, 0xC08000}, {0xC08000, 0x910000}, {0xD08000, 0x910000}, {0x7082D0, 0x910000}};
    bool all = true;
    for (const auto& p : pairs)
        all = all && ClassifyTransfer(Key{p[0]}, Key{p[1]}, why) != TransferKind::Refused;
    CHECK(all);

    // By hand (X :2516-2577 the destination tile = (y / 8) * 32 + x / 40 by the destination's pitch; 4x -> 4x the same tile
    // pixel and sample; :2916-2936 the swap by 20 (4x source); :2938-2954 source tile = destination tile + 0 - 720, & 2047;
    // :2956-2987 split by the source's pitch 16 into 40x8 tiles):
    // (640, 176): tile 22 * 32 + 16 = 720 -> source tile 0 -> (0 + 20, 0);
    // (1240, 176): tile 735 -> 15 -> (15 * 40 + 20, 0) = (620, 0);
    // (0, 184): tile 23 * 32 = 736 -> 16 = source row 1, column 0 -> (20, 8);
    // (1279, 255) sample 3: tile 31 * 32 + 31 = 1023 -> 303 = row 18, column 15; tile pixel (39, 7) -> 19 -> (600 + 19, 144 + 7).
    auto is = [](SourceTexel t, uint32_t x, uint32_t y, uint32_t s) { return t.x == x && t.y == y && t.sample == s; };
    CHECK(is(MapEdramTexel(640, 176, 2, d, c), 20, 0, 2));
    CHECK(is(MapEdramTexel(1240, 176, 0, d, c), 620, 0, 0));
    CHECK(is(MapEdramTexel(0, 184, 1, d, c), 20, 8, 1));
    CHECK(is(MapEdramTexel(1279, 255, 3, d, c), 619, 151, 3));
    CONTROL(is(MapEdramTexel(0, 184, 1, d, c), 660, 0, 1));  // the source tile split by the destination's pitch (32): (16 * 40 + 20, 0)
    // The two colour sources of the same draw (no swap): D08000 (4x, 16 a row): (0, 48) = tile 6 * 32 = 192 -> source row 12
    // -> (0, 96); C08000 (1x, 16 a row, 80x16 tiles): (0, 8) = tile 32 -> source row 2 -> (0, 32); sample 3 -> tile pixel (1, 1).
    const Key d7 = Key::Color(0, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), c7 = Key::Color(0, 16, Msaa::X1, ColorFormat::k2_10_10_10_Float);
    CHECK(d7.packed == 0xD08000u && c7.packed == 0xC08000u);
    CHECK(is(MapEdramTexel(0, 48, 2, d7, c), 0, 96, 2));
    CHECK(is(MapEdramTexel(0, 8, 0, c7, c), 0, 32, 0) && is(MapEdramTexel(0, 8, 3, c7, c), 1, 33, 0));
}

// Depth -> depth between pitches, the local capture window1 (not distributed) draw 32631: surface 14020500 (pitch 1280, 4x: 32 tiles a
// row), RB_DEPTH_INFO 00010400 (kD24FS8 at 1024), scissor 1280x256: EDRAM tiles 1024-1439 from the 4x key 7082D0 (base 720)
// and 1440-2047 from the 2x key 688400 (base 1024), both 16 tiles a row, into the 4x key 710400 (base 1024, 32 a row).
void TestDepthPitch()
{
    std::string why;
    const Key a = Key::Depth(720, 16, Msaa::X4, DepthFormat::D24FS8), b = Key::Depth(1024, 16, Msaa::X2, DepthFormat::D24FS8),
              d = Key::Depth(1024, 32, Msaa::X4, DepthFormat::D24FS8);
    CHECK(a.packed == 0x7082D0u && b.packed == 0x688400u && d.packed == 0x710400u);  // 1024 | 32 << 11 | 2 << 19 | 1 << 21 | 1 << 22
    CHECK(ClassifyTransfer(a, d, why) == TransferKind::Depth && ClassifyTransfer(b, d, why) == TransferKind::Depth);
    // Every distinct transfer pair of the window1 replay (test_edram <cp.txt> "pairs:", 22 of them) is one the unit executes.
    const uint32_t pairs[22][2] = {{0x908000, 0x808000}, {0x6082D0, 0x7082D0}, {0x808000, 0x908000}, {0x908000, 0x008000}, {0x008000, 0x908000},
                                   {0x6082D0, 0x708400}, {0x808000, 0xD08000}, {0x6082D0, 0xD08000}, {0xD08000, 0xC88000}, {0xC88000, 0x7082D0},
                                   {0x688400, 0x7082D0}, {0xC88000, 0xD08000}, {0xD08000, 0xC08000}, {0xC08000, 0x3800800}, {0x3800800, 0xC08000},
                                   {0xC08000, 0x308000}, {0x308000, 0xC08000}, {0xC08000, 0x910000}, {0xD08000, 0x910000}, {0x7082D0, 0x910000},
                                   {0x7082D0, 0x710400}, {0x688400, 0x710400}};
    bool all = true;
    for (const auto& p : pairs)
        all = all && ClassifyTransfer(Key{p[0]}, Key{p[1]}, why) != TransferKind::Refused;
    CHECK(all);

    // 7082D0 -> 710400 by hand (X :2516-2577 destination tile = (y / 8) * 32 + x / 40; 4x -> 4x the same tile pixel and
    // sample; no column swap (depth -> depth, :2916); :2938-2954 source tile = destination tile + 1024 - 720 = + 304;
    // :2956-2987 split by the source's pitch 16 into 40x8 tiles):
    // (0, 0): tile 0 -> 304 = row 19, column 0 -> (0, 152); (600, 0): 15 -> 319 = row 19, column 15 -> (600, 152);
    // (640, 0): 16 -> 320 = row 20, column 0 -> (0, 160); (1279, 7) sample 3: 31 -> 335 = row 20, column 15 -> (639, 167);
    // (1240, 96): tile 12 * 32 + 31 = 415 (the last one from 7082D0) -> 719 = row 44, column 15 -> (600, 352).
    auto is = [](SourceTexel t, uint32_t x, uint32_t y, uint32_t s) { return t.x == x && t.y == y && t.sample == s; };
    CHECK(is(MapEdramTexel(0, 0, 1, a, d), 0, 152, 1));
    CHECK(is(MapEdramTexel(600, 0, 0, a, d), 600, 152, 0));
    CHECK(is(MapEdramTexel(640, 0, 2, a, d), 0, 160, 2));
    CHECK(is(MapEdramTexel(1279, 7, 3, a, d), 639, 167, 3));
    CHECK(is(MapEdramTexel(1240, 96, 0, a, d), 600, 352, 0));
    CONTROL(is(MapEdramTexel(640, 0, 2, a, d), 0, 80, 2));    // tile 320 split by the destination's pitch (32): row 10
    CONTROL(is(MapEdramTexel(0, 0, 1, a, d), 20, 152, 1));    // no column swap between two depth keys
    // 688400 (2x, 80x8 pixel tiles) -> 710400, the same base (offset 0): 2x -> 4x (X :2823-2828 x = tile pixel x << 1 |
    // sample bit 0; :2843-2849 source sample = (sample >> 1) ^ 1, native 2x):
    // (0, 104): tile 13 * 32 = 416 -> row 26, column 0 -> (0, 208); sample 0 -> x 0, sample 1; sample 3 -> x 1, sample 0;
    // (40, 104) sample 2: tile 417 -> column 1 -> (80, 208), sample 0;
    // (1279, 255) sample 1: tile 1023 -> row 63, column 15 -> (1200, 504) + (39 << 1 | 1, 7) = (1279, 511), sample 1.
    CHECK(is(MapEdramTexel(0, 104, 0, b, d), 0, 208, 1));
    CHECK(is(MapEdramTexel(0, 104, 3, b, d), 1, 208, 0));
    CHECK(is(MapEdramTexel(40, 104, 2, b, d), 80, 208, 0));
    CHECK(is(MapEdramTexel(1279, 255, 1, b, d), 1279, 511, 1));
}

// Depth resolves (X GetResolveInfo's depth path, GetCopyShader's fast bit copy; edram_vk_plan.h ResolveDepthToGuest).
void TestDepthResolveVk()
{
    // The EDRAM word of a depth sample (X vulkan_render_target_cache.cc:3591-3617; kResolveDumpDepth's XeDepthStencilWord):
    // kD24FS8 0.5 = 0x3F000000 - (112 << 23) = 0x07000000 >> 3 = 0xE00000 -> 0xE0000000 | 0x12; kD24S8 0.5 -> RoundEven(8388607.5)
    // = 0x800000 -> (0x800000 << 8) | 0x12; 1.0 -> 0xF00000 (20e4) / 0xFFFFFF (unorm24).
    CHECK(PackDepthStencilWord(0.5f, 0x12, DepthFormat::D24FS8) == 0xE0000012u && PackDepthStencilWord(0.5f, 0x12, DepthFormat::D24S8) == (0x800000u << 8 | 0x12u));
    CHECK(PackDepthStencilWord(1.0f, 0, DepthFormat::D24FS8) == 0xF0000000u && PackDepthStencilWord(1.0f, 0, DepthFormat::D24S8) == 0xFFFFFF00u);

    // An 8x8 4x kD24FS8 depth resolve, sample select k2, endian 0 then 2, into a 32-texel pitch (offsets by hand as in
    // TestResolveFast: (0,0) 0, (1,0) 4, (0,1) 16, all in macro tile 0).
    edram::ResolveInfo info;
    info.ok = info.copy = info.isDepth = true;
    info.msaa = Msaa::X4;
    info.source = Key::Depth(0, 16, Msaa::X4, DepthFormat::D24FS8);
    info.sampleSelect = edram::SampleSelect::k2;
    info.destFormat = 23, info.destPitch = 32, info.destHeight = 32;
    info.x1 = 8, info.y1 = 8;
    info.destExtentLength = 4096;
    std::string why;
    CHECK(ResolveDepthConvertible(info, why));
    const DumpLayout l = DumpLayoutFor(Msaa::X4, 8, 8);
    std::vector<uint32_t> words(size_t(l.width) * l.height * l.samples);
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++)
            for (uint32_t g = 0; g < 4; g++)
                words[l.Word(x, y, g)] = g << 28 | y << 8 | x;
    std::vector<uint8_t> dest(4096, 0xEE);
    CHECK(ResolveDepthToGuest(info, words.data(), dest.data(), uint32_t(dest.size()), why));
    CHECK(WordAt(dest, 0) == 0x20000000u && WordAt(dest, 4) == 0x20000001u && WordAt(dest, 16) == 0x20000100u);  // guest sample 2
    CONTROL(WordAt(dest, 0) == 0x00000000u);  // sample 0
    info.destEndian = 2;  // k8in32: the bytes reversed (X xenos.h GpuSwap)
    CHECK(ResolveDepthToGuest(info, words.data(), dest.data(), uint32_t(dest.size()), why));
    CHECK(WordAt(dest, 0) == 0x00000020u && WordAt(dest, 4) == 0x01000020u && dest[0] == 0x20 && dest[3] == 0x00);
    // Refused: two samples (never left by DecodeResolve for depth), a destination format other than k_24_8_FLOAT for kD24FS8,
    // a colour resolve; nothing written.
    edram::ResolveInfo bad = info;
    bad.sampleSelect = edram::SampleSelect::k01;
    CHECK(!ResolveDepthConvertible(bad, why) && why.find("one sample") != std::string::npos);
    bad = info, bad.destFormat = 22;
    CHECK(!ResolveDepthConvertible(bad, why));
    bad = info, bad.isDepth = false;
    std::vector<uint8_t> untouched(4096, 0xEE);
    CHECK(!ResolveDepthToGuest(bad, words.data(), untouched.data(), uint32_t(untouched.size()), why) && untouched[0] == 0xEE);
    // kD24S8 -> 22.
    bad = info, bad.source = Key::Depth(0, 16, Msaa::X4, DepthFormat::D24S8), bad.destFormat = 22;
    CHECK(ResolveDepthConvertible(bad, why));
    // The colour paths keep refusing a depth source.
    CHECK(!ResolveFullConvertible(info, 1, why) && !ResolveUsesFastPath(info, 1));
}

}  // namespace

// The depth transfer 4x -> 2x (FM4's: the 4x kD24FS8 key 708400 (base 1024, 16
// tiles a row) -> the 2x kD24FS8 key 688400 (same base and pitch)). Expected values from the EDRAM sample layout (X
// shaders/edram.xesli:23-33: 1x pixel (x, r) is EDRAM sample (x, r) of its tile; 4x pixel (X, Y) host sample h is
// (2X + (h & 1), 2Y + (h >> 1)); 2x pixel (x, y) guest sample g = host h ^ 1 (X :2599-2601) is (x, 2y + g)), not from
// MapTransferTexel; the passes (stencil cleared, depth copied, stencil set bit by bit: X :4975-5011, kStencilBit
// :4156-4180) modelled as the pipelines state them (edram_vk.cpp: compare ALWAYS, REPLACE with reference 0xFF under write
// mask 1 << bit, a fragment whose source bit is clear discarded).
void TestDepth4xTo2x()
{
    std::string why;
    const Key k1 = Key::Depth(1024, 16, Msaa::X1, DepthFormat::D24FS8), k4 = Key::Depth(1024, 16, Msaa::X4, DepthFormat::D24FS8),
              k2 = Key::Depth(1024, 16, Msaa::X2, DepthFormat::D24FS8);
    CHECK(k4.packed == 0x708400u && k2.packed == 0x688400u);  // the keys the stop names
    CHECK(ClassifyTransfer(k4, k2, why) == TransferKind::Depth && ClassifyTransfer(kAd, k2, why) == TransferKind::Depth);
    // Colour -> 2x depth (TestColorToDepth) and 1x depth -> 2x depth are accepted; 2x -> 2x stays refused.
    CHECK(ClassifyTransfer(kA7, k2, why) == TransferKind::ColorToDepth);
    CHECK(ClassifyTransfer(k1, k2, why) == TransferKind::Depth);
    CHECK(ClassifyTransfer(Key::Depth(0, 16, Msaa::X2, DepthFormat::D24FS8), k2, why) == TransferKind::Refused && why.find("2x depth") != std::string::npos);
    CHECK(ClassifyTransfer(Key::Depth(1024, 16, Msaa::X4, DepthFormat::D24S8), k2, why) == TransferKind::Depth);  // FM4 Recomp: 4x kD24S8 -> 2x kD24FS8, format change
    CONTROL(ClassifyTransfer(Key::Depth(0, 16, Msaa::X2, DepthFormat::D24FS8), k2, why) != TransferKind::Refused);

    // The ownership model: the 4x key owns 1024-1439 (26 rows of 16 tiles); binding the 2x key (1280 pixels, 208 rows)
    // claims (208 << 1) / 16 * 16 = 416 tiles from 1024: one transfer {1024, 1440, k4}, classified Depth.
    edram::Ownership o;
    std::vector<edram::Transfer> t;
    CHECK(o.Change(k4, 0, 416, t, why) && t.empty());
    RenderTarget rt;
    rt.pitch = 1280, rt.msaa = 1, rt.samples = 2, rt.hizPitch = 1280, rt.depthBase = 1024, rt.depthFormat = 1, rt.targetHeight = 208;
    const edram::Binding b = edram::Bind(o, BindRegistersFor(rt, false, true), 208);
    CHECK(b.ok && b.depth == k2 && b.transfers[0] == std::vector<edram::Transfer>{{1024, 1440, k4}} && AllClassified(b.transfers[0], k2, TransferKind::Depth));

    // Sample mapping over the whole transfer (1280 x 208 2x pixels, both samples): every destination sample reads the 4x
    // sample at the same EDRAM sample, and each 4x sample is read exactly once.
    auto edram4x = [](SourceTexel s) { return std::pair<uint32_t, uint32_t>{2 * s.x + (s.sample & 1), 2 * s.y + (s.sample >> 1)}; };
    std::vector<uint8_t> hit(size_t(640) * 208 * 4, 0);
    bool same = true, inside = true;
    for (uint32_t y = 0; y < 208; y++)
        for (uint32_t x = 0; x < 1280; x++)
            for (uint32_t h = 0; h < 2; h++)
            {
                const SourceTexel s = MapEdramTexel(x, y, h, k4, k2);
                inside = inside && s.x < 640 && s.y < 208 && s.sample < 4;
                if (!inside)
                    break;
                same = same && edram4x(s) == std::pair<uint32_t, uint32_t>{x, 2 * y + (h ^ 1)};
                hit[(size_t(s.y) * 640 + s.x) * 4 + s.sample]++;
            }
    bool once = inside;
    for (uint8_t n : hit)
        once = once && n == 1;
    CHECK(inside && same && once);
    // Hand-computed points: 2x (0, 0) host 0 = guest 1 -> EDRAM (0, 1) = 4x (0, 0) host 2; 2x (85, 3) host 1 = guest 0 ->
    // EDRAM (85, 6) = 4x (42, 3) host 1; 2x (1279, 207) host 0 = guest 1 -> EDRAM (1279, 415) = 4x (639, 207) host 3.
    auto is = [](SourceTexel s, uint32_t x, uint32_t y, uint32_t sample) { return s.x == x && s.y == y && s.sample == sample; };
    CHECK(is(MapEdramTexel(0, 0, 0, k4, k2), 0, 0, 2) && is(MapEdramTexel(85, 3, 1, k4, k2), 42, 3, 1) && is(MapEdramTexel(1279, 207, 0, k4, k2), 639, 207, 3));
    CONTROL(is(MapEdramTexel(0, 0, 0, k4, k2), 0, 0, 0));  // without the native 2x host sample order

    // The isolated GPU check's data path (1x -> 4x -> 2x, edram_vk.cpp RecordDepthSelfCheck) composes the same mappings:
    // its expectations (from the layout) agree with MapEdramTexel's composition for every sample of its two tile rows.
    bool composed = true, exact = true;
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 1280; x++)
            for (uint32_t g = 0; g < 2; g++)
            {
                const SourceTexel s4 = MapEdramTexel(x, y, g ^ 1, k4, k2), s1 = MapEdramTexel(s4.x, s4.y, s4.sample, k1, k4);
                composed = composed && s1.sample == 0 && DepthSelfCheckWord(s1.x, s1.y) == DepthSelfCheckExpected2x(x, y, g);
            }
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 640; x++)
            for (uint32_t g = 0; g < 4; g++)
            {
                const SourceTexel s1 = MapEdramTexel(x, y, ((g & 1) << 1) | (g >> 1), k1, k4);  // host of guest g
                composed = composed && DepthSelfCheckWord(s1.x, s1.y) == DepthSelfCheckExpected4x(x, y, g);
            }
    for (uint32_t r = 0; r < 32; r++)
        for (uint32_t x = 0; x < 1280; x++)
        {
            const float d = DepthSelfCheckDepth(x, r);
            exact = exact && d > 0.0f && d < 0.5f && Float20e4To32(Depth32To20e4(d)) == d;
        }
    CHECK(composed && exact);
    CHECK(DepthSelfCheckExpected2x(0, 0, 1) == DepthSelfCheckWord(0, 1) && DepthSelfCheckExpected4x(0, 0, 2) == DepthSelfCheckWord(1, 0));
    CONTROL(DepthSelfCheckExpected2x(0, 0, 0) == DepthSelfCheckExpected2x(0, 0, 1));  // the two samples hold different words

    // Depth and stencil preserved by the passes: stencil cleared to 0, depth copied (compare ALWAYS, write on), then for
    // bits 0-7 REPLACE with reference 0xFF under write mask 1 << bit where the source bit is set.
    auto passes = [](float sourceDepth, uint32_t sourceStencil, float oldDepth, uint32_t oldStencil, float& depth, uint32_t& stencil) {
        (void)oldDepth;
        (void)oldStencil;
        stencil = 0;               // vkCmdClearAttachments, stencil aspect, 0
        depth = sourceDepth;       // gl_FragDepth = texelFetch(...).r
        for (uint32_t bit = 0; bit < 8; bit++)
            if ((sourceStencil >> bit) & 1)
                stencil = (stencil & ~(1u << bit)) | (0xFFu & (1u << bit));
    };
    bool preserved = true;
    const float depths[] = {0.0f, 1.0f, 0.5f, 1.0f / 8192.0f, 0.259765625f, DepthSelfCheckDepth(1279, 31)};
    for (const float d : depths)
        for (uint32_t st = 0; st < 256; st++)
        {
            float dd;
            uint32_t ds;
            passes(d, st, 0.75f, 0xA5, dd, ds);
            preserved = preserved && std::memcmp(&dd, &d, 4) == 0 && ds == st && PackDepthStencilWord(dd, ds) == PackDepthStencilWord(d, st);
        }
    CHECK(preserved);
}

// Storage 4 (16_16) through its R16G16_UINT transfer view (X vulkan_render_target_cache.cc:1628-1653, :3545-3576,
// :3692-3711, :5393-5404). The stop that motivates it: the
// 1x kD24FS8 depth key 6082D0 (base 720, 16 tiles a row) into the 4x 16_16 colour key 1106B40 (base 832, 13 tiles a row).
void TestK16_16()
{
    std::string why;
    // The keys by hand (edram.h Key: base bits 0-10, pitch 11-18, msaa 19-20, depth bit 21, format 22-25): depth 1 << 22 |
    // 1 << 21 | 16 << 11 | 720 = 0x400000 + 0x200000 + 0x8000 + 0x2D0; colour 4 << 22 | 2 << 19 | 13 << 11 | 832 = 0x1000000 +
    // 0x100000 + 0x6800 + 0x340.
    const Key d = Key::Depth(720, 16, Msaa::X1, DepthFormat::D24FS8), c4 = Key::Color(832, 13, Msaa::X4, ColorFormat::k16_16);
    CHECK(d.packed == 0x6082D0u && c4.packed == 0x1106B40u);
    CHECK(ClassifyTransfer(d, c4, why) == TransferKind::DepthToColor);
    // Colour 4 -> depth, 4 <-> 0 / 2 / 3 / 14 and 4 -> 4 (another sample count), the MSAA rules unchanged.
    const Key c4b = Key::Color(0, 16, Msaa::X1, ColorFormat::k16_16), c4a = Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16);
    CHECK(ClassifyTransfer(c4a, kAd, why) == TransferKind::ColorToDepth && ClassifyTransfer(c4b, kBd, why) == TransferKind::ColorToDepth);
    CHECK(ClassifyTransfer(c4a, kBc0, why) == TransferKind::Color && ClassifyTransfer(c4a, kBc2, why) == TransferKind::Color &&
          ClassifyTransfer(c4a, kB7, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kBc0, c4a, why) == TransferKind::Color && ClassifyTransfer(kAc, c4b, why) == TransferKind::Color &&
          ClassifyTransfer(kA7, c4b, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(c4a, c4b, why) == TransferKind::Color && ClassifyTransfer(c4b, c4a, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(c4b, Key::Color(0, 16, Msaa::X1, ColorFormat::k32_Float), why) == TransferKind::Color);
    CONTROL(ClassifyTransfer(c4a, c4a, why) != TransferKind::Refused);  // still a key as its own source
    CHECK(ClassifyTransfer(c4b, Key::Color(0, 16, Msaa::X2, ColorFormat::k16_16), why) == TransferKind::Color);  // 1x -> 2x (X :2887-2898)
    CHECK(ClassifyTransfer(c4b, Key::Depth(0, 16, Msaa::X2, DepthFormat::D24FS8), why) == TransferKind::ColorToDepth);  // colour -> 2x depth
    CHECK(ClassifyTransfer(kBd, Key::Depth(0, 16, Msaa::X2, DepthFormat::D24FS8), why) == TransferKind::Depth);  // 1x depth -> 2x

    // The transfer view (X :1637-1639, :1643-1644); the attachment stays R16G16_SNORM (X :1604-1608).
    CHECK(TransferViewFormat(ColorFormat::k16_16) == VK_FORMAT_R16G16_UINT && TransferViewFormat(ColorFormat::k32_Float) == VK_FORMAT_R32_UINT);
    CHECK(HostColorFormat(ColorFormat::k16_16) == VK_FORMAT_R16G16_SNORM);
    CONTROL(TransferViewFormat(ColorFormat::k2_10_10_10) != VK_FORMAT_UNDEFINED);  // transfers as floats

    // The word (X :3567-3574 BitFieldInsert(r, g, 16, 16)): halves r 0x1234, g 0xABCD -> 0xABCD1234; back (X :3701-3711).
    const uint32_t h[4] = {0x1234u, 0xABCDu, 0, 0};
    uint32_t u[4] = {9, 9, 9, 9};
    CHECK(PackTransferUint(h, 4) == 0xABCD1234u);
    UnpackTransferUint(0xABCD1234u, 4, u);
    CHECK(u[0] == 0x1234u && u[1] == 0xABCDu && u[2] == 0 && u[3] == 0);
    CONTROL(PackTransferUint(h, 14) == 0xABCD1234u);  // the R32_UINT texel is the word itself: 0x1234
    CONTROL(PackTransferUint(h, 2) == 0xABCD1234u);   // no transfer view: 0
    // 4 -> 4 passes 0x8000 (-32768, which an SNORM float path would return as 0x8001) through exactly.
    const uint32_t m[4] = {0x8000u, 0x8000u, 0, 0};
    const uint32_t mw = 0x8000u << 16 | 0x8000u;
    CHECK(PackTransferUint(m, 4) == mw);
    UnpackTransferUint(mw, 4, u);
    CHECK(u[0] == 0x8000u && u[1] == 0x8000u);

    // Depth -> 4 (the observed stop): kD24FS8 1.0 with stencil 0x5A -> word 0xF00000 << 8 | 0x5A = 0xF000005A (X :3605-3617)
    // -> r = word & 0xFFFF = 0x005A, g = word >> 16 = 0xF000.
    const uint32_t w = PackDepthStencilWord(1.0f, 0x5A);
    CHECK(w == (0xF00000u << 8 | 0x5Au));
    UnpackTransferUint(w, 4, u);
    CHECK(u[0] == 0x005Au && u[1] == 0xF000u && u[2] == 0 && u[3] == 0);
    CONTROL(u[0] == 0xF000u && u[1] == 0x005Au);  // halves swapped
    // 4 -> depth: those halves -> the same word; bits 8-31 0xF00000 as 20e4 (e 15, m 0) = 1.0, stencil 0x5A (X :4120-4124,
    // :4145-4152); as kD24S8, X :4102-4119 (n + (n >> 23)) * 2^-24 with n = 0xF00000: 0xF00001 * 2^-24.
    const uint32_t back[4] = {0x005Au, 0xF000u, 0, 0};
    const uint32_t bw = PackTransferUint(back, 4);
    CHECK(bw == 0xF000005Au && ColorWordDepth(bw) == 1.0f && ColorWordStencil(bw) == 0x5Au);
    CHECK(ColorWordDepth(bw, DepthFormat::D24S8) == std::ldexp(float(0xF00001u), -24));

    // 4 -> 0 (X :3627-3645 unpack by bytes): word 0xABCD1234 -> bytes 0x34, 0x12, 0xCD, 0xAB / 255, which pack back to it.
    float c[4];
    UnpackTransferColor(0xABCD1234u, 0, c);
    CHECK(c[0] == 0x34 * (1.0f / 255.0f) && c[1] == 0x12 * (1.0f / 255.0f) && c[2] == 0xCD * (1.0f / 255.0f) && c[3] == 0xAB * (1.0f / 255.0f));
    CHECK(PackTransferColor(c, 0) == 0xABCD1234u);
    // 0 -> 4: the 8_8_8_8 word 0x44332211 (TestPack) -> r 0x2211, g 0x4433.
    const float v8[4] = {0x11 / 255.0f, 0x22 / 255.0f, 0x33 / 255.0f, 0x44 / 255.0f};
    UnpackTransferUint(PackTransferColor(v8, 0), 4, u);
    CHECK(u[0] == 0x2211u && u[1] == 0x4433u);

    // Clears (X :5393-5404): uint32[j] = (value >> 16j) & 0xFFFF. 0x7FFF8001 -> {0x8001, 0x7FFF}; 1 << 31 -> {0, 0x8000}
    // (the G = -32768 the float path refused); 0x8000 in both halves survives.
    uint32_t k[4] = {9, 9, 9, 9};
    CHECK(DecodeColorClearUint(0x7FFF8001u, 4, k) && k[0] == 0x8001u && k[1] == 0x7FFFu && k[2] == 0 && k[3] == 0);
    CHECK(DecodeColorClearUint(1u << 31, 4, k) && k[0] == 0 && k[1] == 0x8000u);
    CHECK(DecodeColorClearUint(mw, 4, k) && k[0] == 0x8000u && k[1] == 0x8000u);
    CONTROL(DecodeColorClearUint(0x7FFF8001u, 4, k) && k[0] == 0x7FFF8001u);  // not the 32_FLOAT form
    CONTROL(DecodeColorClearUint(0x7FFF8001u, 2, k));
}

// The 16_16 resolve FM4 stops on: colour 0 of RB_COLOR_INFO format 4
// (storage 4), 4x, sample select 6 (k0123), into texture format 25 (k_16_16) with number 3, exp bias 10, no swap, endian 1
// (k8in16). The registers are a stand-in of that shape (the run logs no raw values): a 32x32 rectangle of a pitch-80
// surface, colour base 0, DEST_INFO 1 | 25 << 7 | 3 << 13 | 10 << 16 (X registers.h:812-826).
edram::ResolveRegisters Rg16Resolve(uint32_t surfaceInfo, uint32_t copyControl, uint32_t destInfo)
{
    edram::ResolveRegisters r = Float32Resolve(destInfo);
    r.surfaceInfo = surfaceInfo, r.colorInfo[0] = 0x00040000, r.copyControl = copyControl;
    return r;
}

void TestRg16Resolve()
{
    std::string why;
    const uint32_t destInfo = 1 | 25u << 7 | 3u << 13 | 10u << 16;
    // Surface 00020050: pitch 80, msaa_samples 2 (4x); RB_COPY_CONTROL 00100060: convert, copy_sample_select 6 (k0123).
    const edram::ResolveInfo rg = edram::DecodeResolve(Rg16Resolve(0x00020050, 0x00100060, destInfo));
    // The key: 80 pixels at 4x = 160 samples = 2 tiles a row (edram.h PitchTilesAt32bpp).
    CHECK(rg.ok && rg.source == Key::Color(0, 2, Msaa::X4, ColorFormat::k16_16) && rg.sampleSelect == edram::SampleSelect::k0123 &&
          rg.destFormat == 25 && rg.destExpBias == 10 && !rg.destSwap && rg.destEndian == 1 && rg.x1 == 32 && rg.y1 == 32);
    // Always X's full resolve: xenos.h:616-641 has no 16_16 case, so draw_util.cc:1312-1318 never takes the fast shader.
    CHECK(!ResolveUsesFastPath(rg, 4) && ResolveFullConvertible(rg, 4, why));
    uint32_t idx[4], n = 0;
    CHECK(ResolveSamples(rg, idx, n) && n == 4 && idx[0] == 0 && idx[1] == 1 && idx[2] == 2 && idx[3] == 3);
    // Even a 1x copy of sample 0 without a bias (the fast path's other conditions) is refused by the fast path (negative
    // control: storage 4 is never bitwise equivalent) and taken by the full one.
    const edram::ResolveInfo one = edram::DecodeResolve(Rg16Resolve(0x00000050, 0x00100000, 1 | 25u << 7));
    CHECK(one.ok && one.source == Key::Color(0, 1, Msaa::X1, ColorFormat::k16_16) && one.sampleSelect == edram::SampleSelect::k0 &&
          one.destExpBias == 0);
    CONTROL(ResolveUsesFastPath(one, 4) || ResolveConvertible(one, 4, why));
    CHECK(ResolveFullConvertible(one, 4, why));
    // Pairings: storage 4 only into 25, 25 only from storage 4; the read-back storage must be the key's.
    edram::ResolveInfo into6 = rg;
    into6.destFormat = 6;
    CONTROL(ResolveFullConvertible(into6, 4, why));
    CONTROL(ResolveFullConvertible(rg, 2, why));

    // The scale by hand: X's 32.0 / 32767.0 (pixel_formats.xesli:367-371) = 2^5 / (2^15 - 1) = 2^-10 / (1 - 2^-15)
    // = 2^-10 (1 + 2^-15 + 2^-30 + ...); as a float (23 fraction bits) the 2^-30 term is below half an ulp:
    // c = 2^-10 (1 + 2^-15). The bias factor (resolve.xesli:131-133) 2^10, halved twice for k0123 (:651, :670): 256.
    // Number 3 (signed integer; canary d119505 pixel_formats.xesli XePackFixed, ShiftGlue fh1_native_resolve_memory.cs.hlsl:218-232):
    // each half is int(clamp(v, -32768, 32767) + (v >= 0 ? 0.5 : -0.5)) & 0xFFFF, so with k0123 the output is the average of the four
    // samples' int16 values, rounded half away from zero.
    // Pixel A, guest samples (by guest index) 0x00010001, 0x00010000, 0x00010000, 0 (red the low half, :367-371):
    // red one unit, c * 256 = 0.25 (1 + 2^-15); + 0.5 -> int(0.75...) = 0. Green three units: 0.75 (1 + 2^-15) + 0.5 -> 1. Word 0x00010000.
    const uint32_t a[4] = {0x00010001u, 0x00010000u, 0x00010000u, 0};
    CHECK(ResolveConvertPixel(rg, 4, a) == 0x00010000u);
    CONTROL(ResolveConvertPixel(rg, 4, a) == 0xC0014000u);  // the UNORM pack this port used before (saturate * 65535)
    CONTROL(ResolveConvertPixel(rg, 4, a) == 0x00000001u);  // red and green exchanged
    // Pixel B, both -1 snorm encodings are -32 (ShiftGlue fh1_native_edram.hlsli:217-220; Xenia 67f16c4's max(-1, int16 c) would
    // give -1): sample 0 red 0x8001: -32767 c = -(2^30 - 1) / 2^25, as a float -32 (the 2^-25 term is below the ulp 2^-19), clamp
    // -32; sample 1 red 0x0400: 1024 c = 1 + 2^-15 exactly. Sum (-32 + 1 + 2^-15) + 0 + 0 = -31 + 2^-15 (ulp 2^-18: exact);
    // * 256 = -7936 + 2^-7 = -7935.9921875 (exact); negative: int(-7936.4921875) = -7936 = -0x1F00 -> 0xE100. Green 0.
    const uint32_t b[4] = {0x00008001u, 0x00000400u, 0, 0};
    CHECK(ResolveConvertPixel(rg, 4, b) == 0x0000E100u);
    CONTROL(ResolveConvertPixel(rg, 4, b) == 0u);  // Xenia's -1 clamp: (-1 + 1 + 2^-15) * 256 = 2^-7 -> 0 (and the old UNORM 0x200)
    const uint32_t b0[4] = {0x00008000u, 0x00000400u, 0, 0};  // -32768 c = -32 - 2^-10 - 2^-25: also clamped to -32: the same word
    CHECK(ResolveConvertPixel(rg, 4, b0) == 0x0000E100u);
    // A negative green half alone: 0xFFFF = -1 unit -> -c * 256 = -0.25 (1 + 2^-15); negative: int(-0.75...) = 0 (toward zero).
    // Red one unit -> 0 as in pixel A. Word 0.
    const uint32_t d[4] = {0xFFFF0001u, 0, 0, 0};
    CHECK(ResolveConvertPixel(rg, 4, d) == 0u);
    // The clamp at 32767 (r 0x0400, g 0x7FFF in every sample): red 4 (1 + 2^-15) * 256 = 1024 + 2^-5 -> int(1024.53125) = 1024 =
    // 0x0400; green 4 * 32 * 256 = 32768 -> 32767 -> int(32767.5) = 0x7FFF.
    const uint32_t s[4] = {0x7FFF0400u, 0x7FFF0400u, 0x7FFF0400u, 0x7FFF0400u};
    CHECK(ResolveConvertPixel(rg, 4, s) == 0x7FFF0400u);
    CONTROL(ResolveConvertPixel(rg, 4, s) == 0xFFFFFFFFu);  // the UNORM saturation of the old pack
    // The swap (DEST_INFO bit 24; resolve.xesli:699-704 .bgra before the pack): (r, g, 0, 0) -> (0, g, r, 0), and k_16_16
    // packs red and green (pixel_formats.xesli:236-239): pixel s -> red 0, green 0x7FFF.
    const edram::ResolveInfo swapped = edram::DecodeResolve(Rg16Resolve(0x00020050, 0x00100060, destInfo | 1u << 24));
    CHECK(swapped.ok && swapped.destSwap && ResolveConvertPixel(swapped, 4, s) == 0x7FFF0000u);
    // A 1x k0 resolve with number 3 and bias 10 (factor 1024, no halving) passes the int16 bits through, since 1024 c = 1 + 2^-15:
    // red 0x0400 -> 1024.03125 -> 0x0400; green 0xF800 (-2048 units: -2 (1 + 2^-15) * 1024 = -2048.0625) -> int(-2048.5625) = -2048
    // -> 0xF800 (Xenia's -1 clamp would make it -1024, 0xFC00); 0x7FFF -> 32 * 1024 = 32768 -> clamped 0x7FFF; 0xFC00 -> -1024.03125
    // -> 0xFC00; 0x8000 -> -32 * 1024 = -32768 -> int(-32768.5) = -32768 -> 0x8000.
    const edram::ResolveInfo pass = edram::DecodeResolve(Rg16Resolve(0x00000050, 0x00100000, destInfo));
    CHECK(pass.ok && pass.destNumber == 3 && pass.destExpBias == 10 && pass.sampleSelect == edram::SampleSelect::k0);
    const uint32_t p1[4] = {0xF8000400u, 0, 0, 0}, p2[4] = {0xFC007FFFu, 0, 0, 0}, p3[4] = {0x00008000u, 0, 0, 0};
    CHECK(ResolveConvertPixel(pass, 4, p1) == 0xF8000400u && ResolveConvertPixel(pass, 4, p2) == 0xFC007FFFu &&
          ResolveConvertPixel(pass, 4, p3) == 0x00008000u);
    CONTROL(ResolveConvertPixel(pass, 4, p1) == 0xFC000400u);  // the -1 clamp
    // Number 0 keeps the UNORM pack (pixel_formats.xesli:144-147): 1x k0, bias 0, sample 0x00010001: c * 65535 = 64.00098 + 0.5 ->
    // 64 = 0x40 in both halves.
    CHECK(one.destNumber == 0);
    const uint32_t u[4] = {0x00010001u, 0, 0, 0};
    CHECK(ResolveConvertPixel(one, 4, u) == 0x00400040u);

    // The rectangle into guest bytes (DumpLayout of 4x 32x32): pixel A at (0, 0), pixel B at (2, 3), every other sample 0.
    // Endian 1, k8in16 (X xenos.h:1070-1086; resolve_full_32bpp.xesli:62-65): 0x00010000 -> 0x01000000, stored little-endian
    // at TiledOffset 0: bytes 00 00 00 01; 0x0000E100 -> 0x000000E1 at TiledOffset(2, 3) = 280 (TestHdrResolve: only the
    // macro row term depends on the pitch, 0 for y < 32); (1, 0) at 4 is 0.
    const DumpLayout l = DumpLayoutFor(Msaa::X4, 32, 32);
    std::vector<uint32_t> words(size_t(l.width) * l.columns * l.height * l.rows, 0);
    for (uint32_t g = 0; g < 4; g++)
        words[l.Word(0, 0, g)] = a[g], words[l.Word(2, 3, g)] = b[g];
    std::vector<uint8_t> dest(rg.destExtentLength, 0xEE);
    CHECK(ResolveConvertToGuest(rg, 4, words.data(), dest.data(), uint32_t(dest.size()), why));
    CHECK(dest[0] == 0x00 && dest[1] == 0x00 && dest[2] == 0x00 && dest[3] == 0x01);
    CHECK(WordAt(dest, 280) == 0x000000E1u && WordAt(dest, 4) == 0u);
    CONTROL(WordAt(dest, 0) == 0x00010000u);  // without the endian swap
}

// The k_8_8 resolve FM4 stops on: colour 0 of RB_COLOR_INFO format 0
// (storage 0, 8_8_8_8), 1x, sample select 0, into texture format 10 (k_8_8) with number 0, exp bias 0, no swap, endian 1.
// The registers are a stand-in of that shape (the run logs no raw values): Float32Resolve's 32x32 rectangle of a pitch-80
// 1x surface, colour format 0, RB_COPY_CONTROL 00100000 (convert, k0).
edram::ResolveRegisters K88Resolve(uint32_t destInfo)
{
    edram::ResolveRegisters r = Float32Resolve(destInfo);
    r.surfaceInfo = 0x00000050, r.colorInfo[0] = 0, r.copyControl = 0x00100000;
    return r;
}

void TestK88Resolve()
{
    std::string why;
    const edram::ResolveInfo k = edram::DecodeResolve(K88Resolve(1 | 10u << 7));
    // 16 bpp (texture_info_formats.inl:11): the extent of a 32x32 copy at pitch 32 is TiledOffset2D(0, 0) + 0xC00 = 3072
    // (texture_util.cc:507-511), not the 32 bpp 0x1000.
    CHECK(k.ok && k.source == Key::Color(0, 1, Msaa::X1, ColorFormat::k8_8_8_8) && k.destFormat == 10 && k.destEndian == 1 &&
          k.destBytesLog2 == 1 && k.x1 == 32 && k.y1 == 32 && k.destExtentLength == 3072);
    // Never bitwise equivalent (xenos.h:616-641 lists no k_8_8): X's full 16 bpp shader (draw_util.cc:1333-1334).
    CHECK(!ResolveUsesFastPath(k, 0) && ResolveFullConvertible(k, 0, why));
    CONTROL(ResolveConvertible(k, 0, why));

    // The pack (pixel_formats.xesli:177-180, XePackR8G8B8A8UNorm :116-119): the 8_8_8_8 word 0x44332211 unpacks with red in
    // bits 0-7 (XeUnpackR8G8B8A8UNorm; R8G8B8A8_UNORM's first byte): r = 0x11 / 255, g = 0x22 / 255, b = 0x33 / 255.
    // uint(r * 255 + 0.5) = 17 = 0x11, g -> 0x22: the 16 bits r | g << 8 = 0x2211.
    const uint32_t p[1] = {0x44332211u};
    CHECK(ResolveConvertPixel(k, 0, p) == 0x2211u);
    CONTROL(ResolveConvertPixel(k, 0, p) == 0x1122u);      // red in the high byte
    CONTROL(ResolveConvertPixel(k, 0, p) == 0x44332211u);  // the 32 bpp k_8_8_8_8 pack
    // The swap (DEST_INFO bit 24; resolve.xesli:699-704 .bgra before the pack): red takes blue 0x33, green stays: 0x2233.
    const edram::ResolveInfo swapped = edram::DecodeResolve(K88Resolve(1 | 10u << 7 | 1u << 24));
    CHECK(swapped.ok && swapped.destSwap && ResolveConvertPixel(swapped, 0, p) == 0x2233u);
    // Saturation with exp bias 1 (bits 16-21; resolve.xesli:131-133 factor 2): word 0x0000C040: red 0x40 -> 64 / 255 * 2 * 255
    // + 0.5 = 128.5 -> 128 = 0x80; green 0xC0 -> 384 / 255 > 1 -> saturated to 255 (:116-119). 0xFF80.
    const edram::ResolveInfo biased = edram::DecodeResolve(K88Resolve(1 | 10u << 7 | 1u << 16));
    const uint32_t q[1] = {0x0000C040u};
    CHECK(biased.ok && ResolveFullConvertible(biased, 0, why) && ResolveConvertPixel(biased, 0, q) == 0xFF80u);
    CONTROL(ResolveConvertPixel(biased, 0, q) == 0x18080u);  // no saturation: green 384 = 0x180

    // Pairings: a 16_16 (storage 4) source only into 25; a 32_FLOAT (storage 14) source never into k_8_8; a decoded
    // destination whose bytes per texel disagree with the format is refused (the store would use the wrong layout).
    const edram::ResolveInfo rg = edram::DecodeResolve(Rg16Resolve(0x00000050, 0x00100000, 1 | 10u << 7));
    CHECK(rg.ok && rg.destFormat == 10);
    CONTROL(ResolveFullConvertible(rg, 4, why));
    const edram::ResolveInfo f32 = edram::DecodeResolve(Float32Resolve(1 | 10u << 7));
    CHECK(f32.ok && f32.source.Format() == uint32_t(ColorFormat::k32_Float));
    CONTROL(ResolveFullConvertible(f32, 14, why));
    edram::ResolveInfo wrong = k;
    wrong.destBytesLog2 = 2;
    CONTROL(ResolveFullConvertible(wrong, 0, why));

    // The rectangle into guest bytes (a 1x 32x32 read-back, DumpLayoutFor(X1): word y * 32 + x), 0x44332211 at (0, 0) and
    // (9, 3), every other texel 0. 0x2211 under 8in16 (endian.xesli:25-31 XeEndianSwap16) is 0x1122, stored little-endian:
    // bytes 22 11, i.e. the guest's big-endian 16-bit 0x2211.
    // TiledOffset2D(9, 3, 32, 1) by hand (texture_util.cc:443-458): macro 0; micro = ((9 & 7) + ((3 & 0xE) << 2)) << 1 =
    // (1 + 8) * 2 = 18; offset = ((18 & ~0xF) << 1) + (18 & 0xF) + ((3 & 1) << 4) = 32 + 2 + 16 = 50; mix: (50 & ~0x1FF) << 3
    // = 0, (3 & 16) << 7 = 0, (50 & 0x1C0) << 2 = 0, ((((3 & 8) >> 2) + (9 >> 3)) & 3) << 6 = 64, 50 & 0x3F = 50 -> 114.
    // The 32 bpp offset of the same texel (CONTROL): micro 9 << 2 = 36, offset 32 * 2 + 4 + 16 = 84, (84 & 0x1C0) << 2 = 256,
    // + 64 + (84 & 0x3F) 20 = 340.
    std::vector<uint32_t> texels(32 * 32, 0);
    texels[0] = texels[3 * 32 + 9] = 0x44332211u;
    std::vector<uint8_t> dest(k.destExtentLength, 0xEE);
    CHECK(ResolveConvertToGuest(k, 0, texels.data(), dest.data(), uint32_t(dest.size()), why));
    CHECK(dest[0] == 0x22 && dest[1] == 0x11 && dest[114] == 0x22 && dest[115] == 0x11);
    CONTROL(dest[0] == 0x11 && dest[1] == 0x22);      // without the endian swap
    CONTROL(dest[340] == 0x22 && dest[341] == 0x11);  // the 32 bpp address
    // Every texel is 2 bytes at a distinct tiled address: 32 * 32 * 2 = 2048 bytes written, the rest of the 0xC00 extent
    // untouched; texel (1, 0) at TiledOffset2D(1, 0, 32, 1) = 2 is 0.
    size_t written = 0;
    for (uint8_t b : dest)
        written += b != 0xEE;
    CHECK(written == 2048 && dest[2] == 0x00 && dest[3] == 0x00);
    // Endian 0 (no swap) stores 0x2211 little-endian: bytes 11 22.
    const edram::ResolveInfo plain = edram::DecodeResolve(K88Resolve(10u << 7));
    std::vector<uint8_t> dest0(plain.destExtentLength, 0xEE);
    CHECK(plain.ok && ResolveConvertToGuest(plain, 0, texels.data(), dest0.data(), uint32_t(dest0.size()), why) && dest0[0] == 0x11 &&
          dest0[1] == 0x22);
    // A destination too short for texel (31, 31) (TiledOffset2D 2942, below the 0xC00 bound): refused, nothing written.
    std::vector<uint8_t> shortDest(2048, 0xEE);
    CONTROL(ResolveConvertToGuest(k, 0, texels.data(), shortDest.data(), uint32_t(shortDest.size()), why));
    CHECK(shortDest[0] == 0xEE);
}

int main()
{
    TestFormats();
    TestClassify();
    TestDepthToColor();
    TestColorToDepth();
    TestTexelMap();
    Test7e3();
    TestPack();
    TestClears();
    TestGather();
    TestResolveToGuest();
    TestFastResidentWords();
    TestResolveWordsToGuest();
    TestTrace12Frame();
    Test7e3Frame();
    TestBases();
    TestHdrResolve();
    TestFloat32();
    TestColorToUnorm24();
    TestDepthToColorMs();
    TestDepthToColorPitch();
    TestDepthPitch();
    TestDepthResolveVk();
    TestDepth4xTo2x();
    TestK16_16();
    TestRg16Resolve();
    TestK88Resolve();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
