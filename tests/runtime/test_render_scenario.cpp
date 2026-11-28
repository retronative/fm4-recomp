// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (render scenario settings): T0 tests of runtime/game/render_scenario.h, CPU only, on a synthetic manager at 65536 (no
// title address). The control ids and table offsets are FM4's, read by hand from its accessors and its offset table (README of the
// draft; config/title_hooks.toml): id 7 -> 124, 8 -> 140, 10 -> 164, 11 -> 180, 12 -> 188, 13 -> 196, 18 -> 236, 19 -> 244,
// 20 -> 252, 29 -> 336, 30 -> 344, 31 -> 352, 32 -> 360, 33 -> 368; the override values at manager + 64 + offset, the mask words at
// manager + 96 and + 100. Expected stores are computed by hand in the comments (IEEE-754 singles by hand: 1.0 = 3F800000,
// 2.0 = 40000000, 20 = 41A00000, 35 = 420C0000, 70 = 428C0000, 30 = 41F00000, 60 = 42700000, 120 = 42F00000). "CONTROL" names a
// wrong behaviour that must not happen.
//
//   tests/run_render_scenario_tests.sh
#include "render_scenario.h"
#include <cstdio>
#include <map>
#include <vector>

using namespace fm4::title::scenario;

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

constexpr uint32_t kManager = 65536, kTable = 4096, kMaskLo = kManager + 96, kMaskHi = kManager + 100;
const Layout kLayout{64, 96, {7, 8, 10, 11, 12, 13, 18, 19, 20, 29, 30, 31, 32, 33}};

// The guest words the plan reads: the offset table and the two mask words.
struct Memory
{
    std::map<uint32_t, uint32_t> words;
    Memory(uint32_t lo = 0, uint32_t hi = 0)
    {
        const std::map<uint32_t, uint32_t> offsets = {{7, 124}, {8, 140}, {10, 164}, {11, 180}, {12, 188}, {13, 196}, {18, 236},
                                                      {19, 244}, {20, 252}, {29, 336}, {30, 344}, {31, 352}, {32, 360}, {33, 368}};
        for (const auto& [id, offset] : offsets)
            words[kTable + 4 * id] = offset;
        words[kMaskLo] = lo, words[kMaskHi] = hi;
    }
    uint32_t operator()(uint32_t address) const
    {
        const auto it = words.find(address);
        return it == words.end() ? 0xDEADBEEFu : it->second;  // an unexpected read shows up as a wrong address
    }
};

std::vector<Store> PlanOf(const Settings& s, const Memory& m = Memory()) { return Plan(kLayout, kManager, kTable, s, m); }

// Value stores at manager + 64 + offset (65600 + offset), then the two mask words.
std::vector<Store> Expected(std::vector<Store> values, uint32_t lo, uint32_t hi)
{
    values.push_back({kMaskLo, 4, lo});
    values.push_back({kMaskHi, 4, hi});
    return values;
}
}  // namespace

