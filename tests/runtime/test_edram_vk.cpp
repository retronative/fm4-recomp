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
    CONTROL(HostColorFormat(ColorFormat::k16_16) != VK_FORMAT_UNDEFINED);  // refused in M4 (edram::SupportedKey)
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
    // Refused by name.
    CHECK(ClassifyTransfer(kAd, kBd, why) == TransferKind::Refused && why.find("4x to 1x") != std::string::npos);
    // An unchanged sample count with a format change: a reinterpretation through the 32-bit packing (X :3387-3700).
    CHECK(ClassifyTransfer(kBc2, kBc0, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(Key::Color(0, 16, Msaa::X4, ColorFormat::k8_8_8_8), kAc, why) == TransferKind::Color);
    // 7e3 (storage 3) with every other accepted storage, both directions, at 4x <-> 1x and at an unchanged count.
    CHECK(ClassifyTransfer(kAc, kA7, why) == TransferKind::Color && ClassifyTransfer(kA7, kAc, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kA7, kBc2, why) == TransferKind::Color && ClassifyTransfer(kBc0, kA7, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kA7, kB7, why) == TransferKind::Color && ClassifyTransfer(kB7, kA7, why) == TransferKind::Color);
    CHECK(ClassifyTransfer(kA7, kA7, why) == TransferKind::Refused && why.find("own source") != std::string::npos);
    CHECK(ClassifyTransfer(Key::Color(0, 16, Msaa::X4, ColorFormat::k16_16), kA7, why) == TransferKind::Refused &&
          why.find("storage format other than") != std::string::npos);
    CHECK(ClassifyTransfer(kA7, Key::Color(0, 16, Msaa::X1, ColorFormat::k32_Float), why) == TransferKind::Refused);
    CHECK(ClassifyTransfer(Key::Color(16, 16, Msaa::X4, ColorFormat::k2_10_10_10_Float), kB7, why) == TransferKind::Refused &&
          why.find("different bases") != std::string::npos);
    CHECK(ClassifyTransfer(Key::Color(16, 16, Msaa::X4, ColorFormat::k2_10_10_10), kBc2, why) == TransferKind::Refused &&
          why.find("different bases") != std::string::npos);
    CHECK(ClassifyTransfer(Key::Color(0, 8, Msaa::X4, ColorFormat::k2_10_10_10), kBc2, why) == TransferKind::Refused);
    CHECK(ClassifyTransfer(Key::Color(0, 16, Msaa::X2, ColorFormat::k2_10_10_10), kBc2, why) == TransferKind::Refused && why.find("2x") != std::string::npos);
    CHECK(ClassifyTransfer(Key::Depth(720, 16, Msaa::X1, DepthFormat::D24S8), kAd, why) == TransferKind::Refused);
    CHECK(ClassifyTransfer(kBd, kAc, why) == TransferKind::Refused && why.find("aspects") != std::string::npos);
    CONTROL(ClassifyTransfer(kAc, kAc, why) != TransferKind::Refused);  // a key is never its own source
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
    CHECK(DecodeColorClear((2u << 30 | 0x001201C0u), 3, c) && c[0] == 1.5f && c[1] == 0.25f && c[2] == 0.001953125f && c[3] == 2 * (1.0f / 3));
    // CONTROL: X's CPU xenos.cc:117 form, (3 + 124) << 23 | 64 << 3 = 0x3F800200 = 1.00006104, is not what is decoded.
    CONTROL(c[0] == 1.0f + std::ldexp(1.0f, -14));
    CHECK(DecodeColorClear(0u, 3, c) && c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f);
    CHECK(DecodeColorClear(0xFFFFFFFFu, 3, c) && c[0] == 31.875f && c[1] == 31.875f && c[2] == 31.875f && c[3] == 1.0f);
    CONTROL(DecodeColorClear(0u, 4, c) || DecodeColorClear(0u, 14, c));  // 16_16 and 32_FLOAT storage are refused in M4
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
    // CONTROL: a depth-only binding of the 1x depth key after step 7 would need 4x -> 1x depth, which the unit refuses.
    b = edram::Bind(o, BindRegistersFor(Target1x(10), false, true), 720);
    CONTROL(b.ok && AllClassified(b.transfers[0], kBd, TransferKind::Depth));
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
    // 4. ForzaRecomp's depth base 0x400 (1024) under the 4x 7e3 clear: tiles 1024-1439 still belong to the depth key at
    //    base 720, a transfer between keys of different bases, which the unit refuses by name (the predicted next stop:
    //    NOTES.md). The colour side is accepted (1x s3 -> 4x s3).
    t7.depthBase = 1024;
    b = edram::Bind(o, BindRegistersFor(t7, true, true), 360);
    const Key d1024 = Key::Depth(1024, 16, Msaa::X4, DepthFormat::D24FS8);
    std::string why;
    CHECK(b.ok && b.depth == d1024 && b.transfers[0] == std::vector<T>{{1024, 1440, kAd}} &&
          ClassifyTransfer(kAd, d1024, why) == TransferKind::Refused && why.find("different bases") != std::string::npos);
    CHECK(AllClassified(b.transfers[1], kA7, TransferKind::Color) && b.transfers[1] == std::vector<T>{{0, 720, kB7}});
}

}  // namespace

int main()
{
    TestFormats();
    TestClassify();
    TestTexelMap();
    Test7e3();
    TestPack();
    TestClears();
    TestGather();
    TestResolveToGuest();
    TestTrace12Frame();
    Test7e3Frame();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
