// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The pure (device-free) part of the Vulkan EDRAM unit (edram_vk.cpp, render_targets_vk.h): the host formats, which
// ownership transfers the M4 unit executes and how a transfer maps a destination sample to a source sample, the
// 32-bit packing a format-changing transfer goes through, the resolve clear values, the resolve registers, and the
// fallback resolve's CPU conversion of a read-back rectangle into tiled guest memory. Only Vulkan enums are used
// (no Vulkan call), so tests/runtime/test_edram_vk.cpp checks all of it on the CPU. Citations: X = xenia-canary
// 67f16c4e315f src/xenia/gpu (BSD-3-Clause), re-derived here, never copied.
#pragma once
#include "edram.h"
#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

namespace runtime::video {

// ---- Host formats (render_targets_vk.h plan 1) ------------------------------------------------------------------

// Declared in render_targets_vk.h. Storage 0 (8_8_8_8, GAMMA) -> R8G8B8A8_UNORM (GAMMA is the pixel shader's
// XE_COLOR0_GAMMA encode, pipeline_cache.h, not an sRGB view); storage 2 -> A2B10G10R10_UNORM_PACK32; storage 3
// (2_10_10_10_FLOAT, _AS_16_16_16_16; 7e3) -> R16G16B16A16_SFLOAT, as X vulkan_render_target_cache.cc:1601-1603
// does unconditionally on its host-render-target path (exact 7e3 storage exists only on X's pixel-shader-interlock
// path, render_target_cache.h:40-100, which lists 2_10_10_10_FLOAT under "significant differences"); it must
// equal pipeline_cache.cpp HostColorFormat(3), which renderer.cpp Draw checks. The rest VK_FORMAT_UNDEFINED. HostDepthFormat: D24_UNORM_S8_UINT for D24S8 only when `d24s8Supported`, else
// D32_SFLOAT_S8_UINT (X vulkan_render_target_cache.cc:1581-1588). The unit passes false: pipeline_cache.cpp
// HostDepthFormat is D32_SFLOAT_S8_UINT for both, and the renderer checks the two agree (renderer.cpp Draw).

// ---- The binding of a draw ---------------------------------------------------------------------------------------

struct RenderTarget;  // draw_state.h
// The edram::Bind input of a draw that needs exactly the renderer's attachments (draw_record.cpp KeyFromDrawState:
// colour 0 when written, depth when z or stencil is enabled): RB_SURFACE_INFO, RB_COLOR_INFO and RB_DEPTH_INFO
// re-packed from `rt` (X registers.h:618-628, 684-700, 779-789), RB_MODECONTROL kColorDepth, RB_COLOR_MASK 0xF on
// target 0 only, and RB_DEPTHCONTROL stencil_enable alone for a depth target (Bind's normalization, X
// draw_util.cc:60-79, then keeps it; z_enable without a write and with zfunc always would be dropped).
edram::BindRegisters BindRegistersFor(const RenderTarget& rt, bool needColor, bool needDepth);

// ---- Ownership transfers (render_targets_vk.h plan 4) ----------------------------------------------------------

enum class TransferKind
{
    Refused,
    Color,  // one colour draw per destination sample (sample masks), source sampled with texelFetch
    Depth,  // depth (gl_FragDepth) + 8 stencil-bit draws per destination sample
};

// What the unit does for a transfer `source` -> `dest` (both Keys of the same tiles). Accepted: colour 4x -> 1x and
// 1x -> 4x between storage formats 0 and 2 (the observed traffic; the pixel
// goes through the 32-bit packing when the formats differ: 4x s2 -> 1x s0 and 1x s0 -> 4x s2 each 38 times per 400
// swaps); colour between any two of storage 0, 2 and 3 (7e3) at 4x -> 1x, 1x -> 4x, or an unchanged sample count
// when the formats differ (a format-only reinterpretation through the 32-bit packing, X :3387-3700; not traced yet:
// FM4's 7e3 scene shares base 0 with the 2_10_10_10 targets, NOTES.md); depth 1x -> 4x of the same format.
// Everything else is refused with its name: a key as its own source, a transfer between keys of different bases or
// pitches (never observed; the texel mapping below assumes equal ones), 2x MSAA, depth 4x -> 1x, a depth format
// change or an aspect change (edram::Ownership::Change already refuses those two).
TransferKind ClassifyTransfer(edram::Key source, edram::Key dest, std::string& why);

// The source texel of a destination sample (X vulkan_render_target_cache.cc:2597-2601 "First sample bit at 4x
// with Vulkan standard locations - horizontal sample. Second sample bit - vertical sample"; same bpp 4x -> 1x
// :2804-2818, 1x -> 4x :2821-2829 and :2857-2864). Equal bases and pitches, so tile-relative and absolute
// pixel coordinates map the same way (a tile row is 80 samples: 80 1x pixels or 40 4x pixels).
struct SourceTexel
{
    uint32_t x = 0, y = 0, sample = 0;
};
SourceTexel MapTransferTexel(uint32_t destX, uint32_t destY, uint32_t destSample, edram::Msaa source, edram::Msaa dest);

// The 32-bit guest word of a colour as a format-changing transfer packs it (X :3402-3449 for 8_8_8_8:
// uint(v * 255 + 0.5) per component, R in bits 0-7; :3467-3490 for 2_10_10_10: rgb * 1023, a * 3, R in bits 0-9;
// :3497-3542 for 7e3: R, G, B through Float32To7e3 at bits 0, 10, 20, alpha uint(NClamp(a, 0, 1) * 3 + 0.5) at bit 30),
// and its inverse (X :3627-3690: component / 255, / 1023, alpha / 3; 7e3 R, G, B through Float7e3To32). `storage` 0,
// 2 or 3. The GLSL in edram_vk.cpp kTransferColor is the same arithmetic.
uint32_t PackTransferColor(const float rgba[4], uint32_t storage);
void UnpackTransferColor(uint32_t word, uint32_t storage, float rgba[4]);

// 7e3, the Xenos 10-bit unsigned float of colour formats 3 and 12 (X xenos.h:303 "7e3 [0, 32) RGB, unorm alpha"): bits
// 7-9 the exponent e (bias 3), bits 0-6 the mantissa m; e > 0: 2^(e - 3) * (1 + m / 128), e == 0: m / 512. Range
// [0, 31.875] (0x3FF); every value is exact in float16 and float32.
// Float32To7e3: X spirv_shader_translator_rb.cc:109-137 UnclampedFloat32To7e3 (NClamp to [0, 31.875]) then :25-107
// PreClampedFloat32To7e3 (after DirectXTex): below 0.25 (float bits 0x3E800000) the significand with its implicit 1 is
// shifted right by min(125 - exponent, 24), otherwise the exponent is rebiased by -124 << 23; then rounded to nearest
// even at bit 16 and bits 16-25 kept. FM4 Recomp difference: `f > 0` replaces NClamp's lower bound, so NaN, negatives
// and -0 give 0 (GLSL.std.450 NMax may return -0 for (-0, 0), whose bits would encode as 0x200 = 2.0). Inherited as is:
// below 0.25 the shifted-out bits are lost before rounding, so a value just above a tie rounds as the tie (APPROXIMATE;
// the hardware's rounding is UNKNOWN).
// Float7e3To32: the shader form, X spirv_shader_translator_rb.cc:139-235 Float7e3To32 and shaders/pixel_formats.xesli:
// 308-348 XeUnpackR10G10B10A2Float (mantissa at float bits 16-22). NOT X xenos.cc:103-119 Float7e3To32, which shifts the
// mantissa by 3 (the 20e4 shift) and so decodes every code with a nonzero mantissa wrongly (0x1C0 = 1.5 as 1.000061).
uint32_t Float32To7e3(float f);
float Float7e3To32(uint32_t f10);

// ---- Resolve clears (render_targets_vk.h plan 5) -----------------------------------------------------------------

// X vulkan_render_target_cache.cc:5366-5395: storage 0: byte j / 255; storage 2: 10-bit j / 1023, alpha bits 30-31 / 3;
// storage 3: 7e3 j (Float7e3To32, the shader form: X :5388 calls the CPU xenos.cc form, whose mantissa shift is wrong,
// see above), alpha bits 30-31 / 3. Every value is exact in R16G16B16A16_SFLOAT. False for another storage format.
bool DecodeColorClear(uint32_t value, uint32_t storage, float rgba[4]);
// X :5346-5363 without X's [0, 2) -> [0, 1) halving of 20e4 depth: unit 8's viewport does not halve (FoldViewport,
// draw_state.h) and edram::PrepareResolve refuses a 20e4 clear above 1.0 instead.
// kD24S8: X xenos.h:432-438 UNorm24To32((v >> 8) & 0xFFFFFF); kD24FS8: X xenos.cc:150-166 Float20e4To32; the
// stencil = v & 0xFF.
void DecodeDepthClear(uint32_t value, edram::DepthFormat format, float& depth, uint32_t& stencil);
float UNorm24To32(uint32_t n24);     // X xenos.h:432-438
float Float20e4To32(uint32_t f24);   // X xenos.cc:150-166

// ---- Resolve registers --------------------------------------------------------------------------------------------

using RegisterRead = uint32_t (*)(const void* context, uint32_t index);
// Every register edram::ResolveRegisters names, read through `read` (the CP's register file, kernel/graphics.h
// ReadRegister); `vertexWords` are left 0: the caller reads them from guest memory at the address of vertex fetch
// constant 0 (word 0 bits 2-31, X xenos.h:1135-1148) and stores them as loaded (little-endian words).
edram::ResolveRegisters GatherResolveRegisters(RegisterRead read, const void* context);
constexpr uint32_t ResolveVertexAddress(uint32_t fetch0) { return fetch0 & ~3u; }

// ---- The fallback resolve's CPU conversion (render_targets_vk.h plan 6 "fallback") ---------------------------

// `texels`: the read-back rectangle (info.x1 - info.x0) x (info.y1 - info.y0), row-major, tightly packed, each
// texel the host target's 32-bit word as vkCmdCopyImageToBuffer stores it (A2B10G10R10_UNORM_PACK32: R in bits 0-9,
// the Xenos 2_10_10_10 word; R8G8B8A8_UNORM: bytes R, G, B, A, i.e. the little-endian 8_8_8_8 word with R in bits
// 0-7). Writes each texel to `dest` (the guest bytes from info.destExtentStart, `destBytes` of them) at
// TiledOffset(destOffsetX + x, destOffsetY + y, destPitch, 2) (X shaders/resolve.xesli:161-184
// XeResolveDestPixelAddress, with info.destBase = info.destExtentStart), after X's fast 32bpp path
// (shaders/resolve_fast_32bpp_1x2xmsaa.xesli:64-74): the red/blue exchange when destSwap (resolve.xesli:769-777),
// then the endian swap (GpuSwap32), stored as a little-endian word. Only bitwise-equivalent destinations (X
// xenos.h:616-641): storage 0 -> 6 or 50, storage 2 -> 7 or 54; sample select k0 of a 1x source; anything else
// refused with the reason, nothing written.
bool ResolveToGuest(const edram::ResolveInfo& info, uint32_t sourceStorage, const uint32_t* texels, uint8_t* dest, uint32_t destBytes,
                    std::string& why);
// The checks of ResolveToGuest that do not need the texels, for the resolve hook to refuse before recording. A 7e3
// (storage 3) source is refused by name: X has no bitwise-equivalent texture format for it (xenos.h:616-641), so its
// resolve is always X's full converting one (draw_util.cc:1312-1347, shaders/resolve.xesli), not implemented in M4.
// Every refusal names the copy's shape (source storage, MSAA, sample select, destination format, bias, swap, endian).
bool ResolveConvertible(const edram::ResolveInfo& info, uint32_t sourceStorage, std::string& why);

}  // namespace runtime::video