int main()
{
    // The bits a setting may own: ids 7, 8, 10..13 (0x80 + 0x100 + 0x400 + 0x800 + 0x1000 + 0x2000 = 0x3D80), 18..20 (0x1C0000),
    // 29..31 (0xE0000000) in the low word: 0xE01C3D80; 32 and 33 in the high word: 3.
    Expect(OwnedMask(kLayout.ids) == ((uint64_t(3) << 32) | 0xE01C3D80u), "owned mask: ids 7, 8, 10-13, 18-20, 29-33");

    Settings console;
    Expect(PlanOf(console) == Expected({}, 0, 0), "console: no value stored, both mask words written back clear");
    // Every bit set before (the game's and ours): ours cleared, the game's kept. ~0xE01C3D80 = 0x1FE3C27F; ~3 = 0xFFFFFFFC.
    Expect(PlanOf(console, Memory(0xFFFFFFFFu, 0xFFFFFFFFu)) == Expected({}, 0x1FE3C27Fu, 0xFFFFFFFCu),
           "console after every bit set: our bits cleared, the game's bits kept");

    Settings shadows;
    shadows.sceneryShadows = true;
    Expect(PlanOf(shadows) == Expected({{65600 + 244, 1, 1}}, 1u << 19, 0), "scenery shadows: CollidableShadows byte 1, bit 19");

    Settings every;
    every.reflections = Reflections::EveryRace;
    Settings twice;
    twice.reflections = Reflections::Double;
    Expect(PlanOf(every) == Expected({{65600 + 336, 4, 0x3F800000u}}, 1u << 29, 0) &&
               PlanOf(twice) == Expected({{65600 + 336, 4, 0x40000000u}}, 1u << 29, 0),
           "reflection updates: EnvMapFrequencyScale word 1.0 (every race) or 2.0 (double), bit 29");

    Settings mirror;
    mirror.fullMirror = true;
    // HalfRateMirror byte 0 (id 31), MaxCarsInMirror word 16 (id 30), CollidablesInMirror byte 1 (id 18): bits 31, 30, 18.
    Expect(PlanOf(mirror) == Expected({{65600 + 352, 1, 0}, {65600 + 344, 4, 16}, {65600 + 236, 1, 1}}, (1u << 31) | (1u << 30) | (1u << 18), 0),
           "mirror full: half rate off, 16 cars, collidables; bits 31, 30, 18");

    Settings particles;
    particles.softParticles = true;
    Settings bloom;
    bloom.fullBloom = true;
    Expect(PlanOf(particles) == Expected({{65600 + 368, 1, 1}}, 0, 2) && PlanOf(bloom) == Expected({{65600 + 360, 1, 0}}, 0, 1),
           "soft particles (id 33: high word bit 1) and full-rate bloom (id 32: high word bit 0)");

    Settings crowds;
    crowds.crowds = true;
    Expect(PlanOf(crowds) == Expected({{65600 + 252, 1, 1}}, 1u << 20, 0), "crowds every race: CrowdLoad byte 1, bit 20");

    Settings high;
    high.detail = Detail::High;
    Expect(PlanOf(high) == Expected({{65600 + 124, 4, 3}, {65600 + 128, 4, 5}, {65600 + 132, 4, 7},
                                     {65600 + 164, 4, 0x41A00000u}, {65600 + 168, 4, 0x420C0000u}, {65600 + 172, 4, 0x428C0000u}},
                                    (1u << 7) | (1u << 10), 0),
           "level of detail high: thresholds 3, 5, 7 and distances 20, 35, 70; bits 7, 10");

    Settings ultra;
    ultra.detail = Detail::Ultra;
    Expect(PlanOf(ultra) == Expected({{65600 + 124, 4, 4}, {65600 + 128, 4, 6}, {65600 + 132, 4, 8},
                                      {65600 + 164, 4, 0x41F00000u}, {65600 + 168, 4, 0x42700000u}, {65600 + 172, 4, 0x42F00000u},
                                      {65600 + 196, 4, 0}, {65600 + 180, 1, 1}, {65600 + 188, 1, 0}, {65600 + 140, 4, 0}, {65600 + 144, 4, 0}},
                                     0x3D80u, 0),
           "level of detail ultra: thresholds 4, 6, 8, distances 30, 60, 120, wheel LOD 0, keep LOD0, no force, focus 0..0; bits 7, 8, 10-13");

    // Back to the console's after shadows were on: the game's bit 5 (not ours) kept, bit 19 cleared, nothing stored.
    Expect(PlanOf(console, Memory((1u << 19) | (1u << 5), 0)) == Expected({}, 1u << 5, 0), "a setting back at the game's value clears only its bit");

    // Every setting at once: the values (in the plan's order) come first, the two mask words last.
    Settings all;
    all.sceneryShadows = all.fullMirror = all.softParticles = all.crowds = all.fullBloom = true;
    all.reflections = Reflections::Double;
    all.detail = Detail::Ultra;
    const std::vector<Store> plan = PlanOf(all);
    bool masksLast = plan.size() >= 2 && plan[plan.size() - 2].address == kMaskLo && plan.back().address == kMaskHi;
    for (size_t i = 0; i + 2 < plan.size(); i++)
        masksLast = masksLast && plan[i].address != kMaskLo && plan[i].address != kMaskHi;
    Expect(masksLast && plan[plan.size() - 2].value == 0xE01C3D80u && plan.back().value == 3u, "everything: values first, then both mask words, every owned bit set");

    CONTROL(PlanOf(shadows) == Expected({{65600 + 244, 1, 1}}, 0, 1u << 19), "bit 19 in the high word");
    CONTROL(PlanOf(particles) == Expected({{65600 + 368, 1, 1}}, 1u << 1, 0), "id 33 in the low word");
    CONTROL(PlanOf(shadows) == Expected({{65536 + 244, 1, 1}}, 1u << 19, 0), "the value without the override's +64");
    CONTROL(PlanOf(every) == Expected({{65600 + 336, 4, 0x0000803Fu}}, 1u << 29, 0), "1.0 in the host's byte order");
    CONTROL(PlanOf(console, Memory(1u << 5, 0)) == Expected({}, 0, 0), "the game's own bit cleared");

    // An id the mask cannot hold (64 or more) is not written.
    Layout wide = kLayout;
    wide.ids.softParticles = 70;
    Expect(Plan(wide, kManager, kTable, particles, Memory()) == Expected({}, 0, 0), "an id of 64 or more: no store, no bit");

    std::printf("%d failure(s), %d control(s)\n", failures, controls);
    return failures == 0 ? 0 : 1;
}
