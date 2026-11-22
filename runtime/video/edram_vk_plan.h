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
#include "render_hooks.h"  // FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): PendingClear (PushPendingClear)
#include <cstdint>
#include <string>
#include <vector>
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

// ---- The render area of an EDRAM-unit pass (M7 GPU cost) -------------------------------------------------------

// FM4 Recomp: the render area of a transfer or resolve-clear pass over host target `width` x `height`: the full width and only
// the rows the rectangles touch, rounded outwards to 32 and clamped to the target; no rectangle -> the whole target. Load
// and store act only inside the render area (Vulkan VkRenderPassBeginInfo::renderArea; pixels outside it are untouched),
// and every transfer draw and clear is scissored to its rectangle, so nothing is rendered outside the area. A tile-based
// GPU loads and stores the area, not the image: FM4 touches 360-720 rows of 1024- and 2048-row host targets.
VkRect2D UnitPassArea(const VkRect2D* rects, size_t count, uint32_t width, uint32_t height);

// FM4 Recomp: a new pending clear of `add` on a target with the pending rectangles `rects` (oldest first): every older clear whose
// rectangle lies wholly inside `add` is dropped (the new clear overwrites it), the rest stay in order, `add` goes last. Returns how
// many were dropped. Values are not compared: order alone keeps the result right.
size_t MergePendingClear(std::vector<VkRect2D>& rects, VkRect2D add);
// FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): `add` appended to a target's pending clears (oldest first) under MergePendingClear's rule:
// the older clears whose rectangles lie wholly inside add's are dropped with their values, the rest keep their order and values.
// Returns how many were dropped. The resolve clears (edram_vk.cpp Clear) and the clear-class draws (AcquireTargets) both go through it.
size_t PushPendingClear(std::vector<PendingClear>& pending, const PendingClear& add);
// FM4 Recomp (M7 GPU cost, FM4_CLEAR_FLUSH_OVERLAP; after SpeedBreaker 312ce69 runtime/gpu/renderer.cpp:2749-2780 FlushPendingClears,
// GPL-3.0, adapted to EDRAM tiles): which of the pending clears of `key`'s target (oldest first) must land before a read of the
// tiles `read`. Newest first, a clear is taken when its tiles (edram::RectTiles of its rectangle in key's layout) meet `read` or the
// tiles of a clear already taken (a newer clear it must land under), and its tiles join that set; the rest stay pending, in order,
// for the next pass on the target (each later reader runs this again). Returns the indices taken, ascending (the recording order).
// `read` == nullptr: every index (FlushClears without the flag, today's flush of all of them). Tile overlap is coarser than pixel
// overlap (two pixel-disjoint clears in one tile are taken together): conservative, since every reader copies whole tiles.
std::vector<size_t> SelectClearFlush(edram::Key key, const std::vector<PendingClear>& pending, const edram::TileSet* read);
// The clear's VkRect2D as a rectangle of its host image (a negative offset is cut at 0; nothing left: an empty rectangle).
edram::Rect PendingClearRect(const VkRect2D& r);

// ---- Ownership transfers (render_targets_vk.h plan 4) ----------------------------------------------------------

enum class TransferKind
{
    Refused,
    Color,  // one colour draw per destination sample (sample masks), source sampled with texelFetch
    Depth,  // depth (gl_FragDepth) + 8 stencil-bit draws per destination sample
    // X kDepthToColor (vulkan_render_target_cache.cc:176-177, :4910-4913): one colour draw per destination sample, the
    // source's depth and stencil aspects sampled, packed into the guest's 32-bit depth/stencil word, unpacked as colour.
    DepthToColor,
    // X kColorToDepth + kColorToStencilBit (vulkan_render_target_cache.cc:170-171, :178-180, :4904-4908): the colour sample
    // packed to the guest's 32-bit word (X :3385-3586), its bits 8-31 the 20e4 depth (X :3736-3741, :4120-4124
    // Depth20e4To32), bits 0-7 the stencil, written bit by bit (X :4156-4180). As Depth, one draw per destination sample,
    // with the 40-sample column swap (:2916-2936). FM4 Recomp reduction: no host-depth owner (edram.cpp Ownership::Change).
    ColorToDepth,
};

