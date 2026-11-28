// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
// Portions copyright (c) 2026, i2kiselev (BSD 3-Clause; FM4 Static Recomp 0.3.2 host at 367bbea9, src/game_hooks.cpp, ApplyRenderScenarioSettings
// and the value tables above it). FM4 Recomp changes: the guest writes split into a pure plan (this header, tested on a synthetic
// layout) and a thin writer (render_scenario.cpp); the control ids and offsets come from config/title_hooks.toml, not from code;
// the settings arrive as a struct instead of cvars; no diagnostic omit list or LOD threshold override.
//
// FM4 Recomp: the extra graphics settings that live in the game's own render scenario manager (CDynamicRenderSettings). Each
// setting the game reads goes through an accessor that first tests a bit of the manager's override mask (bit `id`: the word at
// +overrideMask for ids 0..31, the next word for 32..63) and, when it is set, reads the override value at
// manager + overrideValues + offset[id] (offset: the game's control offset table, one word per id); otherwise it walks the stack
// of RenderScenarios\*.xml files. So a value written there with its bit set wins in every scenario, and clearing the bit gives
// the game's own value back. Pure: no guest memory, no title address (the layout is a parameter).
//   tests/run_render_scenario_tests.sh (tests/runtime/test_render_scenario.cpp)
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fm4::title::scenario {

// The game's control ids of the values the settings write (config/title_hooks.toml [[value]] kScenario*).
struct Controls
{
    uint32_t carLodThresholds, carFocusLodMinMax, carLodTransDist, carKeepLod0, carForceNoLod0, carMinWheelLod;
    uint32_t collidablesInMirror, collidableShadows, crowdLoad, envMapFrequencyScale, maxCarsInMirror, halfRateMirror;
    uint32_t halfRateBloom, softParticles;
};

// Where the override layer is in the manager.
struct Layout
{
    uint32_t overrideValues, overrideMask;
    Controls ids;
};

enum class Reflections : uint8_t { Console, EveryRace, Double };
enum class Detail : uint8_t { Console, High, Ultra };

// Every member's default is the game's own value: nothing written, its bit cleared.
struct Settings
{
    bool sceneryShadows = false;           // CollidableShadows 1: shadows of cones and barriers in races too
    Reflections reflections = Reflections::Console;  // EnvMapFrequencyScale 1.0 (every race) or 2.0 (double)
    bool fullMirror = false;               // HalfRateMirror 0, MaxCarsInMirror 16, CollidablesInMirror 1
    bool softParticles = false;            // SoftParticles 1
    bool crowds = false;                   // CrowdLoad 1
    bool fullBloom = false;                // HalfRateBloom 0
    Detail detail = Detail::Console;       // the car LOD values (High, Ultra)
};

// One forced value: a byte, or `count` big-endian words.
struct Value
{
    uint32_t id;
    bool byte;
    uint32_t count;
    uint32_t words[3];
};

inline Value Byte(uint32_t id, uint8_t v) { return {id, true, 1, {v, 0, 0}}; }
inline Value Words(uint32_t id, uint32_t count, uint32_t a, uint32_t b = 0, uint32_t c = 0) { return {id, false, count, {a, b, c}}; }
inline Value Float(uint32_t id, float v) { return {id, false, 1, {std::bit_cast<uint32_t>(v), 0, 0}}; }
inline Value Floats(uint32_t id, float a, float b, float c)
{
    return {id, false, 3, {std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b), std::bit_cast<uint32_t>(c)}};
}

