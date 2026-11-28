// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (FM4_SHADER_OVERRIDES): T0 tests of runtime/video/shader_overrides.h, CPU only. Expected words by hand: the pixel
// constants start at word 1024; c5 = words 1044..1047, c57.y = word 1024 + 4 * 57 + 1 = 1253; 1.0f = 0x3F800000, 0.0f = 0.
// "CONTROL" names a wrong behaviour that must not happen.
//
//   tests/run_shader_overrides_tests.sh
#include "shader_overrides.h"
#include <cstdio>
#include <vector>

namespace o = runtime::video::overrides;
namespace {
int failures = 0, controls = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}
#define CONTROL(wrong, name)                \
    do                                      \
    {                                       \
        controls++;                         \
        Expect(!(wrong), "control: " name); \
    } while (0)
std::vector<uint32_t> Filled() { return std::vector<uint32_t>(2048, 0x40000000u); }  // every word 2.0f
}  // namespace

int main()
{
    {
        auto w = Filled();
        Expect(o::Apply(o::kRoadDetailShader, true, false, w.data(), w.size(), 256), "road detail, motion blur off: changed");
        Expect(w[1044] == 0 && w[1045] == 0 && w[1046] == 0 && w[1047] == 0x3F800000u, "c5 = (0, 0, 0, 1): words 1044..1047");
        Expect(w[1043] == 0x40000000u && w[1048] == 0x40000000u, "c4.w and c6.x untouched");
        CONTROL(o::Apply(o::kRoadDetailShader, true, false, w.data(), w.size(), 256), "a second apply changes nothing");
    }
    {
        auto w = Filled();
        CONTROL(o::Apply(o::kRoadDetailShader, false, true, w.data(), w.size(), 256), "road detail with only car dirt off: unchanged");
        Expect(w[1044] == 0x40000000u, "c5.x stays");
    }
    {
        auto w = Filled();
        const uint64_t dirt = o::kDirtShaders[0];
        Expect(o::Apply(dirt, false, true, w.data(), w.size(), 232), "a dirt shader reading c0..c57 (232 words), car dirt off: changed");
        Expect(w[1253] == 0 && w[1252] == 0x40000000u && w[1254] == 0x40000000u, "only c57.y (word 1253) is 0");
        auto v = Filled();
        CONTROL(o::Apply(dirt, false, true, v.data(), v.size(), 228), "a shader reading 228 words (c0..c56) does not read c57: unchanged");
        auto x = Filled();
        CONTROL(o::Apply(dirt, true, false, x.data(), x.size(), 256), "a dirt shader with only motion blur off: unchanged");
    }
    {
        auto w = Filled();
        CONTROL(o::Apply(0x1234ull, true, true, w.data(), w.size(), 256), "another shader: unchanged");
        auto s = std::vector<uint32_t>(1024, 0);
        CONTROL(o::Apply(o::kRoadDetailShader, true, true, s.data(), s.size(), 256), "fewer than 2048 words: unchanged");
    }
    Expect(sizeof o::kDirtShaders / sizeof o::kDirtShaders[0] == 31, "31 dirt shaders (FM4 Static Recomp car_dirt.cpp kDirtPixelShaders)");
    std::printf("%s: %d failure(s), %d control(s)\n", failures ? "FAILED" : "passed", failures, controls);
    return failures ? 1 : 0;
}