// What the unit does for a transfer `source` -> `dest` (both Keys of the same tiles). Accepted: colour 4x -> 1x and
// 1x -> 4x between storage formats 0 and 2 (the observed traffic; the pixel
// goes through the 32-bit packing when the formats differ: 4x s2 -> 1x s0 and 1x s0 -> 4x s2 each 38 times per 400
// swaps); colour between any two of storage 0, 2 and 3 (7e3) at 4x -> 1x, 1x -> 4x, or an unchanged sample count
// when the formats differ (a format-only reinterpretation through the 32-bit packing, X :3387-3700; not traced yet:
// FM4's 7e3 scene shares base 0 with the 2_10_10_10 targets); depth 1x -> 4x of the same format.
// Any bases (MapEdramTexel's tile offset; observed: the 1x kD24FS8 key at 720 -> the 4x
// kD24FS8 key at 1024), any pitches (MapEdramTexel). FM4 Recomp: depth -> depth between kD24FS8 and kD24S8 is Depth too,
// the shader reinterpreting the guest bits (ConvertDepthFormat). Everything else is refused with its name: a key as its own
// source, 2x MSAA, depth 4x -> 1x.
// Depth -> colour (DepthToColor; observed: tiles 720-1023 of the 1x kD24FS8
// key at base 720, which the front-buffer resolve's depth clear last claimed, move to the 4x 7e3 colour key at base
// 0): accepted from a kD24FS8 or kD24S8 (observed: the 4x kD24S8 key 308000 into
// the 1x 7e3 key C08000) source into a colour key of storage 0, 2 or 3, at any pitches (observed:
// 16 -> 32 tiles a row; MapEdramTexel), any bases and any MSAA
// pair MapTransferTexel implements (a multisampled source read per sample with sampler2DMS / usampler2DMS, X
// :2307-2358, :3052-3080); 1x <-> 2x is refused by name as for colour.
// Storage 4 (16_16) as source or destination of Color, DepthToColor and ColorToDepth, with the same rules, through its
// R16G16_UINT transfer view (X :1628-1653, :3545-3576, :3692-3711; observed: the 1x kD24FS8 key
// 6082D0 into the 4x 16_16 key 1106B40). Storage 14 (32_FLOAT) stays colour <-> colour only.
// FM4 Recomp (alpha, RT format 5): storage 5 (16_16_16_16, 64 bpp) is Color to and from every colour storage above and itself at
// another sample count (MapTransferSource); storage 5 -> depth is ColorToDepth (the half of the source sample, TransferWordOf64) and
// depth -> storage 5 DepthToColor (FM4 Recomp, alpha, photo mode: two depth samples a 64 bpp sample, MapTransferSource's first and
// second, each PackDepthStencilWord, joined by TransferTexelOf64; X :3005-3023, :3303-3333, :3352-3370), with the MSAA pairs above.
// The other 64 bpp formats (16_16_16_16_FLOAT, 32_32_FLOAT) are refused by name.
TransferKind ClassifyTransfer(edram::Key source, edram::Key dest, std::string& why);

// FM4 Recomp (M7 GPU cost, FM4_HOST_MSAA_1X): the source bits of an EDRAM unit pipeline (edram_vk.cpp Pipeline's `sourceFlags`) for a
// source host image of `hostSamples` samples: bit 0 the multisampled variant (XE_SOURCE_MS: sampler2DMS / usampler2DMS) exactly when
// the IMAGE has more than one sample (a sampler2DMS over a 1-sample view, or a sampler2D over a multisampled one, is invalid Vulkan;
// with FM4_HOST_MSAA_1X a 4x key's image has one sample: bit 0 clear, the shader's texelFetch reads sample 0); bit 1 read through the
// transfer view (`uintView`: storage 4, 14, 5); bit 3 a 64 bpp colour source (`colour64`: storage 5, never a depth key). Bit 2
// (sample shading) is the caller's.
uint32_t TransferSourceFlags(uint32_t hostSamples, bool uintView, bool colour64);

