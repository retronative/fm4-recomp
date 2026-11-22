// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9, Extended View; kernel/extended_view.h): the title hook on the game's viewport aspect getter (width / height of a
// viewport, the result in f1). The per-view setup calls it twice for a view: once for the camera's aspect (the projection and the
// culling frustum read that field) and once for the view's record. Called from those two places for a full-screen 1280x720
// viewport, the result is multiplied by the latched factor; every other caller (a 2D overlay) and every other viewport (mirrors,
// cube maps and shadow maps set their own aspects and never ask) get the game's own answer. FM4's addresses and the evidence are
// in config/title_hooks.toml; none here.
// A window narrower than 16:9 (16:10) also needs a taller field of view: the same setup reads the view's field of view (radians)
// through a getter of two instructions, once before each aspect call; from those two places, for the same 1280x720 view (the
// setup's own r31 still holds it), the answer becomes view::ViewFov's, so the projection and the frustum again change together.
// FM4_DIAG_ASPECT_LOG=<lines> (diagnostic): each distinct (caller, viewport size, widened) once, with its count at the end of the
// run not kept: the first <lines> distinct callers are named when first seen.
#include "title_symbols.h"
#include "kernel/extended_view.h"
#include "kernel/graphics.h"
#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <tuple>

namespace fm4::title {

void ViewportAspect(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const uint32_t caller = uint32_t(ctx.lr), viewport = ctx.r3.u32;
    const uint32_t width = PPC_LOAD_U32(viewport + 8), height = PPC_LOAD_U32(viewport + 12);
    original(ctx, base);
    runtime::view::State& view = runtime::view::Global();
    const bool mainView = (caller == sym::kMainViewSetup + sym::kAspectForCamera || caller == sym::kMainViewSetup + sym::kAspectForRecord) && width == 1280 && height == 720;
    const float factor = view.Factor();
    const double before = ctx.f1.f64;
    if (mainView)
    {
        view.NoteWidened(runtime::gpu::VblankCount());
        if (factor != 1.0f)
            ctx.f1.f64 = double(runtime::view::ViewAspect(float(before), factor));  // the getter returns a single
    }
    static const long lines = [] { const char* v = std::getenv("FM4_DIAG_ASPECT_LOG"); return v != nullptr ? std::strtol(v, nullptr, 10) : 0L; }();
    if (lines > 0)
    {
        static std::mutex lock;
        static std::set<std::tuple<uint32_t, uint32_t, uint32_t, bool, int>> seen;
        std::lock_guard guard(lock);
        if (long(seen.size()) < lines && seen.insert({caller, width, height, mainView, int(factor * 1000.0f)}).second)
            std::fprintf(stderr, "[diag aspect] vblank %llu: the aspect getter called from %08X for a %ux%u viewport: %.4f%s\n",
                         (unsigned long long)runtime::gpu::VblankCount(), caller, width, height, before,
                         mainView ? (factor != 1.0f ? " -> reshaped" : " (a main view; factor 1)") : " (left alone)");
        if (mainView && factor != 1.0f && long(seen.size()) <= lines)
        {
            static bool said = false;
            if (!said)
                said = true, std::fprintf(stderr, "[diag aspect] a main view's aspect %.4f x %.4f = %.4f\n", before, double(factor), ctx.f1.f64);
        }
    }
}

void ViewFieldOfView(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const uint32_t caller = uint32_t(ctx.lr), view = ctx.r31.u32;  // r31: the per-view setup's view (only read for its two callers)
    original(ctx, base);
    if (caller != sym::kMainViewSetup + sym::kFovForCamera && caller != sym::kMainViewSetup + sym::kFovForRecord)
        return;
    if (PPC_LOAD_U32(view + 8 + 8) != 1280 || PPC_LOAD_U32(view + 8 + 12) != 720)  // the view's viewport: width, height
        return;
    const float factor = runtime::view::Global().Factor();
    const double before = ctx.f1.f64;
    if (factor < 1.0f)
        ctx.f1.f64 = double(runtime::view::ViewFov(float(before), factor));
    static const long lines = [] { const char* v = std::getenv("FM4_DIAG_ASPECT_LOG"); return v != nullptr ? std::strtol(v, nullptr, 10) : 0L; }();
    static long logged = 0;
    if (lines > 0 && factor < 1.0f && logged < 4)
        logged++, std::fprintf(stderr, "[diag aspect] vblank %llu: the field-of-view getter called from %08X for a main view: %.5f -> %.5f rad (factor %.4f)\n",
                               (unsigned long long)runtime::gpu::VblankCount(), caller, before, ctx.f1.f64, double(factor));
}

}  // namespace fm4::title