// The values `s` forces, in a fixed order (the reference's: detail, mirror, bloom, particles, shadows, reflections, crowds).
// The numbers are the reference's (game_hooks.cpp kHighDetail ... kCrowdsEveryRace), re-checked against FM4's accessors (README).
inline std::vector<Value> Values(const Settings& s, const Controls& c)
{
    std::vector<Value> out;
    if (s.detail == Detail::High)
    {
        out.push_back(Words(c.carLodThresholds, 3, 3, 5, 7));
        out.push_back(Floats(c.carLodTransDist, 20.0f, 35.0f, 70.0f));
    }
    else if (s.detail == Detail::Ultra)
    {
        out.push_back(Words(c.carLodThresholds, 3, 4, 6, 8));
        out.push_back(Floats(c.carLodTransDist, 30.0f, 60.0f, 120.0f));
        out.push_back(Words(c.carMinWheelLod, 1, 0));
        out.push_back(Byte(c.carKeepLod0, 1));
        out.push_back(Byte(c.carForceNoLod0, 0));
        out.push_back(Words(c.carFocusLodMinMax, 2, 0, 0));
    }
    if (s.fullMirror)
    {
        out.push_back(Byte(c.halfRateMirror, 0));
        out.push_back(Words(c.maxCarsInMirror, 1, 16));
        out.push_back(Byte(c.collidablesInMirror, 1));
    }
    if (s.fullBloom)
        out.push_back(Byte(c.halfRateBloom, 0));
    if (s.softParticles)
        out.push_back(Byte(c.softParticles, 1));
    if (s.sceneryShadows)
        out.push_back(Byte(c.collidableShadows, 1));
    if (s.reflections != Reflections::Console)
        out.push_back(Float(c.envMapFrequencyScale, s.reflections == Reflections::EveryRace ? 1.0f : 2.0f));
    if (s.crowds)
        out.push_back(Byte(c.crowdLoad, 1));
    return out;
}

inline uint64_t Bit(uint32_t id) { return id < 64 ? uint64_t(1) << id : 0; }

// Every id some setting may own: the bits a setting back at the game's value clears. Other bits are the game's and are kept.
inline uint64_t OwnedMask(const Controls& c)
{
    uint64_t m = 0;
    for (const uint32_t id : {c.carLodThresholds, c.carFocusLodMinMax, c.carLodTransDist, c.carKeepLod0, c.carForceNoLod0, c.carMinWheelLod,
                              c.collidablesInMirror, c.collidableShadows, c.crowdLoad, c.envMapFrequencyScale, c.maxCarsInMirror,
                              c.halfRateMirror, c.halfRateBloom, c.softParticles})
        m |= Bit(id);
    return m;
}

// One guest store: `size` 1 (a byte) or 4 (a big-endian word) at `address`.
struct Store
{
    uint32_t address;
    uint8_t size;
    uint32_t value;
    bool operator==(const Store& o) const { return address == o.address && size == o.size && value == o.value; }
};

// The stores that make the manager at `manager` hold `s`: every forced value first, then the two mask words (read-modify-write:
// the bits the game owns kept, ours set or cleared), so a reader never sees a bit set before its value. `load(address)` reads a
// big-endian guest word (the offset table and the current mask words). An id the layout cannot place (64 or more) is skipped.
template <typename Load>
std::vector<Store> Plan(const Layout& layout, uint32_t manager, uint32_t offsetTable, const Settings& s, Load load)
{
    std::vector<Store> out;
    uint64_t mask = 0;
    for (const Value& v : Values(s, layout.ids))
    {
        if (v.id >= 64)
            continue;
        mask |= Bit(v.id);
        const uint32_t at = manager + layout.overrideValues + load(offsetTable + 4 * v.id);
        if (v.byte)
            out.push_back({at, 1, v.words[0] & 0xFF});
        else
            for (uint32_t i = 0; i < v.count; i++)
                out.push_back({at + 4 * i, 4, v.words[i]});
    }
    const uint64_t owned = OwnedMask(layout.ids);
    for (uint32_t half = 0; half < 2; half++)
    {
        const uint32_t at = manager + layout.overrideMask + 4 * half;
        const uint32_t ours = uint32_t(owned >> (32 * half)), set = uint32_t(mask >> (32 * half));
        out.push_back({at, 4, (load(at) & ~ours) | set});
    }
    return out;
}

}  // namespace fm4::title::scenario