// The guest's 32-bit depth/stencil word from the host's float depth and 8-bit stencil (X :3605-3617): kD24FS8 depth
// through PreClampedDepthTo20e4 (X spirv_shader_translator_rb.cc:236-326) with X's defaults round_to_nearest_even =
// depth_float24_round = false (render_target_cache.cc:46) and remap_from_0_to_0_5 = false (FM4 Recomp difference: X
// passes true because it stores guest depth halved, :5357-5359; this renderer stores it unhalved, edram_vk_plan.h
// DecodeDepthClear), at bits 8-31, the stencil at bits 0-7. `depth` in [0, 1] (a D32 attachment clamps to the viewport).
uint32_t Depth32To20e4(float depth);
// A kD24S8 source's depth packs as X :3593-3604, uint(RoundEven(depth * float(0xFFFFFF))): one float32 multiply (rounded
// to nearest) then round half to even. FM4 Recomp difference: depth is clamped to [0, 1] first (NaN -> 0), where X's
// OpConvertFToU of a negative or NaN product is undefined; identical inside [0, 1]. Not the exact inverse of
// ColorWordDepth: n in [2^22, 2^23) loses its lowest bit when the float32 product rounds to a .5 tie (0x600001 -> 0x600000).
uint32_t Depth32ToUnorm24(float depth);
uint32_t PackDepthStencilWord(float depth, uint32_t stencil, edram::DepthFormat format = edram::DepthFormat::D24FS8);
// The other way (ColorToDepth): the depth a colour word becomes in a kD24FS8 target, Float20e4To32(word >> 8) (X
// spirv_shader_translator_rb.cc:328+ Depth20e4To32 with remap_to_0_to_0_5 false: this renderer stores guest depth
// unhalved, edram_vk_plan.h DecodeDepthClear; X passes true at vulkan_render_target_cache.cc:4121-4123), and its
// stencil, word & 0xFF (X :4145-4152, :4157-4170). In a kD24S8 target the depth is X's unorm24 conversion
// (vulkan_render_target_cache.cc:4102-4119): float(n + (n >> 23)) * 2^-24 for n = word >> 8, not n / 0xFFFFFF ("Multiplying by
// 1.0 / 0xFFFFFF produces an incorrect result (for 0xC00000 ...)"), = UNorm24To32 (X xenos.h:432-438). Exact in float32
// (an integer <= 2^24 times a power of two), so the D32_SFLOAT_S8_UINT stand-in holds it bit for bit.
float ColorWordDepth(uint32_t word, edram::DepthFormat format = edram::DepthFormat::D24FS8);
// FM4 Recomp: the depth a depth -> depth transfer between formats writes (X kDepthToDepth: the source depth packed to the
// guest's 24 bits by its format, :3591-3604, then read by the destination's, :4100-4124): ColorWordDepth(PackDepthStencilWord(
// depth, 0, from), to); the same format returns `depth` unchanged (X :3587-3590). kD24FS8 -> kD24S8 is exact against X;
// kD24S8 -> kD24FS8 differs from X only below the 20e4 quantum (no host-depth owner, edram.cpp Ownership::Change).
float ConvertDepthFormat(float depth, edram::DepthFormat from, edram::DepthFormat to);
constexpr uint32_t ColorWordStencil(uint32_t word) { return word & 0xFF; }

// The source texel of a destination sample (X vulkan_render_target_cache.cc:2597-2601 "First sample bit at 4x
// with Vulkan standard locations - horizontal sample. Second sample bit - vertical sample"; same bpp 4x -> 1x
// :2804-2818, 1x -> 4x :2821-2829 and :2857-2864). Equal bases and pitches, so tile-relative and absolute
// pixel coordinates map the same way (a tile row is 80 samples: 80 1x pixels or 40 4x pixels).
struct SourceTexel
{
    uint32_t x = 0, y = 0, sample = 0;
};
SourceTexel MapTransferTexel(uint32_t destX, uint32_t destY, uint32_t destSample, edram::Msaa source, edram::Msaa dest);

// The source texel of a depth -> colour destination sample, X's full mapping (vulkan_render_target_cache.cc:2516-2577
// the destination pixel split into its 32bpp tile and tile-relative pixel; :2857-2864 and :2823-2828 the 1x -> 4x
// samples-to-pixels rule on the tile-relative pixel; :2916-2936 the 40-sample column swap between colour and depth,
// half = 80 >> (1 + source 4x); :2938-2954 the source tile = destination tile + (destination base - source base),
// modulo 2048). The same layout as X shaders/edram.xesli:23-59 XeEdramOffsetInts (a depth tile's x moved by half the
// tile width). Any pitches, as MapEdramTexel. The GLSL kTransferDepthToColor (XeSourceTexel) is the same.
SourceTexel MapCrossAspectTexel(uint32_t destX, uint32_t destY, uint32_t destSample, edram::Key source, edram::Key dest);

// Every transfer's source texel (MapCrossAspectTexel is this): the same tile split, MSAA rule and tile offset for all
// modes (X vulkan_render_target_cache.cc:2516-2577, :2779-2864, :2938-2954: one path whatever TransferMode, the
// address push constant carrying the destination-relative source tile offset), the 40-sample column swap only between
// colour and depth (:2916). With equal bases it equals MapTransferTexel (80x16 / 40x8 tiles keep the parity of x and y).
// The destination tile index splits by the destination's pitch (X :2516-2577), the source tile index by the source's
// (:2956-2987). The GLSL XeSourceTexel (edram_vk.cpp) is the same arithmetic.
// FM4 Recomp (M9, FM4_INTERNAL_RES): `halves` is the internal scale in halves (render_scale.h: 2 = 1x, 3 = 1.5x, 4 = 2x): coordinates
// are host pixels of images halves / 2 times the keys' guest size, a tile as many times the host pixels on both axes (the shader's
// XE_HALVES), each host pixel with the
// key's own sample count; the MSAA rules run on the scaled tile pixel (X draw_resolution_scale, ShiftGlue 72b7dd6
// vulkan/render_target_cache.cpp:2567-2600). Each pair stays a bijection of host samples within a tile.
// `sourceHalves` (0: the same as `halves`, the destination's): FM4_INTERNAL_SCALE_PITCH, a source image at another scale. The
// destination's tile pixel is first taken to the source's scale (floor(p * sourceHalves / halves): the nearest pixel at the lower
// resolution), then mapped as between two images of the source's scale.
SourceTexel MapEdramTexel(uint32_t destX, uint32_t destY, uint32_t destSample, edram::Key source, edram::Key dest, uint32_t halves = 2, uint32_t sourceHalves = 0);

