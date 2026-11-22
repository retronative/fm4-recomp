// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SpeedBreaker / ReCarbon contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9, opt-in FM4_INTERNAL_RES): internal rendering at 1.5 (FHD, 1920x1080), 2 (QHD, 2560x1440) or 3 (4K, 3840x2160) times
// the guest's pixels. The scale is kept in HALVES (2 = 1x, 3 = 1.5x, 4 = 2x, 6 = 3x), so every quantity stays an integer: every EDRAM key's host image
// is halves / 2 times the guest size on both axes (one scale for every key, as Xenia's draw_resolution_scale, so ownership
// transfers stay tile copies: research/m9/qhd_plan/RECARBON_MAP.md 3.1; a tile's pixel sizes 80, 40, 20, 10 x 16, 8 are all even,
// so a tile is a whole number of host pixels at 1.5x too); the CPU EDRAM model (edram.h) stays in guest pixels and rectangles are
// converted at the Vulkan boundary, edge by edge (HostEdge: at 1.5x an odd guest edge lands half a host pixel in and is floored, the
// same for both rectangles that share it, so neighbours neither overlap nor leave a gap). Resolves write 1x words to guest memory (a box-downsample), as ReCarbon does
// (runtime/gpu/renderer.cpp:4714-4730, :4865-4873 at 66401ea), so every CPU and texture consumer keeps seeing guest-sized data.
// Adapted in part from ReCarbon runtime/gpu/render_scale.h (GPL-3.0-or-later; FM4 Recomp's names and its halves): the
// host-edge arithmetic (HostEdge, :69) and the fail-closed parse (ParseRenderScale, :220-243).
//
// No Vulkan (tests/runtime/test_render_scale.cpp). InternalHalves() (render_scale.cpp, with a Stop) and InternalHalvesQuiet() read
// the variable.
// FM4 Recomp (M9, live resolution, step 1): the scale is runtime state (detail::currentHalves), seeded once from the variable by
// whichever of the two getters runs first; every reader goes through them, so nothing keeps a copy made at start.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace runtime::video::scale {

constexpr uint32_t kHalves1x = 2;  // the scale in halves: 2 = 1x, 3 = 1.5x, 4 = 2x, 6 = 3x
constexpr uint32_t kHalvesMax = 6;
// "1.5", "2" or "3" (the log's spelling of a scale above 1x; "1" at 1x).
constexpr const char* HalvesName(uint32_t halves) { return halves == 3 ? "1.5" : halves == 4 ? "2" : halves == 6 ? "3" : "1"; }

// FM4_INTERNAL_RES: unset, empty, "720p" or "1x" -> 2 halves (today's path); "fhd", "1080p" or "1.5x" -> 3; "qhd", "1440p" or
// "2x" -> 4; "4k", "2160p" or "3x" -> 6. Anything else: false with the accepted spellings in `error` (the caller stops: a misspelt setting must not silently
// render at 1x).
inline bool ParseInternalRes(const char* value, uint32_t& halves, std::string& error)
{
    if (value == nullptr || *value == '\0' || std::strcmp(value, "720p") == 0 || std::strcmp(value, "1x") == 0)
        return halves = 2, true;
    if (std::strcmp(value, "fhd") == 0 || std::strcmp(value, "1080p") == 0 || std::strcmp(value, "1.5x") == 0)
        return halves = 3, true;
    if (std::strcmp(value, "qhd") == 0 || std::strcmp(value, "1440p") == 0 || std::strcmp(value, "2x") == 0)
        return halves = 4, true;
    if (std::strcmp(value, "4k") == 0 || std::strcmp(value, "2160p") == 0 || std::strcmp(value, "3x") == 0)
        return halves = 6, true;
    error = std::string("FM4_INTERNAL_RES '") + value + "': use 720p (or 1x; the default), fhd (or 1080p, 1.5x), qhd (or 1440p, 2x) or 4k (or 2160p, 3x)";
    return false;
}

// A guest pixel edge in host pixels: floor(guest * halves / 2). Exact for an even edge or an even number of halves; an odd edge at
// 1.5x lies half a host pixel in and takes the host pixel edge before it.
constexpr uint32_t HostEdge(uint32_t guest, uint32_t halves) { return guest * halves / 2; }
// Whether a guest size or edge has a whole host size (a host image's or a copy's extent must: never rounded).
constexpr bool HostExact(uint32_t guest, uint32_t halves) { return (guest * halves) % 2 == 0; }

// A guest rectangle (origin and size in guest pixels) in host pixels: each of its four edges through HostEdge (never the size times
// the scale: at 1.5x that would move a shared edge by a pixel between two neighbours).
struct Rect
{
    uint32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};
