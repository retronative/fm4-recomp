// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Portions copyright (c) 2026, i2kiselev (BSD 3-Clause; FM4 Static Recomp 0.3.2 host at 367bbea, src/car_dirt.cpp: which pixel shaders
// and constants, ComposePixelShaderConstantOverride). FM4 Recomp changes: the shaders by our hash (runtime/kernel/shader_hash.h,
// FNV-1a-64 of the microcode), mapped from the SDK's XXH3-64 of the same microcode over every shader of the disc (Media/Shaders and
// the .fxobj inside every zip) and a race's loaded shaders (2026-11-26: 31 of 31 dirt shaders, the road
// detail shader at run time; the replay blur shader 4746CAD32EF168CF was not found: replays only, not ported); applied to the draw's
// constant words before the renderer's reuse check (renderer.cpp), so a reused block always holds the overridden values.
//
// FM4 Recomp (opt-in FM4_SHADER_OVERRIDES=1; the GRAPHICS menu's MOTION BLUR and CAR DIRT rows, kernel/graphics_menu.h):
//   - MOTION BLUR off: the road detail pass (SDK PS 79B4914F27ECEC73) writes the grain's luminance times c5.w plus the speed streaks
//     times c5.x into the texture the road samples; a still camera gives c5 = (0, 0, 0, 1).
//   - CAR DIRT off: the car pixel shaders scale the dirt by c57.y (0 at a race start); c57.y = 0.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstddef>

namespace runtime::video::overrides {

constexpr uint64_t kRoadDetailShader = 0xf3f73beb09686d42ull;  // SDK 79B4914F27ECEC73
constexpr uint64_t kDirtShaders[] = {
    0x4b636e6f4d087d8dull,  // SDK 06BE1936C4522FFC
    0x55c4af75d8c4b541ull,  // SDK 08E3A599907F4E8C
    0xa9f2481d912d2812ull,  // SDK 0DD6E71970442D74
    0x976ccf3214cd7506ull,  // SDK 0F4A07657AF682FB
    0x6035d3a9956d6fbeull,  // SDK 134B2725B75271DC
    0xa2388ecbd8619b81ull,  // SDK 1D1DBE92CED7E7D7
    0x9c51bc8e4d2b8869ull,  // SDK 1F8A0299BFFB5109
    0xcbe9aea3e9823f04ull,  // SDK 30B95D950B29B7FF
    0x37fbd23f0bc6ad45ull,  // SDK 37AF896A2CBCE528
    0x0370fb1ace02db49ull,  // SDK 3FF6978DD63DDB54
    0xd45e298ad20846dfull,  // SDK 403213A5BCEDF8CA
    0x6c56396d3a8cf4f5ull,  // SDK 4042C4CC34BCA73D
    0x91de180cd1038857ull,  // SDK 46CB671D893E6709
    0x2838b9682109313bull,  // SDK 5AFF2866B278F96D
    0x15897307581db054ull,  // SDK 5FE934B3878F2660
    0x30f4026b8d97ccd7ull,  // SDK 670B85AE4903DC7E
    0x10dcca27ff28dd78ull,  // SDK 71C34CF563C167CA
    0x9cdc1a31e7530032ull,  // SDK 74A074E5889D0959
    0xb4b1572c93dfe6e6ull,  // SDK 7FDDAE0F0B88E28A
    0xd5a5acdebf60d22dull,  // SDK 892947FF214FF4E8
    0x7283ea3152d83108ull,  // SDK 8E39781009A79A81
    0xb5cb037b87e011d7ull,  // SDK B1C51D0C3686C920
    0x6ee2d8d486455355ull,  // SDK B85EF04779ABD438
    0xaa8dd50f013a9ba7ull,  // SDK C5862561EE9EC6CB
    0xcabcdd042f50c92eull,  // SDK C58B812D2C1BF687
    0x56c9265f4ee077adull,  // SDK C6A382D1784CEFCA
    0x005e96318820e88eull,  // SDK D9AA68D7F015BD51
    0x4fb02de24c2b8189ull,  // SDK E5B1A71249D0CA9A
    0x2c3eea5a4b440c2cull,  // SDK F3F74C657AC30AAD
    0xd5dccea8f2cf5114ull,  // SDK F53EE850DFF13F04
    0x1ad10ad0ea56bbfdull,  // SDK F57EF6715B0638C3
};

// The pixel stage's constant words start at word 1024 of DrawState::aluConstants (256 float4 per stage); cN.k is word 4N + k.
constexpr size_t kPixelBase = 1024;

// `words`: the draw's 2048 ALU constant words (host-order floats); `psWords`: how many of the pixel stage's the shader reads
// (DrawState::psConstantWords). Returns true when it changed a word. A constant beyond psWords is not read by the shader and left.
inline bool Apply(uint64_t psHash, bool motionBlurOff, bool carDirtOff, uint32_t* words, size_t count, uint32_t psWords)
{
    if (count < 2048)
        return false;
    const auto set = [&](uint32_t reg, uint32_t component, float value) {
        const uint32_t i = 4 * reg + component;
        if (i >= psWords)
            return false;
        const uint32_t bits = std::bit_cast<uint32_t>(value);
        const bool changed = words[kPixelBase + i] != bits;
        words[kPixelBase + i] = bits;
        return changed;
    };
    bool changed = false;
    if (motionBlurOff && psHash == kRoadDetailShader)
    {
        const bool c0 = set(5, 0, 0.0f), c1 = set(5, 1, 0.0f), c2 = set(5, 2, 0.0f), c3 = set(5, 3, 1.0f);
        changed = c0 || c1 || c2 || c3;
    }
    if (carDirtOff && std::find(std::begin(kDirtShaders), std::end(kDirtShaders), psHash) != std::end(kDirtShaders))
        changed = set(57, 1, 0.0f) || changed;
    return changed;
}

}  // namespace runtime::video::overrides