// FM4 Recomp (alpha, RT format 5): MapEdramTexel with 64 bpp keys (storage 5). A 64 bpp tile is 40x16 samples of 8 bytes (X
// vulkan_render_target_cache.cc:2540-2545 tile width >> dest_is_64bpp; :2912-2913 and :2966-2987 the source's, >> source_is_64bpp;
// :5554-5556 "treating 64bpp tiles as storing 40x16 samples"), a key's tiles a row PitchTiles() (doubled, X :4823, :5223-5224). Each
// 64 bpp sample is two 32-bit EDRAM words, low then high, in the dwords a 32 bpp key sees as two horizontally adjacent samples (a 4x
// source, host sample bit 0) or pixels (1x/2x); X's rules per pair, :2607-2685 (32 -> 64), :2686-2777 (64 -> 32), :2833-2903 (the
// common Y of a 1x/2x source):
//   - `first`: the source texel (32 -> 64: of the destination's low word); `second`: 32 -> 64, the texel of its high word (X :3005-3023:
//     sample | 1 from a 4x source, x | 1 otherwise), else equal to `first`;
//   - `half`: 64 -> 32, the half of the source sample that is the destination's word (X :2714-2717 the destination's horizontal
//     sample at 4x, :2771-2774 its horizontal pixel otherwise; :3080-3102 the pick), else 0.
// For two 32 bpp keys (or two 64 bpp keys) `first` is MapEdramTexel's former result, `second` == `first`, `half` 0. A cross-aspect
// pair with a 64 bpp side (a 64 bpp colour source into depth, a depth source into 64 bpp colour) takes the column swap after the
// MSAA rule at the source's width (X :2912-2936: 20 pixels for a 4x depth source, 10 for a 4x 64 bpp colour one), before the tile
// offset; `second` keeps `first`'s parity (the swap and tile widths are even). The GLSL XeSourceTexel with XE_SOURCE_64 / XE_DEST_64
// (edram_vk.cpp kTransferCommon), kTransferColor64, kTransferColorToDepth (XE_SOURCE_64) and kTransferDepthToColor (XE_DEST_64) are
// the same arithmetic.
struct TransferSource
{
    SourceTexel first, second;
    uint32_t half = 0;
};
TransferSource MapTransferSource(uint32_t destX, uint32_t destY, uint32_t destSample, edram::Key source, edram::Key dest, uint32_t halves = 2, uint32_t sourceHalves = 0);
// FM4 Recomp (alpha, RT format 5): the 32-bit guest word of half `half` of a 64 bpp 16_16_16_16 sample read through its
// R16G16B16A16_UINT view (X :3080-3102 the half's two components, :3545-3576 the first the low 16 bits): half 0 r | g << 16, half 1
// b | a << 16. And the sample from its two words (X :3352-3370 "unpack two packed 32-bit parts"): r, g the low word's halves, b, a
// the high word's. A 64 -> 64 transfer copies the R16G16B16A16_UINT texel itself (X :3254-3273), the same as both composed.
uint32_t TransferWordOf64(const uint32_t texel[4], uint32_t half);
void TransferTexelOf64(uint32_t low, uint32_t high, uint32_t texel[4]);

// The 32-bit guest word of a colour as a format-changing transfer packs it (X :3402-3449 for 8_8_8_8:
// uint(v * 255 + 0.5) per component, R in bits 0-7; :3467-3490 for 2_10_10_10: rgb * 1023, a * 3, R in bits 0-9;
// :3497-3542 for 7e3: R, G, B through Float32To7e3 at bits 0, 10, 20, alpha uint(NClamp(a, 0, 1) * 3 + 0.5) at bit 30),
// and its inverse (X :3627-3690: component / 255, / 1023, alpha / 3; 7e3 R, G, B through Float7e3To32). `storage` 0,
// 2 or 3. The GLSL in edram_vk.cpp kTransferColor is the same arithmetic.
uint32_t PackTransferColor(const float rgba[4], uint32_t storage);
void UnpackTransferColor(uint32_t word, uint32_t storage, float rgba[4]);