constexpr Rect HostRect(const Rect& guest, uint32_t halves)
{
    const uint32_t x0 = HostEdge(guest.x, halves), y0 = HostEdge(guest.y, halves);
    return {x0, y0, HostEdge(guest.x + guest.width, halves) - x0, HostEdge(guest.y + guest.height, halves) - y0};
}

// The guest pixel a host pixel lies in (the pixel whose area holds the host pixel's first half: floor(host * 2 / halves)).
constexpr uint32_t GuestOfHost(uint32_t host, uint32_t halves) { return host * 2 / halves; }

// The resolve downsample's box filter along one axis (edram_vk.cpp kDownsampleColor computes the same in GLSL): guest pixel `guest`
// covers the host span [guest * halves / 2, (guest + 1) * halves / 2); in half host pixels that is [guest * halves, +halves), and
// host pixel k covers [2k, 2k + 2). The first host pixel it touches and the halves it covers of that pixel and of the next two
// (halves <= 6: never more than three host pixels): 2x -> (2, 2, 0); 1.5x -> (2, 1, 0) for an even guest pixel, (1, 2, 0) for an odd
// one; 3x -> (2, 2, 2).
constexpr uint32_t kBoxTaps = 3;
struct BoxTaps
{
    uint32_t first = 0, weight[kBoxTaps] = {};
    bool operator==(const BoxTaps&) const = default;
};
constexpr BoxTaps DownsampleTaps(uint32_t guest, uint32_t halves)
{
    const uint32_t lo = guest * halves, hi = lo + halves, k = lo / 2;
    const auto cover = [&](uint32_t pixel) {
        const uint32_t a = pixel * 2 > lo ? pixel * 2 : lo, b = pixel * 2 + 2 < hi ? pixel * 2 + 2 : hi;
        return b > a ? b - a : 0u;
    };
    return {k, {cover(k), cover(k + 1), cover(k + 2)}};
}
// The host pixel a guest pixel covers whole (a depth resolve's unfiltered pick): 2x -> 2g; 1.5x -> 3m for g = 2m, 3m + 2 for 2m + 1;
// 3x -> 3g (the first of its three).
constexpr uint32_t WholeHostPixel(uint32_t guest, uint32_t halves) { return (guest * halves + 1) / 2; }

// FM4 Recomp (M9, live resolution, step 1): the one owner of the current scale, in halves; 0 until a getter below has read
// FM4_INTERNAL_RES. In the header (an inline variable) because the pure units and their tests do not link render_scale.cpp. Atomic
// because not every reader is on the command processor's thread; nothing stores to it after the first read (a later switch would,
// on the command processor's thread at a swap's safe point only: renderer.cpp ServeResolutionRequest).
namespace detail {
inline std::atomic<uint32_t> currentHalves{0};
}

// The current scale in halves: FM4_INTERNAL_RES read once (a Stop on a value it does not accept), one log line when it is above 1x.
uint32_t InternalHalves();

// The same value for the pure units (draw_record, host_samples), without the Stop: a value the parse refuses reads as 1x
// here (the renderer stops on it at start through InternalHalves, before any draw).
inline uint32_t InternalHalvesQuiet()
{
    uint32_t value = detail::currentHalves.load(std::memory_order_relaxed);
    if (value == 0)  // not read yet: the variable, quietly (a reader storing meanwhile stores the same value, or stops)
    {
        uint32_t s = kHalves1x;
        std::string ignored;
        const uint32_t parsed = ParseInternalRes(std::getenv("FM4_INTERNAL_RES"), s, ignored) ? s : kHalves1x;
        if (detail::currentHalves.compare_exchange_strong(value, parsed, std::memory_order_relaxed))
            value = parsed;  // otherwise `value` now holds what the other reader stored
    }
    return value;
}

// FM4 Recomp (M9, live resolution, step 2): the scale from here on. Only the renderer's switch calls it (renderer.cpp
// SwitchResolution: the command processor's thread, at a swap, with no submission in flight and every object of the old scale
// already destroyed or replaced), and only after InternalHalves() has run once (its first call stores the variable's value).
// False, changing nothing, for anything but the four scales the parse gives.
constexpr bool HalvesAccepted(uint32_t halves) { return halves == 2 || halves == 3 || halves == 4 || halves == 6; }
inline bool SetInternalHalves(uint32_t halves)
{
    if (!HalvesAccepted(halves) || detail::currentHalves.load(std::memory_order_relaxed) == 0)
        return false;
    detail::currentHalves.store(halves, std::memory_order_relaxed);
    return true;
}
// A scale's spelling as FM4_INTERNAL_RES and graphics.conf write it (the parse's first spelling of each).
constexpr const char* HalvesSpelling(uint32_t halves) { return halves == 3 ? "fhd" : halves == 4 ? "qhd" : halves == 6 ? "4k" : "720p"; }

