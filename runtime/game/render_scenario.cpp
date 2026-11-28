// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
// Portions copyright (c) 2026, i2kiselev (BSD 3-Clause; FM4 Static Recomp 0.3.x host, src/game_hooks.cpp: ApplyRenderScenarioSettings,
// called every main-loop tick from its Fm4MainLoopSession / Fm4PresentInterval hooks, and Fm4DepthOfFieldOption). FM4 Recomp changes:
// whole-function delegate hooks instead of mid-function hooks (our generator has no mid-function kind), the plan in render_scenario.h,
// FM4's numbers from config/title_hooks.toml (title_symbols.h), the settings from graphics.conf (kernel/graphics_menu.h RenderNow).
//
// FM4 Recomp: the GRAPHICS menu's render scenario settings (SCENERY SHADOWS, REFLECTION UPDATES, MIRROR, SOFT PARTICLES, CROWDS,
// BLOOM, LEVEL OF DETAIL) and DEPTH OF FIELD, through two title hooks (config/title_hooks.toml, kind "delegate"):
//   - RenderScenarioTick: the main loop calls one function once a tick (its only caller); before it runs, the manager's override
//     layer is made to hold the settings now (render_scenario.h Plan). Every tick, as the reference does, so a race loads its cars
//     with the values in place and a change in the menu or the Settings panel reaches the game at its next tick. The stores are the
//     game thread's own, on the main loop's thread; the readers on other threads see a byte or an aligned word change, value first.
//   - DepthOfFieldSwitch: the renderer's depth-of-field switch (renderer, enable) gets enable 0 while DEPTH OF FIELD is OFF.
// A setting at the game's own value writes nothing and clears its bit: the game's scenario files decide again.
#include "title_symbols.h"
#include "render_scenario.h"
#include "kernel/graphics_menu.h"
#include "ppc_recomp_shared.h"
#include <cstdio>

namespace fm4::title {
namespace {

namespace settings = runtime::settings;

scenario::Settings FromGraphics(const settings::Graphics& g)
{
    scenario::Settings s;
    s.sceneryShadows = g.shadows == 1;
    s.reflections = g.reflections == 1 ? scenario::Reflections::EveryRace : g.reflections == 2 ? scenario::Reflections::Double : scenario::Reflections::Console;
    s.fullMirror = g.mirror == 1;
    s.softParticles = g.particles == 1;
    s.crowds = g.crowds == 1;
    s.fullBloom = g.bloom == 1;
    s.detail = g.detail == 1 ? scenario::Detail::High : g.detail == 2 ? scenario::Detail::Ultra : scenario::Detail::Console;
    return s;
}

const scenario::Layout& FmLayout()
{
    using namespace fm4::sym;
    static const scenario::Layout layout{kScenarioOverrideValues, kScenarioOverrideMask,
                                         {kScenarioCarLodThresholds, kScenarioCarFocusLodMinMax, kScenarioCarLodTransDist, kScenarioCarKeepLod0,
                                          kScenarioCarForceNoLod0, kScenarioCarMinWheelLod, kScenarioCollidablesInMirror, kScenarioCollidableShadows,
                                          kScenarioCrowdLoad, kScenarioEnvMapFrequencyScale, kScenarioMaxCarsInMirror, kScenarioHalfRateMirror,
                                          kScenarioHalfRateBloom, kScenarioSoftParticles}};
    return layout;
}

void ApplyRenderScenario(uint8_t* base)
{
    const uint32_t manager = PPC_LOAD_U32(sym::kScenarioManagerPointer);
    if (manager == 0)
        return;  // before the renderer made it: the next tick
    const settings::Graphics now = settings::RenderNow();
    const scenario::Settings s = FromGraphics(now);
    static settings::Graphics logged{};
    static bool said = false;
    if (!said || !(now == logged))
    {
        said = true, logged = now;
        std::fprintf(stderr, "[graphics] render scenario: shadows %s, reflections %s, mirror %s, particles %s, crowds %s, bloom %s, detail %s\n",
                     settings::kShadows[now.shadows].file, settings::kReflections[now.reflections].file, settings::kMirrors[now.mirror].file,
                     settings::kParticles[now.particles].file, settings::kCrowds[now.crowds].file, settings::kBlooms[now.bloom].file,
                     settings::kDetails[now.detail].file);
    }
    for (const scenario::Store& st :
         scenario::Plan(FmLayout(), manager, sym::kScenarioControlOffsets, s, [base](uint32_t address) { return uint32_t(PPC_LOAD_U32(address)); }))
    {
        if (st.size == 1)
            PPC_STORE_U8(st.address, uint8_t(st.value));
        else
            PPC_STORE_U32(st.address, st.value);
    }
}

}  // namespace

// The main loop's per-tick call (its only caller; the return address is checked all the same, as ViewportAspect does).
void RenderScenarioTick(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    if (uint32_t(ctx.lr) == sym::kMainLoop + sym::kMainLoopTick)
        ApplyRenderScenario(base);
    original(ctx, base);
}

// The renderer's depth-of-field switch (r3 the renderer, r4 enable).
void DepthOfFieldSwitch(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    if (ctx.r4.u32 != 0 && settings::RenderNow().dof == 1)
    {
        static bool said = false;
        if (!said)
            said = true, std::fprintf(stderr, "[graphics] depth of field: off (the game asked for it on)\n");
        ctx.r4.u64 = 0;
    }
    original(ctx, base);
}

}  // namespace fm4::title