// The transfer view of a colour storage format whose transfers and clears move raw integers (X
// vulkan_render_target_cache.cc:1628-1653 GetColorOwnershipTransferVulkanFormat: "Floating-point numbers have NaNs ... and
// SNORM has two representations of -1"): storage 14 (32_FLOAT) -> R32_UINT, storage 4 (16_16) -> R16G16_UINT; the target is
// created MUTABLE_FORMAT with that view (edram_vk.cpp). VK_FORMAT_UNDEFINED for the rest (they transfer as floats).
// FM4 Recomp (alpha, RT format 5): storage 5 (16_16_16_16, host R16G16B16A16_SNORM) -> R16G16B16A16_UINT (X :1640-1642), a 64-bit
// view of a 64-bit format (one Vulkan compatibility class). PackTransferUint / UnpackTransferUint stay 32 bpp (0 for storage 5):
// a 64 bpp texel is two words (TransferWordOf64, TransferTexelOf64).
VkFormat TransferViewFormat(edram::ColorFormat storage);
// The 32-bit guest word of a texel read through a transfer view, and back (the GLSL XePackUint / XeUnpackUint in
// edram_vk.cpp kTransferPacking are the same): storage 14 the R32_UINT texel itself (X :3579-3585, :3713-3726); storage 4
// the R16G16_UINT halves, word = r | g << 16 (X :3567-3574 BitFieldInsert(r, g, 16, 16)) and r = word & 0xFFFF, g = word
// >> 16 (X :3701-3711 BitFieldUExtract at 0 and 16, width 16); 4 -> 4 passes the halves through (X :3556-3566). The other
// texel components are 0. 0 / {0, 0, 0, 0} for a storage without a transfer view.
uint32_t PackTransferUint(const uint32_t texel[4], uint32_t storage);
void UnpackTransferUint(uint32_t word, uint32_t storage, uint32_t texel[4]);

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
// see above), alpha bits 30-31 / 3. Every value is exact in R16G16B16A16_SFLOAT. False for another storage format (4 and
// 14 clear through DecodeColorClearUint).
bool DecodeColorClear(uint32_t value, uint32_t storage, float rgba[4]);
// The clears through a transfer view (TransferViewFormat): storage 14 (32_FLOAT) the raw word in uint32[0] (X
// vulkan_render_target_cache.cc:5417-5421 "Using uint for proper denormal and NaN handling"); storage 4 (16_16) the raw
// halves, uint32[j] = (value >> 16j) & 0xFFFF (X :5393-5404, so a 0x8000 half survives); false for every other storage.
bool DecodeColorClearUint(uint32_t value, uint32_t storage, uint32_t rgba[4]);
// FM4 Recomp (alpha, 64 bpp resolve clear): storage 5 (16_16_16_16) through its R16G16B16A16_UINT transfer view: X
// render_target_cache.cc:1202-1205 clear value = RB_COLOR_CLEAR | RB_COLOR_CLEAR_LO << 32, vulkan_render_target_cache.cc:5405-5415
// uint32[j] = (clear_value >> 16j) & 0xFFFF. False for every other storage.
bool DecodeColorClearUint64(uint32_t value, uint32_t valueLo, uint32_t storage, uint32_t rgba[4]);
// X :5346-5363 without X's [0, 2) -> [0, 1) halving of 20e4 depth: unit 8's viewport does not halve (FoldViewport,
// draw_state.h) and edram::PrepareResolve refuses a 20e4 clear above 1.0 instead.
// kD24S8: X xenos.h:432-438 UNorm24To32((v >> 8) & 0xFFFFFF); kD24FS8: X xenos.cc:150-166 Float20e4To32; the
// stencil = v & 0xFF.
void DecodeDepthClear(uint32_t value, edram::DepthFormat format, float& depth, uint32_t& stencil);
float UNorm24To32(uint32_t n24);     // X xenos.h:432-438
float Float20e4To32(uint32_t f24);   // X xenos.cc:150-166

// ---- Clear-value ownership (FM4 Recomp, M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP; edram.h Ownership::SetClearValue) -----------
//
// A `value` Transfer (edram.h Transfer::value) moves a whole-tile constant between two keys of the same aspect and format without a
// copy: the destination gets a pending clear of the range with the guest word decoded for it, exactly as a resolve clear of that word
// on the destination would be (edram_vk.cpp Clear). Exactness per pair: the source held the
// clear DecodeColorClear / DecodeDepthClear(word, format) (a resolve clear's own value; a clear draw's value is recorded only when
// ClearRequestWord's round trip reproduces it bit for bit), and a same-format transfer copies the host value verbatim
// (edram_vk.cpp kTransferColor: `sourceFormat == destFormat ? v : ...`; the stencil-export depth transfer writes the source float32
// depth and stencil byte), so the destination holds the same clear value whether copied or cleared; a constant is the same for every
// sample, so the MSAA pair does not matter (4x -> 2x: every destination sample reads a source sample of the constant).