// FM4 Recomp (M9, GPU cost; opt-in FM4_INTERNAL_SCALE_PITCH=1): only the EDRAM keys whose pitch is a full-width surface are scaled:
// 16 tiles or more at 32 bpp (1280 samples: a 1280-wide 1x or 2x surface, a 640-wide 4x one, FM4's 1280-wide 4x bands). Narrower keys
// (the shadow map's 520x4x, the reflection and environment targets) stay 1x: their resolves only ever feed 1x textures, so their
// scaled pixels were pure cost (FM4_DIAG_PASS_TIMES, QHD race: about 3 ms a frame). As ReCarbon scales only pitches 640-1280
// (runtime/gpu/renderer.cpp:2093-2096, :2127-2128 at 66401ea). An ownership transfer between a scaled and an unscaled key then
// reads the nearest source pixel (edram_vk_plan.h MapTransferSource's `sourceHalves`): tiles change owner with their picture at the
// lower of the two resolutions.
constexpr uint32_t kScaledPitchTiles = 16;
constexpr uint32_t HalvesForPitch(uint32_t pitchTilesAt32bpp, uint32_t halves, bool pitchRule)
{
    return !pitchRule || pitchTilesAt32bpp >= kScaledPitchTiles ? halves : kHalves1x;
}
inline bool PitchScaled()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_INTERNAL_SCALE_PITCH");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

// FM4_INTERNAL_OUTPUT: what the display image (the window's source and the captures) holds above 1x. Unset, empty or "native": the
// internal resolution (1920x1080, 2560x1440), from the scaled front buffer; "720p": 1280x720 from the 1x words (the scaled targets'
// box-downsample: supersampling only, the first QHD runs' mode). Anything else: false with the spellings in `error`.
inline bool ParseInternalOutput(const char* value, bool& native, std::string& error)
{
    if (value == nullptr || *value == '\0' || std::strcmp(value, "native") == 0)
        return native = true, true;
    if (std::strcmp(value, "720p") == 0)
        return native = false, true;
    error = std::string("FM4_INTERNAL_OUTPUT '") + value + "': use native (the default: the display image at the internal resolution) or 720p";
    return false;
}
// Whether FM4_INTERNAL_OUTPUT is native (or unset); read once. A value the parse refuses reads as native here (the renderer stops
// on it at start through InternalHalves).
inline bool OutputNative()
{
    static const bool native = [] {
        bool n = true;
        std::string ignored;
        ParseInternalOutput(std::getenv("FM4_INTERNAL_OUTPUT"), n, ignored);
        return n;
    }();
    return native;
}
// Whether the display image is at the internal resolution at a scale of `halves` (above 1x and not FM4_INTERNAL_OUTPUT=720p).
inline bool OutputScaledAt(uint32_t halves) { return OutputNative() && halves > kHalves1x; }
// The same at the current scale. FM4 Recomp (M9, live resolution, step 1): computed from the current scale at each call (it was
// read once); the variable's part is still read once.
inline bool OutputScaled() { return OutputNative() && InternalHalvesQuiet() > kHalves1x; }

// FM4 Recomp (M9): a pixel shader variant compiled with XE_HALVES (above 1x; draw_record.cpp adds it to the variants that read the
// pixel position): the translator's two gl_FragCoord reads (shader_glsl.cpp param_gen and alpha-to-mask) in guest pixels, the host
// pixel's guest pixel floor(floor(host) * 2 / halves) (GuestOfHost; RECARBON_MAP.md A9, A10; ShiftGlue spirv_translator.cpp:3525-3540
// divides without the outer floor). Applied to the variant's text only, so a 1x run's shader text stays byte-identical. False when a read is not found.
inline bool ScaleFragCoordReads(std::string& glsl)
{
    static const struct { const char* from; const char* to; } kReads[] = {
        {"float x = abs(floor(gl_FragCoord.x)), y = abs(floor(gl_FragCoord.y));",
         "float x = abs(floor(floor(gl_FragCoord.x) * 2.0 / float(XE_HALVES))), y = abs(floor(floor(gl_FragCoord.y) * 2.0 / float(XE_HALVES)));"},
        {"uvec2 xe_p = uvec2(gl_FragCoord.xy);", "uvec2 xe_p = uvec2(gl_FragCoord.xy) * 2u / XE_HALVES;"}};
    bool any = false;
    for (const auto& r : kReads)
        if (const size_t at = glsl.find(r.from); at != std::string::npos)
            glsl.replace(at, std::strlen(r.from), r.to), any = true;
    return any;
}

}  // namespace runtime::video::scale