// The pending clear `out` of `dest` for a value transfer of `word` (ClearRun word) over `rect` (dest's pixels, a RangeRectangles
// rectangle of the range): colour storage 0, 2 or 3 as the float clear DecodeColorClear gives, depth as DecodeDepthClear by dest's
// depth format (depth + stencil aspects). False for a storage cleared through its transfer view (4, 14: the renderer's draw pass
// records pending clears through the typed view; left to the ordinary copy) and for anything else.
bool ValueClear(edram::Key dest, uint32_t word, const edram::Rect& rect, PendingClear& out);
// The guest word a clear-class draw's values (render_hooks.h ClearRequest) stand for on `key` (colour: PackTransferColor of `rgba`
// in key's storage format; depth: PackDepthStencilWord of `depthValue` and `stencil` in key's depth format), accepted only when
// decoding the word for key (DecodeColorClear / DecodeDepthClear: what a value transfer or a resolve clear would clear with)
// reproduces the request's floats bit for bit and its stencil: the host clear of the request and the clear of the word are then
// the same clear. Values outside the format's range (NaN, negative, above 1 for a UNORM, above 31.875 for 7e3) are refused before
// packing. False: the tiles are not recorded as constants and keep today's copy path.
bool ClearRequestWord(const ClearRequest& request, edram::Key key, uint32_t& word);

// FM4 Recomp (M7 GPU cost, opt-in FM4_RESOLVE_COMPUTE=1): the resident resolve's fragment shader
// (edram_vk.cpp kResolveResident) as a compute shader with the same conversion, operation for operation: its colour output becomes a
// word written to an r32ui storage image (set 1, binding 0) at the invocation's pixel, and gl_FragCoord the invocation's id; 8x8
// invocations a group, those outside width x height write nothing. Each replaced text must occur exactly once in `fragment`, else false
// with `why` naming it (the fragment shader changed: the compute path must be revisited, never drift).
constexpr uint32_t kResolveComputeGroup = 8;
bool ResolveComputeSource(const std::string& fragment, std::string& out, std::string& why);

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

// ---- The full (converting) resolve: X draw_util.cc:1305-1345 GetCopyShader, shaders/resolve_full_32bpp.xesli ----------
//
// X takes its fast (bit-copy) shader only when there is no exponent bias, one sample is selected and the formats are
// bitwise equivalent (draw_util.cc:1312-1318); everything else goes through the full shader: per sample unpack the
// EDRAM word to float (resolve.xesli:273-321 XeResolveUnpack32bpp4Samples: 8_8_8_8 / 2_10_10_10 UNORM, 7e3 through
// pixel_formats.xesli:328-349 XeUnpackR10G10B10A2Float, 16_16 through :367-371 XeUnpackR16G16Edram), sum the selected samples (resolve.xesli:632-694: the first
// sample XeResolveFirstSampleIndex :206-216; k01 / k23 add the next sample row (+80 ints: the guest sample with y + 1),
// k0123 also the next sample column (+1 int) and its next row), multiply by dest_exp_bias_factor = 2^bias (:131-133)
// halved once for k01 / k23 and twice for k0123 (:651, :670), swap red and blue (.bgra, :699-704), pack to the
// destination format (pixel_formats.xesli:200-256 XePack32bpp4Pixels: :116-125 saturate, * 255 or 1023 / 3, + 0.5,
// truncate; k_16_16 :236-239 red and green * 65535), endian swap (resolve_full_32bpp.xesli:62-65), store at the tiled address (resolve.xesli:161-184).

// FM4 Recomp's fast path (ResolveToGuest): a 1x source, sample k0, no bias, bitwise-equivalent formats.
bool ResolveUsesFastPath(const edram::ResolveInfo& info, uint32_t sourceStorage);

// The guest sample index of host sample `host` of an image of `msaa` (and its inverse, the same function): 4x: X's host
// sample bit 0 is horizontal, bit 1 vertical (vulkan_render_target_cache.cc:2597-2598), the guest's bit 1 horizontal,
// bit 0 vertical (shaders/edram.xesli:27-33) -> guest = (host & 1) << 1 | host >> 1; native 2x: "top is 1 in Vulkan,
// bottom is 0" (:2599-2600) and guest sample 0 is the top (edram.xesli:32-33) -> guest = host ^ 1; 1x: 0.
uint32_t GuestSampleOfHost(edram::Msaa msaa, uint32_t host);

// The sample dump's layout (edram_vk.cpp kResolveDump, then vkCmdCopyImageToBuffer, tightly packed): the rectangle's
// `width` x `height` pixels, guest sample g of pixel (x, y) at word (y + height * (g >> 1)) * (width * columns) +
// x + width * (g & 1); columns = 2 for 2x and 4x (else 1), rows = 2 for 4x (else 1). A 1x copy of a storage 0 / 2 / 4 / 14 host
// image (whose words are the guest's; storage 4: R16G16_SNORM's raw halves, R | G << 16 little-endian) has the same layout.
struct DumpLayout
{
    uint32_t width = 0, height = 0, samples = 1, columns = 1, rows = 1;
    constexpr size_t Word(uint32_t x, uint32_t y, uint32_t g) const
    {
        return (size_t(y) + size_t(height) * (g >> 1)) * (size_t(width) * columns) + x + size_t(width) * (g & 1);
    }
};
DumpLayout DumpLayoutFor(edram::Msaa msaa, uint32_t width, uint32_t height);

// The isolated GPU check of the 4x -> 2x depth transfer (renderer.cpp RunSelfCheck with FM4_SELFCHECK_EDRAM=depth4x2x):
// the 1x kD24FS8 key at base 1024, 16 tiles a row, holds at pixel (x, row) a known depth (k / 8192, exact in 20e4) and
// stencil; it is transferred into the 4x key 708400 and from there into the 2x key 688400 (same base and pitch, so the
// same EDRAM samples). The expectations come from the EDRAM sample layout alone (X shaders/edram.xesli:23-33: a 1x pixel
// is one EDRAM sample, 4x pixel (x, y) host sample h is (2x + (h & 1), 2y + (h >> 1)), 2x pixel (x, y) guest sample g is
// (x, 2y + g)), not from MapTransferTexel: each guest sample's word (PackDepthStencilWord, kD24FS8) as the depth dump reads it.
float DepthSelfCheckDepth(uint32_t x, uint32_t row);
uint32_t DepthSelfCheckStencil(uint32_t x, uint32_t row);
uint32_t DepthSelfCheckWord(uint32_t x, uint32_t row);  // the 1x pixel's word
uint32_t DepthSelfCheckExpected4x(uint32_t x, uint32_t y, uint32_t guestSample);
uint32_t DepthSelfCheckExpected2x(uint32_t x, uint32_t y, uint32_t guestSample);
// FM4 Recomp: the 1x key's pixel after kD24FS8 -> kD24S8 (dumped as kD24S8) and after the way back (dumped as kD24FS8).
uint32_t DepthSelfCheckExpectedS8(uint32_t x, uint32_t row);
uint32_t DepthSelfCheckExpectedRoundTrip(uint32_t x, uint32_t row);

// The guest samples the resolve reads and how they are summed (resolve.xesli:206-216, :632-694); false for a sample the
// source does not have (DecodeResolve sanitizes, draw_util.cc:495-545). `count` 1, 2 or 4, `indices` in summation order.
bool ResolveSamples(const edram::ResolveInfo& info, uint32_t indices[4], uint32_t& count);

// One destination word (before the endian swap) from the guest words of one pixel's samples, indexed by guest sample.
uint32_t ResolveConvertPixel(const edram::ResolveInfo& info, uint32_t sourceStorage, const uint32_t* samplesByGuestIndex);
// FM4 Recomp (alpha, 64 bpp resolve): the two destination words (before the endian swap), low then high, of a 1x 16_16_16_16 pixel
// resolved into k_16_16_16_16_FLOAT (32): the sample's EDRAM words `low` (r | g << 16) and `high` (b | a << 16), each component
// int16 * 32 / 32767 clamped at -32 (as the 16_16 unpack), times 2^destExpBias, red and blue exchanged when destSwap, each packed to
// IEEE binary16 rounding to nearest even (APPROXIMATE, edram_vk_plan.cpp PackHalf): out[0] = half(r) | half(g) << 16, out[1] =
// half(b) | half(a) << 16 (X resolve.xesli:368-389, :131-133, :699-704; pixel_formats.xesli:269-277, :373-377).
void ResolveConvertPixel64(const edram::ResolveInfo& info, uint32_t low, uint32_t high, uint32_t out[2]);

// The checks of the full path that do not need the words: storage 0, 2, 3 or 14 into destination format 6, 7, 36, 50, 54,
// storage 4 (16_16) into 25 (k_16_16) only (the 32bpp formats DecodeResolve accepts; XePack32bpp4Pixels :205-252); storage
// 0, 2 or 3 into 10 (k_8_8, 16 bpp: XePack16bpp4PixelsInUInt2 :177-180, endian 0 or 8in16); the selected samples exist; a
// non-empty rectangle whose base is its extent's start. Refusals name the copy's shape.
// FM4 Recomp: a 16 bpp destination (info.destBytesLog2 1) is stored 2 bytes a texel: ResolveConvertPixel's low 16 bits,
// XeEndianSwap16 (resolve_full_16bpp.xesli:61-63, texture_untile.h EndianSwap16Pair), at TiledOffset2D(destOffsetX + x,
// destOffsetY + y, destPitch, 1).
// FM4 Recomp (alpha, 64 bpp resolve): storage 5 (16_16_16_16) into 32 (k_16_16_16_16_FLOAT) only, 1x sample 0, number 7, endian 0
// or 8in16 (info.destBytesLog2 3): `words` then holds two words a pixel, row-major (the 1x source's R16G16B16A16 texels copied to the
// buffer), and each pixel's ResolveConvertPixel64 words, EndianSwap16Pair each, are stored low word first at TiledOffset2D(destOffsetX
// + x, destOffsetY + y, destPitch, 3), 8 bytes.
bool ResolveFullConvertible(const edram::ResolveInfo& info, uint32_t sourceStorage, std::string& why);
// Every pixel of the rectangle from `words` (DumpLayoutFor(info.msaa, ...)) into `dest` as ResolveToGuest does; nothing
// written on a refusal.
bool ResolveConvertToGuest(const edram::ResolveInfo& info, uint32_t sourceStorage, const uint32_t* words, uint8_t* dest, uint32_t destBytes,
                           std::string& why);
// FM4 Recomp (FM4_RESOLVE_GPU_WORDS): `words` are the rectangle's converted 32 bpp words, row-major and tightly packed, as
// ResolveConvertPixel computes them (edram_vk.cpp kResolveResident on the GPU); each is endian-swapped and stored at its tiled
// address exactly as ResolveConvertToGuest stores ResolveConvertPixel's result. Nothing written on a refusal.
// FM4 Recomp (M7 CPU cost, FM4_RESOLVE_GPU_WORDS16): for a 16 bpp destination (k_8_8, info.destBytesLog2 1) `words` holds the
// R16_UINT resident image's halfwords (ResolveConvertPixel's low 16 bits), tightly packed; each is stored as ResolveConvertToGuest's
// 16 bpp branch stores it (EndianSwap16Pair, TiledOffset2D(..., 1), 2 bytes).
// FM4 Recomp: the bounds check of a tiled 32-bit store in runs of 4 texels (edram_vk_plan.cpp StoreTiledRow32); false with `why`
// naming the first texel past `destBytes`.
bool TiledRunsFit(uint32_t destOffsetX, uint32_t destOffsetY, uint32_t destPitch, uint32_t width, uint32_t height, uint32_t destBytes, std::string& why);
// FM4 Recomp (M7 CPU cost): the tile-major store's run table (edram_vk_plan.cpp StoreTiledRows32, the layout proof above it). The two
// rows ye (even) and ye + 1 of a 32x32 tile at 32 bpp fill one contiguous 256-byte stripe at in-tile offset (ye & 30) << 7; its run r
// (16 bytes, words 4r-4r+3) holds the four consecutive texels [kTiledStripeRunX[(ye >> 3) & 1][r], + 4) of the row with parity r & 1.
// This is the tile's 1,024-entry inverse map (stripe word -> texel) factored by that structure. tests/runtime/test_edram_vk.cpp
// TestTiledStripes checks every derived entry against Xenia's GetTiledOffset2D, with a shifted entry as the control.
constexpr uint8_t kTiledStripeRunX[2][16] = {{0, 0, 4, 4, 8, 8, 12, 12, 16, 16, 20, 20, 24, 24, 28, 28},
                                              {16, 16, 20, 20, 24, 24, 28, 28, 0, 0, 4, 4, 8, 8, 12, 12}};
bool ResolveWordsToGuest(const edram::ResolveInfo& info, uint32_t sourceStorage, const uint32_t* words, uint8_t* dest, uint32_t destBytes,
                         std::string& why);

// ---- Depth resolves (X draw_util.cc GetResolveInfo's depth path and GetCopyShader: always the fast bit copy) ----------
//
// `words`: the sample dump of the depth target (edram_vk.cpp kResolveDumpDepth, DumpLayoutFor(info.msaa, ...)), each the
// guest's EDRAM word of a sample, PackDepthStencilWord(depth, stencil, format) (X's EDRAM layout of depth, the same packing
// as its depth -> colour transfer, vulkan_render_target_cache.cc:3591-3617). The resolve writes the one selected sample's
// word (ResolveSamples: one sample for depth, DecodeResolve's sanitization), endian swapped (X
// shaders/resolve_fast_32bpp_1x2xmsaa.xesli:67-69, resolve_fast_32bpp_4xmsaa.xesli:84-92 XeEndianSwap32; no red/blue swap,
// draw_util.cc :1271-1274), at TiledOffset(destOffsetX + x, destOffsetY + y, destPitch, 2), as ResolveToGuest.
bool ResolveDepthConvertible(const edram::ResolveInfo& info, std::string& why);
bool ResolveDepthToGuest(const edram::ResolveInfo& info, const uint32_t* words, uint8_t* dest, uint32_t destBytes, std::string& why);

}  // namespace runtime::video
