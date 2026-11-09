// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// See edram_vk.h. X = xenia-canary 67f16c4e315f src/xenia/gpu (BSD-3-Clause): the transfer design is
// vulkan/vulkan_render_target_cache.cc's (one draw per destination with the source sampled by texelFetch,
// :2479-2829 coordinates, :3387-3668 packing; the per-sample fallback with sample masks when sampleRateShading is
// not used, :2497-2502; stencil written bit by bit with the stencil cleared first, :4975-5011 and the kStencilBit
// output, when VK_EXT_shader_stencil_export is not used), re-derived as GLSL here; the resolve clear is
// :5340-5431. Nothing here decides ownership: edram.cpp does, this file executes its outputs.
#include "edram_vk.h"
#include "edram.h"
#include "edram_vk_plan.h"
#include "render_targets_vk.h"
#include "resolve_resident.h"
#include "kernel/graphics.h"
#include "kernel/virtual_memory.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

namespace runtime::video {
namespace {

using edram::Key;
using edram::Msaa;
using edram::Transfer;

constexpr VkPipelineStageFlags kAttachmentStages =
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags kAttachmentAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
constexpr VkDeviceSize kStagingBytes = 16ull << 20;  // 4 Mi texels: a 2048x2048 resolve; FM4's are 1280x720 (3.5 MiB)
// FM4 Recomp (FM4_RESOLVE_RESIDENT): the staging buffer holds several pending resolves, each up to kStagingBytes of read-back
// plus (FM4_RESOLVE_RESIDENT_CHECK) its GPU words; another resolve is recorded only while two kStagingBytes still fit (resolveRoom).
constexpr VkDeviceSize kStagingTotalBytes = 4 * kStagingBytes;
// Descriptor sets of the pool. Xenia keeps every render target until an explicit ClearCache (X render_target_cache.cc:534-570,
// :1467); FM4's race-prep frame creates 32 keys (~580 MiB at 8 bytes a sample), so 64 leaves room.
constexpr uint32_t kMaxTargets = 64;

// ---- The built-in transfer shaders (compiled once at InitializeEdramVk) ------------------------------------------

// A fullscreen triangle: vertices (-1, -1), (3, -1), (-1, 3); the scissor limits it to one transfer rectangle.
const char* const kTransferVertex = R"(#version 450
void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Push constants of every transfer draw (struct TransferPush below).
const char* const kTransferCommon = R"(
layout(push_constant) uniform XeTransfer
{
    uint destSamples4;  // 1: the destination is 4x (one draw per sample, sample mask 1 << destSample)
    uint destSample;
    uint sourceFormat;  // storage format 0 (8_8_8_8), 2 (2_10_10_10) or 3 (2_10_10_10_FLOAT, 7e3); a depth key's depth format
    uint destFormat;
    uint stencilBit;
    uint pitchTiles;    // the destination key's pitch in tiles at 32 bpp (X :2516-2577: it splits the destination tile index)
    uint tileOffset;    // (destination base - source base) & 2047
    uint sourceMsaa;    // Xenos msaa_samples: 0 1x, 1 2x, 2 4x
    uint destMsaa;
    uint x0, y0, width, height;  // the sample dump (kResolveDump): the source rectangle
    uint sourcePitchTiles;       // the source key's pitch in tiles (X :2956-2965: it splits the source tile index)
} xe_transfer;
// The pixel size of a 32bpp tile (80x16 samples): 1x 80x16, 2x 80x8, 4x 40x8 (X render_target_cache.cc:355-359).
uvec2 XeTileSize(uint msaa)
{
    return msaa == 2u ? uvec2(40u, 8u) : msaa == 1u ? uvec2(80u, 8u) : uvec2(80u, 16u);
}
// edram_vk_plan.cpp MapEdramTexel without the colour/depth column swap: X vulkan_render_target_cache.cc:2516-2577
// (destination tile and tile pixel); the MSAA rule on the tile pixel, MapTransferTexel (:2597-2601; 4x -> 1x :2804-2818;
// 1x -> 4x :2823-2828, :2857-2864; 4x -> 2x :2786-2802; 2x -> 4x :2823-2828, :2843-2849; 2x <-> 1x :2868-2898, native 2x host samples);
// :2938-2954 (source tile = destination tile + tileOffset, & 2047). xy the source pixel, z its host sample.
ivec3 XeSourceTexel(uvec2 d)
{
    uint sm = xe_transfer.sourceMsaa, dm = xe_transfer.destMsaa, ds = xe_transfer.destSample;
    uvec2 tile = d / XeTileSize(dm), p = d % XeTileSize(dm);
    uint s = dm == 0u ? 0u : ds;
    if (sm == 2u && dm == 0u)
    {
        s = (p.x & 1u) | ((p.y & 1u) << 1u);
        p >>= 1u;
    }
    else if (sm == 0u && dm == 2u)
    {
        p = (p << 1u) | uvec2(ds & 1u, (ds >> 1u) & 1u);
        s = 0u;
    }
    else if (sm == 2u && dm == 1u)
    {
        s = (p.x & 1u) | (((ds ^ 1u) & 1u) << 1u);
        p.x >>= 1u;
    }
    else if (sm == 1u && dm == 2u)
    {
        p.x = (p.x << 1u) | (ds & 1u);
        s = ((ds >> 1u) & 1u) ^ 1u;
    }
    else if (sm == 1u && dm == 0u)  // X :2868-2886
    {
        s = (p.y & 1u) ^ 1u;
        p.y >>= 1u;
    }
    else if (sm == 0u && dm == 1u)  // X :2887-2898
    {
        p.y = (p.y << 1u) | ((ds ^ 1u) & 1u);
        s = 0u;
    }
#ifdef XE_SWAP_COLUMNS
    // Colour <-> depth: the 40-sample column swap within the source tile (X :2916-2936), half = 80 >> (1 + source 4x).
    uint halfTile = sm == 2u ? 20u : 40u;
    p.x = p.x < halfTile ? p.x + halfTile : p.x - halfTile;
#endif
    uint t = (tile.y * xe_transfer.pitchTiles + tile.x + xe_transfer.tileOffset) & 2047u;
    return ivec3(ivec2(uvec2(t % xe_transfer.sourcePitchTiles, t / xe_transfer.sourcePitchTiles) * XeTileSize(sm) + p), int(s));
}
ivec2 XeSourcePixel(uvec2 d)
{
    return XeSourceTexel(d).xy;
}
int XeSourceSample(uvec2 d)
{
    return XeSourceTexel(d).z;
}
)";

// Colour packing: edram_vk_plan.cpp PackTransferColor / UnpackTransferColor (X :3402-3542, :3627-3690) and, for
// storage 3, Float32To7e3 / Float7e3To32 (X spirv_shader_translator_rb.cc:25-137 and :139-234). Shared by the colour
// and the depth -> colour shaders.
const char* const kTransferPacking = R"(
uint XeFloat32To7e3(float f)
{
    uint u = floatBitsToUint(f > 0.0 ? min(f, 31.875) : 0.0);
    uint b = u < 0x3E800000u ? ((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23u), 24u) : u - (124u << 23u);
    return ((b + 0x7FFFu + ((b >> 16u) & 1u)) >> 16u) & 0x3FFu;
}
float XeFloat7e3To32(uint f10)
{
    f10 &= 0x3FFu;
    if (f10 == 0u)
        return 0.0;
    uint m = f10 & 0x7Fu, e = f10 >> 7u;
    if (e == 0u)
    {
        uint msb = uint(findMSB(m));
        e = msb - 6u;
        m = (m << (7u - msb)) & 0x7Fu;
    }
    return uintBitsToFloat(((e + 124u) << 23u) | (m << 16u));
}
uint XePack(vec4 v, uint storage)
{
    if (storage == 14u)  // 32_FLOAT (transfers read its R32_UINT view instead: kTransferColor XE_SOURCE_UINT)
        return floatBitsToUint(v.r);
    if (storage == 0u)
    {
        uvec4 u = uvec4(v * 255.0 + 0.5) & 0xFFu;
        return u.r | (u.g << 8u) | (u.b << 16u) | (u.a << 24u);
    }
    if (storage == 3u)
        return XeFloat32To7e3(v.r) | (XeFloat32To7e3(v.g) << 10u) | (XeFloat32To7e3(v.b) << 20u) |
               ((uint((v.a > 0.0 ? min(v.a, 1.0) : 0.0) * 3.0 + 0.5) & 3u) << 30u);
    uvec4 u = uvec4(v * vec4(1023.0, 1023.0, 1023.0, 3.0) + 0.5) & uvec4(0x3FFu, 0x3FFu, 0x3FFu, 3u);
    return u.r | (u.g << 10u) | (u.b << 20u) | (u.a << 30u);
}
vec4 XeUnpack(uint w, uint storage)
{
    if (storage == 14u)  // 32_FLOAT (transfers write its R32_UINT view instead: kTransferColor XE_DEST_UINT)
        return vec4(uintBitsToFloat(w), 0.0, 0.0, 0.0);
    if (storage == 0u)
        return vec4((uvec4(w) >> uvec4(0u, 8u, 16u, 24u)) & 0xFFu) * (1.0 / 255.0);
    if (storage == 3u)
        return vec4(XeFloat7e3To32(w), XeFloat7e3To32(w >> 10u), XeFloat7e3To32(w >> 20u), float(w >> 30u) * (1.0 / 3.0));
    return vec4((uvec4(w) >> uvec4(0u, 10u, 20u, 30u)) & uvec4(0x3FFu, 0x3FFu, 0x3FFu, 3u)) * vec4(1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 3.0);
}
// A texel of a transfer view (edram_vk_plan.cpp PackTransferUint / UnpackTransferUint): storage 4 (16_16, R16G16_UINT) the
// halves, word = r | g << 16 (X :3567-3574), r = word & 0xFFFF, g = word >> 16 (X :3701-3711); storage 14 (32_FLOAT,
// R32_UINT) the word itself (X :3579-3585, :3713-3726). A uvec4 output: an R32_UINT attachment drops the extra components.
uint XePackUint(uvec4 t, uint storage)
{
    return storage == 4u ? (t.r & 0xFFFFu) | ((t.g & 0xFFFFu) << 16u) : t.r;
}
uvec4 XeUnpackUint(uint w, uint storage)
{
    return storage == 4u ? uvec4(w & 0xFFFFu, w >> 16u, 0u, 0u) : uvec4(w, 0u, 0u, 0u);
}
)";

// A 32_FLOAT (storage 14) or 16_16 (storage 4) side goes through its transfer view, R32_UINT or R16G16_UINT (X
// GetColorOwnershipTransferVulkanFormat :1628-1653, "Float32 is transferred as uint32 to preserve NaN encodings", :3545-3576,
// :3692-3726): XE_SOURCE_UINT reads the texel and packs the word (XePackUint), XE_DEST_UINT writes it (XeUnpackUint).
const char* const kTransferColor = R"(
#ifdef XE_SOURCE_UINT
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform usampler2DMS xe_source;
#else
layout(set = 0, binding = 0) uniform usampler2D xe_source;
#endif
#else
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform sampler2DMS xe_source;
#else
layout(set = 0, binding = 0) uniform sampler2D xe_source;
#endif
#endif
#ifdef XE_DEST_UINT
layout(location = 0) out uvec4 xe_word;
#else
layout(location = 0) out vec4 xe_color;
#endif
void main()
{
    uvec2 d = uvec2(gl_FragCoord.xy);
#ifdef XE_SOURCE_MS
    int s = XeSourceSample(d);
#else
    int s = 0;
#endif
#ifdef XE_SOURCE_UINT
    uint word = XePackUint(texelFetch(xe_source, XeSourcePixel(d), s), xe_transfer.sourceFormat);
#else
    vec4 v = texelFetch(xe_source, XeSourcePixel(d), s);
    uint word = XePack(v, xe_transfer.sourceFormat);
#endif
#ifdef XE_DEST_UINT
    xe_word = XeUnpackUint(word, xe_transfer.destFormat);
#elif defined(XE_SOURCE_UINT)
    xe_color = XeUnpack(word, xe_transfer.destFormat);
#else
    xe_color = xe_transfer.sourceFormat == xe_transfer.destFormat ? v : XeUnpack(word, xe_transfer.destFormat);
#endif
}
)";

// The depth of a guest word's bits 8-31 by a depth format (0 kD24S8: X :4102-4119 unorm24, (n + (n >> 23)) * 2^-24; 1 kD24FS8:
// Depth20e4To32 without the [0, 0.5) remap, :4120-4124; edram_vk_plan.h ColorWordDepth). Shared by colour -> depth and the
// depth transfer between formats.
const char* const kDepthUnpacking = R"(
float XeDepth20e4To32(uint f24)
{
    f24 &= 0xFFFFFFu;
    if (f24 == 0u)
        return 0.0;
    uint m = f24 & 0xFFFFFu, e = f24 >> 20u;
    if (e == 0u)
    {
        uint shift = 20u - uint(findMSB(m));
        e = 1u - shift;
        m = (m << shift) & 0xFFFFFu;
    }
    return uintBitsToFloat(((e + 112u) << 23u) | (m << 3u));
}
float XeWordDepth(uint word, uint depthFormat)
{
    uint n = word >> 8u;
    return depthFormat == 0u ? float(n + (n >> 23u)) * (1.0 / 16777216.0) : XeDepth20e4To32(n);
}
)";

// Depth -> depth (X kDepthToDepth); a multisampled (2x, 4x) source with XE_SOURCE_MS. The same format: the float copied as
// is (X :3587-3590). FM4 Recomp: another format: the source depth packed to the guest's 24 bits by its format and those
// bits read by the destination's (X :3591-3604 then :4100-4124; edram_vk_plan.h ConvertDepthFormat).
const char* const kTransferDepth = R"(
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform sampler2DMS xe_source_depth;
#else
layout(set = 0, binding = 0) uniform sampler2D xe_source_depth;
#endif
void main()
{
    uvec2 d = uvec2(gl_FragCoord.xy);
#ifdef XE_SOURCE_MS
    float depth = texelFetch(xe_source_depth, XeSourcePixel(d), XeSourceSample(d)).r;
#else
    float depth = texelFetch(xe_source_depth, XeSourcePixel(d), 0).r;
#endif
    gl_FragDepth = xe_transfer.sourceFormat == xe_transfer.destFormat
                       ? depth
                       : XeWordDepth(XeDepthStencilWord(depth, 0u, xe_transfer.sourceFormat), xe_transfer.destFormat);
}
)";

// One stencil bit (X kStencilBit, :4156-4180): the pipeline replaces with reference 0xFF under write mask
// 1 << stencilBit; a fragment whose source bit is clear is discarded.
const char* const kTransferStencilBit = R"(
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 1) uniform usampler2DMS xe_source_stencil;
#else
layout(set = 0, binding = 1) uniform usampler2D xe_source_stencil;
#endif
void main()
{
    uvec2 d = uvec2(gl_FragCoord.xy);
#ifdef XE_SOURCE_MS
    uint stencil = texelFetch(xe_source_stencil, XeSourcePixel(d), XeSourceSample(d)).r;
#else
    uint stencil = texelFetch(xe_source_stencil, XeSourcePixel(d), 0).r;
#endif
    if (((stencil >> xe_transfer.stencilBit) & 1u) == 0u)
        discard;
}
)";

// Colour -> depth (X kColorToDepth, kColorToStencilBit; :3385-3586 the colour packed to the guest word, :3733-4155 its
// bits 8-31 as 20e4 depth, Depth20e4To32 without the [0, 0.5) remap (edram_vk_plan.h ColorWordDepth); :4156-4180 one
// stencil bit of bits 0-7). Compiled with XE_SWAP_COLUMNS (X :2916-2936); XE_STENCIL_BIT selects the stencil pass;
// XE_SOURCE_UINT reads a 16_16 source through its R16G16_UINT view (X :3567-3574 the halves as the word).
const char* const kTransferColorToDepth = R"(
#ifdef XE_SOURCE_UINT
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform usampler2DMS xe_source;
#else
layout(set = 0, binding = 0) uniform usampler2D xe_source;
#endif
#else
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform sampler2DMS xe_source;
#else
layout(set = 0, binding = 0) uniform sampler2D xe_source;
#endif
#endif
void main()
{
    uvec2 d = uvec2(gl_FragCoord.xy);
#ifdef XE_SOURCE_MS
    int s = XeSourceSample(d);
#else
    int s = 0;
#endif
#ifdef XE_SOURCE_UINT
    uint word = XePackUint(texelFetch(xe_source, XeSourcePixel(d), s), xe_transfer.sourceFormat);
#else
    uint word = XePack(texelFetch(xe_source, XeSourcePixel(d), s), xe_transfer.sourceFormat);
#endif
#ifdef XE_STENCIL_BIT
    if (((word >> xe_transfer.stencilBit) & 1u) == 0u)
        discard;
#else
    gl_FragDepth = XeWordDepth(word, xe_transfer.destFormat);  // destFormat: the destination's depth format (kDepthUnpacking)
#endif
}
)";

// The guest's 32-bit EDRAM word of a depth sample (edram_vk_plan.h PackDepthStencilWord, X vulkan_render_target_cache.cc
// :3591-3617): bits 8-31 the depth by the depth format (0 kD24S8: :3593-3604 uint(RoundEven(depth * float(0xFFFFFF))),
// Depth32ToUnorm24; 1 kD24FS8: Depth32To20e4, X spirv_shader_translator_rb.cc:236-326, truncating, no [0, 0.5) remap),
// bits 0-7 the stencil. Shared by the depth -> colour transfer and the depth resolve's sample dump.
const char* const kDepthPacking = R"(
uint XeDepthTo20e4(float depth)
{
    uint u = floatBitsToUint(depth);
    uint b = u < 0x38800000u ? ((u & 0x7FFFFFu) | 0x800000u) >> min(113u - (u >> 23u), 24u) : u - (112u << 23u);
    return (b >> 3u) & 0xFFFFFFu;
}
uint XeDepthToUnorm24(float depth)
{
    float d = depth > 0.0 ? min(depth, 1.0) : 0.0;
    return uint(roundEven(d * 16777215.0));
}
uint XeDepthStencilWord(float depth, uint stencil, uint depthFormat)
{
    uint d24 = depthFormat == 0u ? XeDepthToUnorm24(depth) : XeDepthTo20e4(depth);
    return (stencil & 0xFFu) | (d24 << 8u);
}
)";

// Depth -> colour (X kDepthToColor): the source texel by XeSourceTexel with XE_SWAP_COLUMNS (edram_vk_plan.cpp
// MapEdramTexel; X :2516-2577, :2779-2864, :2916-2954), a multisampled source read per sample with XE_SOURCE_MS (X
// :2307-2358, :3052-3080); the depth packed by the source's depth format (sourceFormat: 0 kD24S8, X :3593-3604
// uint(RoundEven(depth * float(0xFFFFFF))), edram_vk_plan.h Depth32ToUnorm24; 1 kD24FS8, Depth32To20e4, X
// spirv_shader_translator_rb.cc:236-326, truncating, no [0, 0.5) remap); the word stencil | depth << 8 (X :3614-3617),
// unpacked into the destination's storage format (X :3621-3690); with XE_DEST_UINT into a 16_16 destination's R16G16_UINT
// view as the halves (X :3692-3711).
const char* const kTransferDepthToColor = R"(
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform sampler2DMS xe_source_depth;
layout(set = 0, binding = 1) uniform usampler2DMS xe_source_stencil;
#else
layout(set = 0, binding = 0) uniform sampler2D xe_source_depth;
layout(set = 0, binding = 1) uniform usampler2D xe_source_stencil;
#endif
#ifdef XE_DEST_UINT
layout(location = 0) out uvec4 xe_word;
#else
layout(location = 0) out vec4 xe_color;
#endif
void main()
{
    ivec3 t = XeSourceTexel(uvec2(gl_FragCoord.xy));
#ifdef XE_SOURCE_MS
    float depth = texelFetch(xe_source_depth, t.xy, t.z).r;
    uint stencil = texelFetch(xe_source_stencil, t.xy, t.z).r;
#else
    float depth = texelFetch(xe_source_depth, t.xy, 0).r;
    uint stencil = texelFetch(xe_source_stencil, t.xy, 0).r;
#endif
    uint word = XeDepthStencilWord(depth, stencil, xe_transfer.sourceFormat);
#ifdef XE_DEST_UINT
    xe_word = XeUnpackUint(word, xe_transfer.destFormat);
#else
    xe_color = XeUnpack(word, xe_transfer.destFormat);
#endif
}
)";

// The sample dump of a resolve source (edram_vk_plan.h DumpLayout): one R32_UINT fragment per guest sample, the
// rectangle's pixels laid out as columns x rows blocks of guest samples (g & 1 picks the column, g >> 1 the row); the
// value is the guest's 32-bit EDRAM word of the sample (XePack: exact for storage 0 and 2, X's Float32To7e3 for 7e3).
// Vulkan has no copy of a multisampled image to a buffer, so the samples become pixels of a 1x image first; the CPU then
// does X's full resolve (edram_vk_plan.cpp ResolveConvertToGuest) after the slot retires.
const char* const kDumpTexel = R"(
// The source pixel (xy) and host sample (z) of a dump fragment: block (g & 1, g >> 1) of the dump holds guest sample g.
ivec3 XeDumpTexel(uvec2 d)
{
    uvec2 block = uvec2(d.x / xe_transfer.width, d.y / xe_transfer.height);
    uvec2 p = d - block * uvec2(xe_transfer.width, xe_transfer.height) + uvec2(xe_transfer.x0, xe_transfer.y0);
    uint g = block.x | (block.y << 1u);
    // The host sample of guest sample g (edram_vk_plan.cpp GuestSampleOfHost, its own inverse).
    uint h = xe_transfer.sourceMsaa == 2u ? ((g & 1u) << 1u) | ((g >> 1u) & 1u) : xe_transfer.sourceMsaa == 1u ? (g ^ 1u) : 0u;
    return ivec3(ivec2(p), int(h));
}
)";
// XE_SOURCE_UINT: a source with a transfer view (storage 4, 16_16: R16G16_UINT) is read raw and packed with XePackUint
// (word = r | g << 16, the guest's EDRAM word, red the low half as X's XeUnpackR16G16Edram reads it, pixel_formats.xesli
// :367-371), never through floats: XePack has no 16_16 branch, and the SNORM float path loses 0x8000.
const char* const kResolveDump = R"(
#ifdef XE_SOURCE_UINT
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform usampler2DMS xe_source;
#else
layout(set = 0, binding = 0) uniform usampler2D xe_source;
#endif
#else
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform sampler2DMS xe_source;
#else
layout(set = 0, binding = 0) uniform sampler2D xe_source;
#endif
#endif
layout(location = 0) out uint xe_word;
void main()
{
    ivec3 t = XeDumpTexel(uvec2(gl_FragCoord.xy));
#ifdef XE_SOURCE_MS
    int s = t.z;
#else
    int s = 0;
#endif
#ifdef XE_SOURCE_UINT
    xe_word = XePackUint(texelFetch(xe_source, t.xy, s), xe_transfer.sourceFormat);
#else
    xe_word = XePack(texelFetch(xe_source, t.xy, s), xe_transfer.sourceFormat);
#endif
}
)";

// The sample dump of a depth resolve's source (X GetCopyShader: always the fast bit copy of the EDRAM word for depth,
// draw_util.cc :1310-1324): the depth and stencil views (bindings 0 and 1, as kTransferDepthToColor), each guest sample's
// EDRAM word XeDepthStencilWord by the depth format (sourceFormat); the CPU then picks the selected sample
// (edram_vk_plan.cpp ResolveDepthToGuest).
const char* const kResolveDumpDepth = R"(
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform sampler2DMS xe_source_depth;
layout(set = 0, binding = 1) uniform usampler2DMS xe_source_stencil;
#else
layout(set = 0, binding = 0) uniform sampler2D xe_source_depth;
layout(set = 0, binding = 1) uniform usampler2D xe_source_stencil;
#endif
layout(location = 0) out uint xe_word;
void main()
{
    ivec3 t = XeDumpTexel(uvec2(gl_FragCoord.xy));
#ifdef XE_SOURCE_MS
    float depth = texelFetch(xe_source_depth, t.xy, t.z).r;
    uint stencil = texelFetch(xe_source_stencil, t.xy, t.z).r;
#else
    float depth = texelFetch(xe_source_depth, t.xy, 0).r;
    uint stencil = texelFetch(xe_source_stencil, t.xy, 0).r;
#endif
    xe_word = XeDepthStencilWord(depth, stencil, xe_transfer.sourceFormat);
}
)";

// FM4 Recomp (FM4_RESOLVE_RESIDENT, resolve_resident.h): one R32_UINT fragment per pixel of the resolve rectangle, the word
// edram_vk_plan.cpp ResolveConvertPixel computes (X resolve.xesli:273-321, :651-704; pixel_formats.xesli:116-125) from the
// guest words of its selected samples, each the word the sample dump would hold (XePack of the host sample, kResolveDump), so
// both paths start from the same words. The push constants reuse TransferPush: destSamples4 the sample count, destSample the
// guest sample indices (2 bits each, ResolveSamples), destFormat the texture format (6, 7, 50, 54), stencilBit the
// dest_exp_bias_factor's float bits, pitchTiles the red/blue swap, x0 y0 the rectangle in the source. `precise`: no fused
// multiply-add, as the CPU's separate float multiply and add.
const char* const kResolveResident = R"(
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform sampler2DMS xe_source;
#else
layout(set = 0, binding = 0) uniform sampler2D xe_source;
#endif
layout(location = 0) out uint xe_word;
uint XeResolveUnorm(float v, float scale)
{
    precise float s = v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0;
    precise float t = s * scale + 0.5;
    return uint(t);
}
void main()
{
    ivec2 p = ivec2(uvec2(gl_FragCoord.xy) + uvec2(xe_transfer.x0, xe_transfer.y0));
    uint count = xe_transfer.destSamples4;
    precise vec4 pixel = vec4(0.0);
    for (uint i = 0u; i < count; i++)
    {
        uint g = (xe_transfer.destSample >> (2u * i)) & 3u;
        // The host sample of guest sample g (edram_vk_plan.cpp GuestSampleOfHost, its own inverse; kDumpTexel).
        uint h = xe_transfer.sourceMsaa == 2u ? ((g & 1u) << 1u) | ((g >> 1u) & 1u) : xe_transfer.sourceMsaa == 1u ? (g ^ 1u) : 0u;
#ifdef XE_SOURCE_MS
        vec4 texel = texelFetch(xe_source, p, int(h));
#else
        vec4 texel = texelFetch(xe_source, p, 0);
#endif
        vec4 s = XeUnpack(XePack(texel, xe_transfer.sourceFormat), xe_transfer.sourceFormat);
        pixel = i == 0u ? s : pixel + s;
    }
    precise float e = uintBitsToFloat(xe_transfer.stencilBit);
    if (count >= 2u)
        e *= 0.5;
    if (count == 4u)
        e *= 0.5;
    pixel = pixel * e;
    if (xe_transfer.pitchTiles != 0u)
        pixel = pixel.bgra;
    if (xe_transfer.destFormat == 6u || xe_transfer.destFormat == 50u)
        xe_word = XeResolveUnorm(pixel.r, 255.0) | (XeResolveUnorm(pixel.g, 255.0) << 8u) | (XeResolveUnorm(pixel.b, 255.0) << 16u) |
                  (XeResolveUnorm(pixel.a, 255.0) << 24u);
    else
        xe_word = XeResolveUnorm(pixel.r, 1023.0) | (XeResolveUnorm(pixel.g, 1023.0) << 10u) | (XeResolveUnorm(pixel.b, 1023.0) << 20u) |
                  (XeResolveUnorm(pixel.a, 3.0) << 30u);
}
)";

struct TransferPush
{
    uint32_t destSamples4, destSample, sourceFormat, destFormat, stencilBit, pitchTiles, tileOffset;
    uint32_t sourceMsaa, destMsaa, x0, y0, width, height;
    uint32_t sourcePitchTiles;
};

enum class PipelineKind : uint32_t { Color = 0, Depth = 1, StencilBit = 2, DepthToColor = 3, Dump = 4, ColorToDepth = 5, ColorToStencilBit = 6, DumpDepth = 7,
                                     Resident = 8 };

// ---- State ----------------------------------------------------------------------------------------------------

struct Target
{
    Key key;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;      // the attachment view (all aspects); also the sampled colour view
    VkImageView depthView = VK_NULL_HANDLE; // depth aspect, sampled by a depth transfer
    VkImageView stencilView = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t samples = 1, width = 0, height = 0;
    bool depth = false;
    VkDescriptorSet source = VK_NULL_HANDLE;  // this target as a transfer source
    VkFramebuffer framebuffer = VK_NULL_HANDLE;  // over `view`, for the unit's own pass (transfers into it, clears)
    // Storage 14 (32_FLOAT, R32_SFLOAT) and 4 (16_16, R16G16_SNORM), created MUTABLE_FORMAT: the transfer view
    // (edram_vk_plan.h TransferViewFormat: R32_UINT, R16G16_UINT), as a transfer source and as the attachment of the unit's
    // own passes (X :1628-1653, :5393-5404, :5417-5421: transfers and clears move the raw bits).
    VkFormat uintFormat = VK_FORMAT_UNDEFINED;
    VkImageView uintView = VK_NULL_HANDLE;
    VkDescriptorSet uintSource = VK_NULL_HANDLE;
    VkFramebuffer uintFramebuffer = VK_NULL_HANDLE;
    // The tracked state: the layout and the last stages/accesses that touched it (the next barrier's source).
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags access = 0;
    VkImageLayout RestLayout() const { return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; }
    VkImageAspectFlags Aspects() const { return depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT; }
};

struct Pending  // a read-back resolve of the open submission (one, or several with FM4_RESOLVE_RESIDENT)
{
    edram::ResolveInfo info;
    uint32_t storage = 0;
    bool fast = false;  // ResolveToGuest (a 1x bit copy) rather than ResolveConvertToGuest (X's full resolve)
    bool depth = false; // ResolveDepthToGuest (a depth resolve through kResolveDumpDepth)
    VkDeviceSize offset = 0;  // its words in the staging buffer
    bool check = false;       // FM4_RESOLVE_RESIDENT_CHECK: its GPU words at checkOffset
    VkDeviceSize checkOffset = 0;
};

// FM4 Recomp (FM4_RESOLVE_RESIDENT): the resident image's size bounds a converted rectangle; FM4's are 1280x720 at most.
constexpr uint32_t kResidentWidth = 2048, kResidentHeight = 1024;

// The 1x R32_UINT image the sample dump renders into (kResolveDump), then copied to the staging buffer. Its size bounds
// a dumped rectangle: (width * columns) x (height * rows) of edram_vk_plan.h DumpLayout; FM4's HDR resolve (1280x512 2x)
// needs 2560x512, a 1280x720 4x one 2560x1440.
constexpr uint32_t kDumpWidth = 4096, kDumpHeight = 2048;

struct Unit
{
    VulkanDevice dev;
    VkPhysicalDeviceLimits limits{};
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, colorFrom1x = VK_NULL_HANDLE, colorFrom4x = VK_NULL_HANDLE, depthFrom1x = VK_NULL_HANDLE,
                   stencilFrom1x = VK_NULL_HANDLE, depthToColorFrom1x = VK_NULL_HANDLE, depthToColorFromMs = VK_NULL_HANDLE,
                   dumpFrom1x = VK_NULL_HANDLE, dumpFromMs = VK_NULL_HANDLE, dumpDepth1x = VK_NULL_HANDLE, dumpDepthMs = VK_NULL_HANDLE,
                   depthFromMs = VK_NULL_HANDLE, stencilFromMs = VK_NULL_HANDLE, colorToDepth1x = VK_NULL_HANDLE, colorToDepthMs = VK_NULL_HANDLE,
                   colorToStencil1x = VK_NULL_HANDLE, colorToStencilMs = VK_NULL_HANDLE;
    // The colour transfer with a 32_FLOAT or 16_16 side, [source MS][source uint][dest uint] (the [0/1][0][0] pair is
    // colorFrom1x / colorFrom4x; kTransferColor's XE_SOURCE_UINT / XE_DEST_UINT).
    VkShaderModule colorUint[2][2][2] = {};
    // Through a 16_16 transfer view, [source MS]: depth -> colour into it (XE_DEST_UINT), colour -> depth and stencil bit from it
    // (XE_SOURCE_UINT).
    VkShaderModule depthToColorUint[2] = {}, colorToDepthUint[2] = {}, colorToStencilUint[2] = {};
    // The sample dump through a transfer view (kResolveDump XE_SOURCE_UINT), [source MS].
    VkShaderModule dumpUint[2] = {};
    Target dump;  // kDumpWidth x kDumpHeight R32_UINT, not an EDRAM key
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    std::map<uint32_t, std::unique_ptr<Target>> targets;  // by Key::packed; addresses stable for the run
    std::map<uint64_t, VkRenderPass> passes;
    std::map<uint64_t, VkPipeline> pipelines;
    edram::Ownership ownership;
    std::vector<Pending> pending;
    VkDeviceSize stagingUsed = 0;
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): the converted words (kResolveResident) and the last recorded resolve (lastResolve).
    VkShaderModule resident1x = VK_NULL_HANDLE, residentMs = VK_NULL_HANDLE;
    Target resident;  // kResidentWidth x kResidentHeight R32_UINT, not an EDRAM key
    ResidentResolve last;
    bool lastValid = false;
    uint64_t residentResolves = 0, checkedResolves = 0, checkedPixels = 0, checkMismatches = 0;
    uint8_t* guestBase = nullptr;               // main.cpp's writable guest memory (renderer.h Options::guestBase)
    ResolveWriteObserver observer = nullptr;    // guest_writes.h MarkGuestWritten, from the renderer
    uint64_t transfers = 0, transferDraws = 0, resolves = 0;
    std::map<uint64_t, uint64_t> transferPairs;  // (source key << 32 | destination key) -> count; the first of each is logged
};
std::unique_ptr<Unit> unit;

bool Fail(std::string& error, std::string text)
{
    error = std::move(text);
    return false;
}

bool Ok(VkResult r, const char* what, std::string& error)
{
    return r == VK_SUCCESS || Fail(error, std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")");
}

// ---- Barriers (explicit; every image's state tracked) ------------------------------------------------------------

void Transition(VkCommandBuffer c, Target& t, VkImageLayout layout, VkPipelineStageFlags stages, VkAccessFlags access)
{
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = t.layout;
    b.newLayout = layout;
    b.srcAccessMask = t.access;
    b.dstAccessMask = access;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {t.Aspects(), 0, 1, 0, 1};
    OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
    vkCmdPipelineBarrier(c, t.stages, stages, 0, 0, nullptr, 0, nullptr, 1, &b);
    t.layout = layout, t.stages = stages, t.access = access;
}

// Back to the rest state: the attachment layout the renderer's passes expect (render_hooks.h HostTargets). The
// tracked source of the next barrier is then "any attachment access", since a renderer pass may write it.
void Rest(VkCommandBuffer c, Target& t)
{
    const VkAccessFlags access = t.depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                         : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const VkPipelineStageFlags stages =
        t.depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    Transition(c, t, t.RestLayout(), stages, access);
}

// ---- Render passes and pipelines ---------------------------------------------------------------------------------

// One attachment (the destination), loaded and stored, initial = final = its rest layout; external dependencies
// as pipeline_cache.cpp RenderPass's, widened to the fragment-shader reads of transfers.
bool Pass(VkFormat format, uint32_t samples, bool depth, VkRenderPass& out, std::string& error)
{
    const uint64_t key = uint64_t(format) | uint64_t(samples) << 32 | uint64_t(depth) << 40;
    if (const auto it = unit->passes.find(key); it != unit->passes.end())
        return out = it->second, true;
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VkSampleCountFlagBits(samples);
    a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (!depth)
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = a.finalLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference ref{0, a.initialLayout};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = depth ? 0 : 1;
    subpass.pColorAttachments = depth ? nullptr : &ref;
    subpass.pDepthStencilAttachment = depth ? &ref : nullptr;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = kAttachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = kAttachmentStages;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstAccessMask = kAttachmentAccess;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = kAttachmentStages;
    deps[1].dstStageMask = kAttachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = kAttachmentAccess | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &a;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = deps;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!Ok(vkCreateRenderPass(unit->dev.device, &info, nullptr, &pass), "vkCreateRenderPass (EDRAM)", error))
        return false;
    unit->passes[key] = pass;
    out = pass;
    return true;
}

// The format and framebuffer of the unit's own passes on `t`: the transfer view for storage 14 and 4, else the target's own.
bool UintStorage(const Target& t) { return t.uintView != VK_NULL_HANDLE; }
VkFormat PassFormat(const Target& t) { return UintStorage(t) ? t.uintFormat : t.format; }
VkFramebuffer PassFramebuffer(const Target& t) { return UintStorage(t) ? t.uintFramebuffer : t.framebuffer; }

// `sourceFlags`: bit 0 a multisampled source, bit 1 a storage-14 or storage-4 source read through its transfer view.
bool Pipeline(PipelineKind kind, uint32_t sourceFlags, const Target& dest, uint32_t sample, VkPipeline& out, std::string& error)
{
    const bool sourceMs = (sourceFlags & 1) != 0, sourceUint = (sourceFlags & 2) != 0, destUint = UintStorage(dest);
    const VkFormat passFormat = PassFormat(dest);
    const uint64_t key = uint64_t(kind) | uint64_t(sourceFlags & 3) << 4 | uint64_t(sample) << 6 | uint64_t(dest.samples) << 8 | uint64_t(passFormat) << 12;
    if (const auto it = unit->pipelines.find(key); it != unit->pipelines.end())
        return out = it->second, true;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!Pass(passFormat, dest.samples, dest.depth, pass, error))
        return false;
    // A transfer view: colour -> colour on either side; depth -> colour into it; colour -> depth (and stencil bit) from it.
    // ClassifyTransfer admits only storage 4 for the last two (storage 14 stays colour <-> colour). The sample dump reads it
    // (a storage-4 resolve source; its R32_UINT dump image is not a transfer view).
    const bool uintVariant = kind == PipelineKind::Color ? (sourceUint || destUint)
                             : kind == PipelineKind::DepthToColor ? destUint
                             : kind == PipelineKind::ColorToDepth || kind == PipelineKind::ColorToStencilBit || kind == PipelineKind::Dump ? sourceUint
                                                                                                              : false;
    if ((sourceUint || destUint) && !uintVariant)
        return Fail(error, "a transfer-view (storage 14 or 4) side in a transfer that has no uint variant");
    const VkShaderModule fragment = kind == PipelineKind::Color && uintVariant ? unit->colorUint[sourceMs][sourceUint][destUint]
                                    : kind == PipelineKind::DepthToColor && uintVariant ? unit->depthToColorUint[sourceMs]
                                    : kind == PipelineKind::ColorToDepth && uintVariant ? unit->colorToDepthUint[sourceMs]
                                    : kind == PipelineKind::ColorToStencilBit && uintVariant ? unit->colorToStencilUint[sourceMs]
                                    : kind == PipelineKind::Dump && uintVariant ? unit->dumpUint[sourceMs]
                                    : kind == PipelineKind::Color ? (sourceMs ? unit->colorFrom4x : unit->colorFrom1x)
                                    : kind == PipelineKind::Depth             ? (sourceMs ? unit->depthFromMs : unit->depthFrom1x)
                                    : kind == PipelineKind::DepthToColor      ? (sourceMs ? unit->depthToColorFromMs : unit->depthToColorFrom1x)
                                    : kind == PipelineKind::Dump              ? (sourceMs ? unit->dumpFromMs : unit->dumpFrom1x)
                                    : kind == PipelineKind::DumpDepth         ? (sourceMs ? unit->dumpDepthMs : unit->dumpDepth1x)
                                    : kind == PipelineKind::Resident          ? (sourceMs ? unit->residentMs : unit->resident1x)
                                    : kind == PipelineKind::ColorToDepth      ? (sourceMs ? unit->colorToDepthMs : unit->colorToDepth1x)
                                    : kind == PipelineKind::ColorToStencilBit ? (sourceMs ? unit->colorToStencilMs : unit->colorToStencil1x)
                                                                              : (sourceMs ? unit->stencilFromMs : unit->stencilFrom1x);
    const bool writesDepth = kind == PipelineKind::Depth || kind == PipelineKind::ColorToDepth;
    const bool writesStencilBit = kind == PipelineKind::StencilBit || kind == PipelineKind::ColorToStencilBit;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& s : stages)
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, s.pName = "main";
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = unit->vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fragment;
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    // A 4x destination: one draw per sample with sample mask 1 << sample (X :2497-2502, the path without
    // sampleRateShading, which vulkan_device.cpp does not enable).
    const VkSampleMask mask = 1u << sample;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VkSampleCountFlagBits(dest.samples);
    multisample.pSampleMask = dest.samples > 1 ? &mask : nullptr;
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (writesDepth)
        depthStencil.depthTestEnable = depthStencil.depthWriteEnable = VK_TRUE, depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    if (writesStencilBit)
    {
        depthStencil.stencilTestEnable = VK_TRUE;
        depthStencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xFF, 0, 0};  // mask, reference dynamic
        depthStencil.back = depthStencil.front;
    }
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = dest.depth ? 0 : 1;
    colorBlend.pAttachments = dest.depth ? nullptr : &blend;
    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                       VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = writesStencilBit ? 4 : 2;
    dynamic.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = dest.depth ? &depthStencil : nullptr;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = unit->layout;
    info.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!Ok(vkCreateGraphicsPipelines(unit->dev.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines (EDRAM transfer)", error))
        return false;
    unit->pipelines[key] = pipeline;
    out = pipeline;
    return true;
}

// ---- Targets ------------------------------------------------------------------------------------------------------

bool View(VkImage image, VkFormat format, VkImageAspectFlags aspects, VkImageView& out, std::string& error)
{
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {aspects, 0, 1, 0, 1};
    return Ok(vkCreateImageView(unit->dev.device, &info, nullptr, &out), "vkCreateImageView (EDRAM)", error);
}

// The host image of `k` (render_targets_vk.h plan 1 and 2(a): a true multisampled image for a 4x Key), created on
// first use and cleared to zero in `c` (the EDRAM's power-on contents are not known; zero is deterministic), then
// left in its rest layout.
Target* GetTarget(VkCommandBuffer c, Key k, std::string& error)
{
    if (const auto it = unit->targets.find(k.packed); it != unit->targets.end())
        return it->second.get();
    if (std::string why; !edram::SupportedKey(k, why))
        return Fail(error, why), nullptr;
    if (unit->targets.size() >= kMaxTargets)
        return Fail(error, "more than " + std::to_string(kMaxTargets) + " EDRAM host targets (the descriptor pool's size)"), nullptr;
    auto t = std::make_unique<Target>();
    t->key = k;
    t->depth = k.IsDepth();
    t->format = t->depth ? HostDepthFormat(edram::DepthFormat(k.Format()), false) : HostColorFormat(edram::ColorFormat(k.Format()));
    t->samples = k.MsaaMode() == Msaa::X4 ? 4 : k.MsaaMode() == Msaa::X2 ? 2 : 1;  // 2x keys are native 2x images (draw_hdr1: was 1)
    t->width = k.Width(), t->height = k.Height();
    if (t->format == VK_FORMAT_UNDEFINED)
        return Fail(error, "no host format for EDRAM key " + std::to_string(k.packed)), nullptr;
    if (t->width == 0 || t->height == 0 || t->width > unit->limits.maxImageDimension2D || t->height > unit->limits.maxImageDimension2D ||
        t->width > unit->limits.maxFramebufferWidth || t->height > unit->limits.maxFramebufferHeight)
        return Fail(error, "EDRAM key " + std::to_string(k.packed) + ": a " + std::to_string(t->width) + "x" + std::to_string(t->height) +
                               " host target exceeds the device's image or framebuffer limits"),
               nullptr;
    const VkImageUsageFlags usage = (t->depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Storage 14 and 4: MUTABLE_FORMAT for the transfer view (X creates these targets with the uint transfer format as a
    // view format, vulkan_render_target_cache.cc:1628-1653 GetColorOwnershipTransferVulkanFormat; edram_vk_plan.h
    // TransferViewFormat).
    t->uintFormat = t->depth ? VK_FORMAT_UNDEFINED : TransferViewFormat(edram::ColorFormat(k.Format()));
    const bool uintStorage = t->uintFormat != VK_FORMAT_UNDEFINED;
    const VkImageCreateFlags flags = uintStorage ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    // The image's format, then the transfer view's (an attachment and a sampled image at this sample count).
    for (const VkFormat format : {t->format, t->uintFormat})
    {
        if (format == VK_FORMAT_UNDEFINED)
            continue;
        VkImageFormatProperties properties{};
        if (vkGetPhysicalDeviceImageFormatProperties(unit->dev.gpu, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags, &properties) != VK_SUCCESS ||
            !(properties.sampleCounts & t->samples) || properties.maxExtent.width < t->width || properties.maxExtent.height < t->height)
            return Fail(error, "EDRAM key " + std::to_string(k.packed) + ": format " + std::to_string(int(format)) + " with " + std::to_string(t->samples) +
                                   " sample(s) and attachment, sampled and transfer usage is not supported by the device"),
                   nullptr;
    }
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = flags;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = t->format;
    info.extent = {t->width, t->height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VkSampleCountFlagBits(t->samples);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Ok(vkCreateImage(unit->dev.device, &info, nullptr, &t->image), "vkCreateImage (EDRAM)", error))
        return nullptr;
    Target* raw = t.get();
    unit->targets[k.packed] = std::move(t);  // owned from here, so shutdown destroys whatever was created
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(unit->dev.device, raw->image, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(unit->dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!Ok(vkAllocateMemory(unit->dev.device, &allocate, nullptr, &raw->memory), "vkAllocateMemory (EDRAM)", error) ||
        !Ok(vkBindImageMemory(unit->dev.device, raw->image, raw->memory, 0), "vkBindImageMemory (EDRAM)", error))
        return nullptr;
    if (!View(raw->image, raw->format, raw->Aspects(), raw->view, error))
        return nullptr;
    if (raw->depth && (!View(raw->image, raw->format, VK_IMAGE_ASPECT_DEPTH_BIT, raw->depthView, error) ||
                       !View(raw->image, raw->format, VK_IMAGE_ASPECT_STENCIL_BIT, raw->stencilView, error)))
        return nullptr;
    // The target as a transfer source: written once, never updated (the views and layouts are fixed for the run).
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = unit->pool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &unit->setLayout;
    if (!Ok(vkAllocateDescriptorSets(unit->dev.device, &setInfo, &raw->source), "vkAllocateDescriptorSets (EDRAM)", error))
        return nullptr;
    const VkImageLayout sampledLayout = raw->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo images[2] = {{unit->sampler, raw->depth ? raw->depthView : raw->view, sampledLayout},
                                             {unit->sampler, raw->stencilView, sampledLayout}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; i++)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = raw->source;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(unit->dev.device, raw->depth ? 2 : 1, writes, 0, nullptr);
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!Pass(raw->format, raw->samples, raw->depth, pass, error))
        return nullptr;
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = pass;
    fb.attachmentCount = 1;
    fb.pAttachments = &raw->view;
    fb.width = raw->width, fb.height = raw->height, fb.layers = 1;
    if (!Ok(vkCreateFramebuffer(unit->dev.device, &fb, nullptr, &raw->framebuffer), "vkCreateFramebuffer (EDRAM)", error))
        return nullptr;
    if (uintStorage)  // the transfer view: its own source set and framebuffer (the unit's passes on it use that format)
    {
        VkRenderPass uintPass = VK_NULL_HANDLE;
        if (!View(raw->image, raw->uintFormat, VK_IMAGE_ASPECT_COLOR_BIT, raw->uintView, error) ||
            !Ok(vkAllocateDescriptorSets(unit->dev.device, &setInfo, &raw->uintSource), "vkAllocateDescriptorSets (EDRAM uint)", error) ||
            !Pass(raw->uintFormat, raw->samples, false, uintPass, error))
            return nullptr;
        const VkDescriptorImageInfo uintImage{unit->sampler, raw->uintView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write = writes[0];
        write.dstSet = raw->uintSource;
        write.pImageInfo = &uintImage;
        vkUpdateDescriptorSets(unit->dev.device, 1, &write, 0, nullptr);
        fb.renderPass = uintPass;
        fb.pAttachments = &raw->uintView;
        if (!Ok(vkCreateFramebuffer(unit->dev.device, &fb, nullptr, &raw->uintFramebuffer), "vkCreateFramebuffer (EDRAM uint)", error))
            return nullptr;
    }
    // Zero, then the rest layout.
    Transition(c, *raw, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkImageSubresourceRange all{raw->Aspects(), 0, 1, 0, 1};
    if (raw->depth)
    {
        const VkClearDepthStencilValue zero{0.0f, 0};
        vkCmdClearDepthStencilImage(c, raw->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    }
    else
    {
        const VkClearColorValue zero{};
        vkCmdClearColorImage(c, raw->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    }
    Rest(c, *raw);
    std::fprintf(stderr, "[edram] host target for key %06X: %ux%u, %u sample(s), format %d\n", k.packed, raw->width, raw->height, raw->samples, int(raw->format));
    return raw;
}

// ---- Transfers ----------------------------------------------------------------------------------------------------

// Every transfer of `list` into `dest` must be one ClassifyTransfer accepts and come from a key that has a host
// target (checked before anything is recorded or the ownership map is kept).
bool CheckTransfers(Key dest, const std::vector<Transfer>& list, std::string& error)
{
    for (const Transfer& t : list)
    {
        if (ClassifyTransfer(t.source, dest, error) == TransferKind::Refused)
            return false;
        if (!unit->targets.count(t.source.packed))
            return Fail(error, "ownership transfer from key " + std::to_string(t.source.packed) + ", which has no host target");
    }
    return true;
}

bool Draw3(VkCommandBuffer c, PipelineKind kind, uint32_t sourceFlags, const Target& dest, uint32_t sample, const TransferPush& push,
           const std::vector<VkRect2D>& rects, std::string& error)
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!Pipeline(kind, sourceFlags, dest, sample, pipeline, error))
        return false;
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(c, unit->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    for (const VkRect2D& r : rects)
    {
        vkCmdSetScissor(c, 0, 1, &r);
        vkCmdDraw(c, 3, 1, 0, 0);
        unit->transferDraws++;
    }
    return true;
}

bool ExecuteTransfers(VkCommandBuffer c, Target& dest, const std::vector<Transfer>& list, std::string& error)
{
    for (const Transfer& t : list)
    {
        Target& source = *unit->targets.at(t.source.packed);
        const TransferKind kind = ClassifyTransfer(t.source, dest.key, error);
        if (kind == TransferKind::Refused)
            return false;
        edram::Rect r[3];
        const uint32_t n = edram::RangeRectangles(t.startTiles, t.endTiles, dest.key, r);
        std::vector<VkRect2D> rects;
        for (uint32_t i = 0; i < n; i++)
        {
            if (r[i].x + r[i].width > dest.width || r[i].y + r[i].height > dest.height)
                return Fail(error, "transfer rectangle outside the destination's host target");
            rects.push_back({{int32_t(r[i].x), int32_t(r[i].y)}, {r[i].width, r[i].height}});
        }
        if (rects.empty())
            continue;
        // A multisampled source is read with sampler2DMS: the device must sample that count (never assumed on MoltenVK). A
        // transfer view (R32_UINT, R16G16_UINT) is an integer format, read with usampler2DMS: sampledImageIntegerSampleCounts.
        if (source.samples > 1)
        {
            const VkSampleCountFlags need = source.samples;
            const VkPhysicalDeviceLimits& l = unit->limits;
            if (source.depth ? !(l.sampledImageDepthSampleCounts & need) || !(l.sampledImageStencilSampleCounts & need)
                             : !((UintStorage(source) ? l.sampledImageIntegerSampleCounts : l.sampledImageColorSampleCounts) & need))
                return Fail(error, "ownership transfer from a " + std::to_string(source.samples) + "-sample " + (source.depth ? "depth/stencil" : "colour") +
                                       " target: the device cannot sample that count");
        }
        // The source is sampled by the fragment shader: out of its rest layout, back after the pass.
        Transition(c, source, source.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        if (!Pass(PassFormat(dest), dest.samples, dest.depth, begin.renderPass, error))
            return false;
        begin.framebuffer = PassFramebuffer(dest);
        begin.renderArea = {{0, 0}, {dest.width, dest.height}};
        OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
        vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
        // A storage-14 or storage-4 source is read through its transfer view (its second set).
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, unit->layout, 0, 1, UintStorage(source) ? &source.uintSource : &source.source, 0,
                                nullptr);
        const VkViewport viewport{0.0f, 0.0f, float(dest.width), float(dest.height), 0.0f, 1.0f};
        vkCmdSetViewport(c, 0, 1, &viewport);
        // sourceFormat / destFormat: a colour key's storage format, or a depth key's depth format (kTransferDepthToColor,
        // kTransferColorToDepth, kTransferDepth between depth formats; the stencil-bit shaders read neither).
        TransferPush push{dest.samples == 4 ? 1u : 0u, 0, source.key.Format(), dest.key.Format(), 0,
                          dest.key.PitchTilesAt32bpp(), (dest.key.Base() - source.key.Base()) & (edram::kTileCount - 1),
                          uint32_t(source.key.MsaaMode()), uint32_t(dest.key.MsaaMode()), 0, 0, 0, 0, source.key.PitchTilesAt32bpp()};
        const uint32_t sourceMs = (source.samples > 1 ? 1u : 0u) | (UintStorage(source) ? 2u : 0u);  // Pipeline's sourceFlags
        bool ok = true;
        if (kind == TransferKind::Color || kind == TransferKind::DepthToColor)
        {
            // Depth -> colour: the source's depth and stencil views (its descriptor set, bindings 0 and 1), per sample when multisampled.
            const PipelineKind pk = kind == TransferKind::Color ? PipelineKind::Color : PipelineKind::DepthToColor;
            for (uint32_t s = 0; ok && s < dest.samples; s++)
                push.destSample = s, ok = Draw3(c, pk, sourceMs, dest, s, push, rects, error);
        }
        else
        {
            // Stencil zero in the rectangles first (X :4975-5011), then depth, then the stencil bits.
            std::vector<VkClearRect> clears;
            for (const VkRect2D& rect : rects)
                clears.push_back({rect, 0, 1});
            VkClearAttachment stencil{};
            stencil.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            vkCmdClearAttachments(c, 1, &stencil, uint32_t(clears.size()), clears.data());
            // Depth -> depth or colour -> depth (X kColorToDepth + kColorToStencilBit: the same three steps).
            const bool fromColor = kind == TransferKind::ColorToDepth;
            for (uint32_t s = 0; ok && s < dest.samples; s++)
                push.destSample = s, ok = Draw3(c, fromColor ? PipelineKind::ColorToDepth : PipelineKind::Depth, sourceMs, dest, s, push, rects, error);
            vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
            for (uint32_t bit = 0; ok && bit < 8; bit++)
            {
                vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
                push.stencilBit = bit;
                for (uint32_t s = 0; ok && s < dest.samples; s++)
                    push.destSample = s,
                    ok = Draw3(c, fromColor ? PipelineKind::ColorToStencilBit : PipelineKind::StencilBit, sourceMs, dest, s, push, rects, error);
            }
        }
        vkCmdEndRenderPass(c);  // ended even on a refusal, so the command buffer stays well formed until the stop
        Rest(c, source);
        if (!ok)
            return false;
        unit->transfers++;
        if (unit->transferPairs[uint64_t(t.source.packed) << 32 | dest.key.packed]++ == 0)
            std::fprintf(stderr, "[edram] first transfer key %06X (%u sample(s)) -> key %06X (%u sample(s)), tiles %u-%u\n", t.source.packed, source.samples,
                         dest.key.packed, dest.samples, t.startTiles, t.endTiles);
    }
    return true;
}

// ---- Hooks --------------------------------------------------------------------------------------------------------

bool AcquireTargets(VkCommandBuffer c, const RenderTarget& rt, bool needColor, bool needDepth, HostTargets& out, std::string& error)
{
    if (!needColor && !needDepth)
        return Fail(error, "a draw that needs neither a colour nor a depth target");
    // The binding exactly as the renderer's pipeline key needs it (edram_vk_plan.h BindRegistersFor); the rows the
    // draw can touch are the scissor's bottom within the surface (draw_state.h RenderTarget::targetHeight).
    const edram::Ownership before = unit->ownership;
    const edram::Binding b = edram::Bind(unit->ownership, BindRegistersFor(rt, needColor, needDepth), rt.targetHeight);
    if (!b.ok)
        return Fail(error, b.refusal);
    auto refuse = [&](std::string why) { unit->ownership = before; return Fail(error, std::move(why)); };
    if (!b.rasterizes)
        return refuse("the EDRAM model binds nothing for this draw (pitch 0 or not a drawing edram mode)");
    if ((needColor && b.color[0].Empty()) || (needDepth && b.depth.Empty()))
        return refuse("the EDRAM model dropped a target the draw needs (colour and depth at one base)");
    if ((needColor && !CheckTransfers(b.color[0], b.transfers[1], error)) || (needDepth && !CheckTransfers(b.depth, b.transfers[0], error)))
        return refuse(error);
    Target* color = needColor ? GetTarget(c, b.color[0], error) : nullptr;
    Target* depth = needDepth ? GetTarget(c, b.depth, error) : nullptr;
    if ((needColor && color == nullptr) || (needDepth && depth == nullptr))
        return refuse(error);
    if ((depth && !ExecuteTransfers(c, *depth, b.transfers[0], error)) || (color && !ExecuteTransfers(c, *color, b.transfers[1], error)))
        return false;  // recorded partially: the renderer stops the run with this error, nothing is submitted
    out = {};
    const Target& any = color ? *color : *depth;
    out.width = any.width, out.height = any.height, out.samples = any.samples;
    if (color)
        out.color = color->view, out.format = color->format;
    if (depth)
        out.depth = depth->view, out.depthFormat = depth->format;
    if (color && depth && (color->width != depth->width || color->height != depth->height || color->samples != depth->samples))
        return Fail(error, "the colour and depth host targets differ in size or samples");
    return true;
}

// The clear of `rect` on `t` (X vulkan_render_target_cache.cc:5340-5431), inside the unit's pass on t.
bool Clear(VkCommandBuffer c, Target& t, const edram::Rect& rect, uint32_t value, std::string& error)
{
    if (rect.x + rect.width > t.width || rect.y + rect.height > t.height)
        return Fail(error, "resolve clear rectangle outside its host target");
    VkClearAttachment a{};
    if (t.depth)
    {
        a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        DecodeDepthClear(value, edram::DepthFormat(t.key.Format()), a.clearValue.depthStencil.depth, a.clearValue.depthStencil.stencil);
    }
    else
    {
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        // Storage 14 and 4: the raw bits through the transfer view (X :5393-5404, :5417-5421); the others as floats (X :5366-5395).
        if (UintStorage(t) ? !DecodeColorClearUint(value, t.key.Format(), a.clearValue.color.uint32)
                           : !DecodeColorClear(value, t.key.Format(), a.clearValue.color.float32))
            return Fail(error, "resolve colour clear of storage format " + std::to_string(t.key.Format()));
    }
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    if (!Pass(PassFormat(t), t.samples, t.depth, begin.renderPass, error))
        return false;
    begin.framebuffer = PassFramebuffer(t);
    begin.renderArea = {{0, 0}, {t.width, t.height}};
    OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
    vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkClearRect r{{{int32_t(rect.x), int32_t(rect.y)}, {rect.width, rect.height}}, 0, 1};
    vkCmdClearAttachments(c, 1, &a, 1, &r);
    vkCmdEndRenderPass(c);
    return true;
}

uint32_t ReadRegisterHook(const void* context, uint32_t index) { return runtime::gpu::ReadRegister(static_cast<const uint8_t*>(context), index); }

// The guest view of [physical, physical + bytes) as one contiguous block (both ends in one view).
bool GuestRange(uint32_t physical, uint32_t bytes, uint32_t& guest, std::string& error)
{
    uint32_t end = 0;
    if (bytes == 0 || !runtime::PhysicalToGuest(physical, guest) || !runtime::PhysicalToGuest(physical + bytes - 1, end) || end - guest != bytes - 1)
        return Fail(error, "guest range " + std::to_string(physical) + " + " + std::to_string(bytes) + " is not in one view");
    return true;
}

// The sample dump (kResolveDump / kResolveDumpDepth) of rectangle `r` of `source`: every guest sample one R32_UINT pixel
// of the dump image (DumpLayout), copied tightly packed into the staging buffer at `stagingOffset`.
bool RecordDump(VkCommandBuffer c, Target& source, bool depth, uint32_t storage, Msaa msaa, const edram::Rect& r, const DumpLayout& layout,
                VkDeviceSize stagingOffset, std::string& error)
{
    Target& dump = unit->dump;
    // A transfer-view source (storage 4) is read with usampler2DMS when multisampled: the device must sample that count for
    // integer formats (as ExecuteTransfers checks; Init checks only sampledImageColorSampleCounts).
    const bool uintSource = !depth && UintStorage(source);
    if (uintSource && source.samples > 1 && !(unit->limits.sampledImageIntegerSampleCounts & source.samples))
        return Fail(error, "resolve sample dump of a " + std::to_string(source.samples) +
                               "-sample target through its transfer view: the device cannot sample that count (sampledImageIntegerSampleCounts)");
    Transition(c, source, source.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    Transition(c, dump, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    const VkExtent2D extent{layout.width * layout.columns, layout.height * layout.rows};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    if (!Pass(dump.format, 1, false, begin.renderPass, error))
        return false;
    begin.framebuffer = dump.framebuffer;
    begin.renderArea = {{0, 0}, extent};
    OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
    vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, unit->layout, 0, 1, uintSource ? &source.uintSource : &source.source, 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, float(dump.width), float(dump.height), 0.0f, 1.0f};
    vkCmdSetViewport(c, 0, 1, &viewport);
    const TransferPush push{0, 0, storage, 0, 0, 0, 0, uint32_t(msaa), 0, r.x, r.y, layout.width, layout.height, 0};
    const uint32_t sourceFlags = (source.samples > 1 ? 1u : 0u) | (uintSource ? 2u : 0u);  // Pipeline's sourceFlags
    const bool ok = Draw3(c, depth ? PipelineKind::DumpDepth : PipelineKind::Dump, sourceFlags, dump, 0, push, {VkRect2D{{0, 0}, extent}}, error);
    vkCmdEndRenderPass(c);
    Rest(c, source);
    if (!ok)
        return false;
    Transition(c, dump, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.bufferOffset = stagingOffset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent.width, extent.height, 1};  // tightly packed: DumpLayout::Word
    vkCmdCopyImageToBuffer(c, dump.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, unit->staging, 1, &copy);
    return true;
}

// FM4 Recomp (FM4_RESOLVE_RESIDENT, resolve_resident.h): the converted words of rectangle `r` of `source` (kResolveResident) into
// the resident image at (0, 0), left in TRANSFER_SRC_OPTIMAL for the texture unit's copies (TextureHooks::adoptResolve).
bool RecordResident(VkCommandBuffer c, Target& source, uint32_t storage, const edram::ResolveInfo& info, const edram::Rect& r, std::string& error)
{
    Target& dest = unit->resident;
    uint32_t indices[4] = {}, count = 0, packed = 0;
    if (!ResolveSamples(info, indices, count))
        return Fail(error, "resident resolve: a selected sample the source does not have");
    for (uint32_t i = 0; i < count; i++)
        packed |= indices[i] << (2 * i);
    const uint32_t biasBits = (uint32_t(info.destExpBias) << 23) + 0x3F800000u;  // as ResolveConvertPixel (modulo 2^32)
    Transition(c, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    Transition(c, dest, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    if (!Pass(dest.format, 1, false, begin.renderPass, error))
        return false;
    begin.framebuffer = dest.framebuffer;
    begin.renderArea = {{0, 0}, {r.width, r.height}};
    OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
    vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, unit->layout, 0, 1, &source.source, 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, float(dest.width), float(dest.height), 0.0f, 1.0f};
    vkCmdSetViewport(c, 0, 1, &viewport);
    const TransferPush push{count, packed, storage, info.destFormat, biasBits, info.destSwap ? 1u : 0u, 0, uint32_t(info.msaa), 0, r.x, r.y, r.width, r.height, 0};
    const bool ok = Draw3(c, PipelineKind::Resident, source.samples > 1 ? 1u : 0u, dest, 0, push, {VkRect2D{{0, 0}, {r.width, r.height}}}, error);
    vkCmdEndRenderPass(c);
    Rest(c, source);
    if (!ok)
        return false;
    Transition(c, dest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    unit->residentResolves++;
    return true;
}

bool Resolve(VkCommandBuffer c, const uint8_t* base, const DrawState& d, uint64_t draw, std::string& error)
{
    (void)d, (void)draw;
    unit->lastValid = false;
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): several read-back resolves per submission while the staging has room (resolveRoom).
    if (!unit->pending.empty() && !ResolveResident())
        return Fail(error, "a second resolve in one submission (the renderer flushes the slot after each resolve)");
    if (unit->stagingUsed + 2 * kStagingBytes > kStagingTotalBytes)
        return Fail(error, "a resolve with the staging buffer full of pending resolves (the renderer flushes when resolveRoom is false)");
    if (base != unit->guestBase)
        return Fail(error, "the command processor's guest base is not the writable base the EDRAM unit was given (renderer.h Options::guestBase)");
    edram::ResolveRegisters regs = GatherResolveRegisters(ReadRegisterHook, base);
    // The rectangle's three vertices (X draw_util.cc:782-800): six words at vertex fetch constant 0's address.
    uint32_t vertexGuest = 0;
    if ((regs.fetch0 & 3) == 3 && GuestRange(ResolveVertexAddress(regs.fetch0), sizeof regs.vertexWords, vertexGuest, error))
        std::memcpy(regs.vertexWords, base + vertexGuest, sizeof regs.vertexWords);
    else if ((regs.fetch0 & 3) == 3)
        return Fail(error, "resolve vertices: " + error);
    const edram::ResolveInfo info = edram::DecodeResolve(regs);  // refuses a fetch constant that is not a 6-word vertex fetch
    if (!info.ok)
        return Fail(error, info.refusal);
    if (info.empty)  // nothing to copy or clear, as Xenia (edram.h ResolveInfo::empty)
        return true;
    uint32_t destGuest = 0;
    // The path (edram_vk_plan.h): a 1x bit copy for X's fast case, else X's full resolve on the CPU over the guest words of
    // every sample, which a multisampled or 7e3 (R16G16B16A16_SFLOAT host) source gets through the sample dump. A 1x 16_16
    // source is copied directly: R16G16_SNORM's texel bytes are R then G, little-endian, i.e. the guest word r | g << 16.
    // A depth source always goes through the depth sample dump: Vulkan copies a D32_SFLOAT_S8_UINT image's aspects
    // separately (float depth, 8-bit stencil), not as the guest's packed word.
    const uint32_t storage = info.source.Format();  // a depth source: its depth format (0 kD24S8, 1 kD24FS8)
    const bool depthCopy = info.copy && info.isDepth;
    const bool fast = info.copy && !depthCopy && ResolveUsesFastPath(info, storage);
    const bool dumpPath = info.copy && !fast && (depthCopy || info.msaa != Msaa::X1 || storage == uint32_t(edram::ColorFormat::k2_10_10_10_Float));
    const DumpLayout layout = DumpLayoutFor(info.msaa, info.x1 - info.x0, info.y1 - info.y0);
    if (info.copy)
    {
        if (depthCopy ? !ResolveDepthConvertible(info, error) : !fast && !ResolveFullConvertible(info, storage, error))
            return false;
        if (uint64_t(layout.width) * layout.height * layout.samples * 4 > kStagingBytes)
            return Fail(error, "a resolve of " + std::to_string(layout.width) + "x" + std::to_string(layout.height) + " with " +
                                   std::to_string(layout.samples) + " sample(s) exceeds the staging buffer");
        if (dumpPath && (layout.width * layout.columns > kDumpWidth || layout.height * layout.rows > kDumpHeight))
            return Fail(error, "a resolve of " + std::to_string(layout.width) + "x" + std::to_string(layout.height) + " with " +
                                   std::to_string(layout.samples) + " samples exceeds the sample dump image");
        if (!GuestRange(info.destExtentStart, info.destExtentLength, destGuest, error))
            return Fail(error, "resolve destination: " + error);
    }
    const edram::Ownership before = unit->ownership;
    const edram::ResolvePlan plan = edram::PrepareResolve(unit->ownership, info);
    if (!plan.ok)
        return Fail(error, plan.refusal);
    auto refuse = [&](std::string why) { unit->ownership = before; return Fail(error, std::move(why)); };
    if ((plan.copy && !CheckTransfers(plan.copySource, plan.copyTransfers, error)) ||
        (plan.hasDepthClear && !CheckTransfers(plan.depthClear.target, plan.depthClear.transfers, error)) ||
        (plan.hasColorClear && !CheckTransfers(plan.colorClear.target, plan.colorClear.transfers, error)))
        return refuse(error);
    // The copy (X vulkan_render_target_cache.cc:1033-1200 order: the copy, then the clears).
    const VkDeviceSize offset = (unit->stagingUsed + 255) & ~VkDeviceSize(255);
    const VkDeviceSize wordBytes = VkDeviceSize(layout.width) * layout.height * layout.samples * 4;
    bool converted = false, check = false;
    VkDeviceSize checkOffset = 0;
    if (plan.copy)
    {
        Target* source = GetTarget(c, plan.copySource, error);
        if (source == nullptr || !ExecuteTransfers(c, *source, plan.copyTransfers, error))
            return false;
        const edram::Rect& r = plan.copyRect;
        if (r.x + r.width > source->width || r.y + r.height > source->height)
            return Fail(error, "resolve rectangle outside the source's host target");
        if (!dumpPath)
        {
            Transition(c, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {int32_t(r.x), int32_t(r.y), 0};
            copy.imageExtent = {r.width, r.height, 1};  // bufferRowLength 0: tightly packed, as ResolveToGuest reads it
            copy.bufferOffset = offset;
            vkCmdCopyImageToBuffer(c, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, unit->staging, 1, &copy);
        }
        else if (!RecordDump(c, *source, depthCopy, storage, info.msaa, r, layout, offset, error))
            return false;
        // FM4 Recomp (FM4_RESOLVE_RESIDENT): the converted words for the texture unit; the read-back above stays for guest memory.
        // A fast (bit-copy) resolve converts too when the converting path takes it: for 8_8_8_8 -> 6/50 and 2_10_10_10 -> 7/54 at
        // 1x its unpack and pack are the identity, so the words are the fast path's before the endian swap (test_edram_vk
        // TestFastResidentWords; 21 of 34 race resolves were fast and stayed non-resident).
        std::string unconvertible;
        if (ResolveResident() && !depthCopy && (!fast || ResolveFullConvertible(info, storage, unconvertible)) &&
            ResidentConvertible(storage, info.destFormat, info.destBytesLog2) && r.width == info.x1 - info.x0 && r.height == info.y1 - info.y0 &&
            r.width <= kResidentWidth && r.height <= kResidentHeight)
        {
            if (!RecordResident(c, *source, storage, info, r, error))
                return false;
            converted = true;
            if (ResolveResidentCheck())
            {
                check = true;
                checkOffset = (offset + wordBytes + 255) & ~VkDeviceSize(255);
                VkBufferImageCopy words{};
                words.bufferOffset = checkOffset;
                words.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                words.imageExtent = {r.width, r.height, 1};
                OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE): RecordResident ended any open pass; explicit for the site check
                vkCmdCopyImageToBuffer(c, unit->resident.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, unit->staging, 1, &words);
            }
        }
        VkBufferMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = unit->staging;
        host.size = VK_WHOLE_SIZE;
        OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        Rest(c, *source);
    }
    for (const edram::ResolveClear* clear : {plan.hasDepthClear ? &plan.depthClear : nullptr, plan.hasColorClear ? &plan.colorClear : nullptr})
    {
        if (clear == nullptr)
            continue;
        Target* t = GetTarget(c, clear->target, error);
        if (t == nullptr || !ExecuteTransfers(c, *t, clear->transfers, error) || !Clear(c, *t, clear->rect, clear->value, error))
            return false;
    }
    if (plan.copy)
    {
        unit->pending.push_back({info, storage, fast, depthCopy, offset, check, checkOffset});
        unit->stagingUsed = (check ? checkOffset + VkDeviceSize(info.x1 - info.x0) * (info.y1 - info.y0) * 4 : offset + wordBytes);
        // Recorded: the range is stale and pending for the texture unit until the slot retires (guest_writes.h).
        unit->observer(info.destExtentStart, info.destExtentLength);
        ResidentResolve& l = unit->last;
        l = {};
        l.converted = converted;
        l.extentStart = info.destExtentStart, l.extentLength = info.destExtentLength;
        l.textureBase = regs.copyDestBase, l.destFormat = info.destFormat, l.destPitch = info.destPitch, l.destEndian = info.destEndian;
        l.x0 = info.x0, l.y0 = info.y0, l.width = info.x1 - info.x0, l.height = info.y1 - info.y0;
        unit->lastValid = true;
    }
    unit->resolves++;
    return true;
}

// FM4 Recomp (FM4_RESOLVE_RESIDENT_CHECK): the GPU words of a resident resolve against ResolveConvertPixel of the same samples.
void CheckResident(const Pending& p)
{
    const uint8_t* staging = static_cast<const uint8_t*>(unit->stagingMapped);
    const uint32_t* words = reinterpret_cast<const uint32_t*>(staging + p.offset);
    const uint32_t* gpu = reinterpret_cast<const uint32_t*>(staging + p.checkOffset);
    const uint32_t width = p.info.x1 - p.info.x0, height = p.info.y1 - p.info.y0;
    const DumpLayout l = DumpLayoutFor(p.info.msaa, width, height);
    uint64_t mismatches = 0;
    std::string first;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            uint32_t samples[4] = {};
            for (uint32_t g = 0; g < l.samples; g++)
                samples[g] = words[l.Word(x, y, g)];
            const uint32_t want = ResolveConvertPixel(p.info, p.storage, samples), got = gpu[size_t(y) * width + x];
            if (want != got && mismatches++ < 4)
            {
                char text[96];
                std::snprintf(text, sizeof text, " (%u,%u) GPU %08X CPU %08X", x, y, got, want);
                first += text;
            }
        }
    unit->checkedResolves++, unit->checkedPixels += uint64_t(width) * height, unit->checkMismatches += mismatches;
    static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, bool>> named;  // once per (format pair, size, outcome)
    if (named.insert({p.storage, p.info.destFormat, width, height, mismatches == 0}).second || mismatches != 0)
        std::fprintf(stderr, "[edram] resident check: resolve of storage %u to format %u at %08X, %ux%u at %u,%u (%u sample(s), exp bias %d, swap %d): "
                             "%llu of %u pixel(s) differ from the CPU conversion%s\n",
                     p.storage, p.info.destFormat, p.info.destExtentStart, width, height, p.info.x0, p.info.y0, l.samples, p.info.destExpBias,
                     int(p.info.destSwap), (unsigned long long)mismatches, width * height, first.c_str());
}

bool Retired(std::string& error)
{
    // In record order: overlapping destinations end as the last resolve wrote them.
    for (const Pending& p : unit->pending)
    {
        uint32_t guest = 0;
        if (!GuestRange(p.info.destExtentStart, p.info.destExtentLength, guest, error))
            return Fail(error, "resolve destination: " + error);
        if (p.check)
            CheckResident(p);
        // A plain CPU store through main.cpp's writable base, before the renderer calls GuestWritesRetired. (No write-watch
        // fault hook is installed, guest_memory.h SetGuestWriteFaultHook; once one is, a watched page faults
        // here on the CP thread: an open question.)
        const uint32_t* words = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(unit->stagingMapped) + p.offset);
        const bool ok = p.depth  ? ResolveDepthToGuest(p.info, words, unit->guestBase + guest, p.info.destExtentLength, error)
                        : p.fast ? ResolveToGuest(p.info, p.storage, words, unit->guestBase + guest, p.info.destExtentLength, error)
                                 : ResolveConvertToGuest(p.info, p.storage, words, unit->guestBase + guest, p.info.destExtentLength, error);
        if (!ok)
            return false;
    }
    unit->pending.clear();
    unit->stagingUsed = 0;
    return true;
}

bool LastResolve(ResidentResolve& out, VkImage& words)
{
    if (!unit->lastValid)
        return false;
    out = unit->last;
    words = out.converted ? unit->resident.image : VK_NULL_HANDLE;
    return true;
}

bool ResolveRoom() { return unit->stagingUsed + 2 * kStagingBytes <= kStagingTotalBytes; }

void Shutdown()
{
    if (!unit)
        return;
    const VkDevice device = unit->dev.device;
    std::fprintf(stderr, "[edram] shutdown: %zu host target(s), %llu transfer(s) in %llu draw(s), %llu resolve(s)\n", unit->targets.size(),
                 (unsigned long long)unit->transfers, (unsigned long long)unit->transferDraws, (unsigned long long)unit->resolves);
    if (ResolveResident())
        std::fprintf(stderr, "[edram] resident resolves: %llu converted on the GPU; check: %llu resolve(s), %llu pixel(s), %llu mismatch(es)%s\n",
                     (unsigned long long)unit->residentResolves, (unsigned long long)unit->checkedResolves, (unsigned long long)unit->checkedPixels,
                     (unsigned long long)unit->checkMismatches, ResolveResidentCheck() ? "" : " (FM4_RESOLVE_RESIDENT_CHECK off)");
    for (auto& [key, p] : unit->pipelines)
        vkDestroyPipeline(device, p, nullptr);
    for (auto& [key, t] : unit->targets)
    {
        vkDestroyFramebuffer(device, t->framebuffer, nullptr);
        vkDestroyFramebuffer(device, t->uintFramebuffer, nullptr);
        vkDestroyImageView(device, t->uintView, nullptr);
        vkDestroyImageView(device, t->stencilView, nullptr);
        vkDestroyImageView(device, t->depthView, nullptr);
        vkDestroyImageView(device, t->view, nullptr);
        vkDestroyImage(device, t->image, nullptr);
        vkFreeMemory(device, t->memory, nullptr);
    }
    for (auto& [key, p] : unit->passes)
        vkDestroyRenderPass(device, p, nullptr);
    vkDestroyDescriptorPool(device, unit->pool, nullptr);  // frees the sets
    vkDestroyPipelineLayout(device, unit->layout, nullptr);
    vkDestroyDescriptorSetLayout(device, unit->setLayout, nullptr);
    vkDestroySampler(device, unit->sampler, nullptr);
    for (Target* t : {&unit->dump, &unit->resident})
    {
        vkDestroyFramebuffer(device, t->framebuffer, nullptr);
        vkDestroyImageView(device, t->view, nullptr);
        vkDestroyImage(device, t->image, nullptr);
        vkFreeMemory(device, t->memory, nullptr);
    }
    for (VkShaderModule m : {unit->vertex, unit->colorFrom1x, unit->colorFrom4x, unit->depthFrom1x, unit->stencilFrom1x, unit->depthToColorFrom1x, unit->depthToColorFromMs,
                             unit->dumpFrom1x, unit->dumpFromMs, unit->dumpDepth1x, unit->dumpDepthMs, unit->depthFromMs, unit->stencilFromMs, unit->colorToDepth1x, unit->colorToDepthMs,
                             unit->colorToStencil1x, unit->colorToStencilMs, unit->colorUint[0][0][1], unit->colorUint[0][1][0],
                             unit->colorUint[0][1][1], unit->colorUint[1][0][1], unit->colorUint[1][1][0], unit->colorUint[1][1][1],
                             unit->depthToColorUint[0], unit->depthToColorUint[1], unit->colorToDepthUint[0], unit->colorToDepthUint[1],
                             unit->colorToStencilUint[0], unit->colorToStencilUint[1], unit->dumpUint[0], unit->dumpUint[1], unit->resident1x, unit->residentMs})
        vkDestroyShaderModule(device, m, nullptr);
    vkDestroyBuffer(device, unit->staging, nullptr);
    vkFreeMemory(device, unit->stagingMemory, nullptr);  // implicitly unmapped
    unit.reset();
}

const EdramHooks kHooks{AcquireTargets, Resolve, Retired, Shutdown, LastResolve, ResolveRoom};

bool Module(SpirvCompiler& compiler, bool pixel, const std::string& glsl, VkShaderModule& out, std::string& error)
{
    std::vector<uint32_t> spirv;
    std::string warnings;
    if (!compiler.Compile(pixel, glsl, spirv, warnings, error))
        return Fail(error, "EDRAM transfer shader: " + error);
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * 4;
    info.pCode = spirv.data();
    return Ok(vkCreateShaderModule(unit->dev.device, &info, nullptr, &out), "vkCreateShaderModule (EDRAM)", error);
}

bool Create(SpirvCompiler& compiler, std::string& error)
{
    const VkDevice device = unit->dev.device;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(unit->dev.gpu, &properties);
    unit->limits = properties.limits;
    // What the targets and transfers need, checked once (never assumed on MoltenVK).
    const struct { VkFormat format; VkFormatFeatureFlags needed; const char* name; } formats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "R8G8B8A8_UNORM"},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "A2B10G10R10_UNORM_PACK32"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "R16G16B16A16_SFLOAT"},  // storage 3 (7e3)
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "R32_SFLOAT"},                    // storage 14 (32_FLOAT)
        {VK_FORMAT_R16G16_SNORM, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
         "R16G16_SNORM"},  // storage 4 (16_16), blended as X's host path
        {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "D32_SFLOAT_S8_UINT"},
        {VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "R32_UINT"},  // the resolve's sample dump, storage 14's transfer view
        {VK_FORMAT_R16G16_UINT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "R16G16_UINT"},  // storage 4's transfer view (X :1637-1639)
    };
    for (const auto& f : formats)
    {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(unit->dev.gpu, f.format, &p);
        const VkFormatFeatureFlags needed =
            f.needed | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((p.optimalTilingFeatures & needed) != needed)
            return Fail(error, std::string("EDRAM: ") + f.name + " lacks attachment, sampled or transfer support");
    }
    const VkPhysicalDeviceLimits& l = unit->limits;
    const VkSampleCountFlags msaa = VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
    if ((l.framebufferColorSampleCounts & msaa) != msaa || (l.framebufferDepthSampleCounts & msaa) != msaa ||
        (l.framebufferStencilSampleCounts & msaa) != msaa || (l.sampledImageColorSampleCounts & msaa) != msaa)
        return Fail(error, "EDRAM: 2x/4x framebuffers or 2x/4x sampled colour images are not supported");
    // Depth transfers from a multisampled source (depth -> depth, depth -> colour) sample its depth and stencil aspects
    // per sample (sampler2DMS / usampler2DMS on the D32_SFLOAT_S8_UINT depth and stencil views; X :2307-2358).
    if ((l.sampledImageDepthSampleCounts & msaa) != msaa || (l.sampledImageStencilSampleCounts & msaa) != msaa)
        return Fail(error, "EDRAM: 2x/4x sampled depth or stencil images are not supported (sampledImageDepthSampleCounts / sampledImageStencilSampleCounts)");
    // The transfer's texel mapping assumes Vulkan's standard 4x sample locations (sample bit 0 horizontal, bit 1
    // vertical: X vulkan_render_target_cache.cc:2597-2601); a device without them would reinterpret samples wrongly.
    if (!l.standardSampleLocations)
        return Fail(error, "EDRAM: the device does not use the standard sample locations (standardSampleLocations false)");
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!Ok(vkCreateSampler(device, &sampler, nullptr, &unit->sampler), "vkCreateSampler (EDRAM)", error))
        return false;
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // colour or depth
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // stencil (depth sources only)
    };
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (!Ok(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &unit->setLayout), "vkCreateDescriptorSetLayout (EDRAM)", error))
        return false;
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TransferPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &unit->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (!Ok(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &unit->layout), "vkCreatePipelineLayout (EDRAM)", error))
        return false;
    // Two sets per target at most (a storage-14 target's R32_UINT source set), two samplers per set.
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kMaxTargets};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 2 * kMaxTargets;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!Ok(vkCreateDescriptorPool(device, &pool, nullptr, &unit->pool), "vkCreateDescriptorPool (EDRAM)", error))
        return false;
    const std::string header = "#version 450\n", ms = "#define XE_SOURCE_MS 1\n", swap = "#define XE_SWAP_COLUMNS 1\n",
                      bit = "#define XE_STENCIL_BIT 1\n";
    if (!Module(compiler, false, kTransferVertex, unit->vertex, error) ||
        !Module(compiler, true, header + kTransferCommon + kTransferPacking + kTransferColor, unit->colorFrom1x, error) ||
        !Module(compiler, true, header + ms + kTransferCommon + kTransferPacking + kTransferColor, unit->colorFrom4x, error) ||
        !Module(compiler, true, header + kTransferCommon + kDepthPacking + kDepthUnpacking + kTransferDepth, unit->depthFrom1x, error) ||
        !Module(compiler, true, header + kTransferCommon + kTransferStencilBit, unit->stencilFrom1x, error) ||
        !Module(compiler, true, header + swap + kTransferCommon + kTransferPacking + kDepthPacking + kTransferDepthToColor, unit->depthToColorFrom1x, error) ||
        !Module(compiler, true, header + ms + swap + kTransferCommon + kTransferPacking + kDepthPacking + kTransferDepthToColor, unit->depthToColorFromMs, error) ||
        !Module(compiler, true, header + kTransferCommon + kTransferPacking + kDumpTexel + kResolveDump, unit->dumpFrom1x, error) ||
        !Module(compiler, true, header + ms + kTransferCommon + kTransferPacking + kDumpTexel + kResolveDump, unit->dumpFromMs, error) ||
        !Module(compiler, true, header + kTransferCommon + kDepthPacking + kDumpTexel + kResolveDumpDepth, unit->dumpDepth1x, error) ||
        !Module(compiler, true, header + ms + kTransferCommon + kDepthPacking + kDumpTexel + kResolveDumpDepth, unit->dumpDepthMs, error) ||
        !Module(compiler, true, header + ms + kTransferCommon + kDepthPacking + kDepthUnpacking + kTransferDepth, unit->depthFromMs, error) ||
        !Module(compiler, true, header + ms + kTransferCommon + kTransferStencilBit, unit->stencilFromMs, error) ||
        !Module(compiler, true, header + swap + kTransferCommon + kTransferPacking + kDepthUnpacking + kTransferColorToDepth, unit->colorToDepth1x, error) ||
        !Module(compiler, true, header + ms + swap + kTransferCommon + kTransferPacking + kDepthUnpacking + kTransferColorToDepth, unit->colorToDepthMs, error) ||
        !Module(compiler, true, header + swap + bit + kTransferCommon + kTransferPacking + kDepthUnpacking + kTransferColorToDepth, unit->colorToStencil1x, error) ||
        !Module(compiler, true, header + ms + swap + bit + kTransferCommon + kTransferPacking + kDepthUnpacking + kTransferColorToDepth, unit->colorToStencilMs, error))
        return false;
    // The colour transfers with a 32_FLOAT or 16_16 side (kTransferColor's XE_SOURCE_UINT / XE_DEST_UINT; [0/1][0][0] are colorFrom1x/4x).
    unit->colorUint[0][0][0] = unit->colorFrom1x, unit->colorUint[1][0][0] = unit->colorFrom4x;
    for (uint32_t m = 0; m < 2; m++)
        for (uint32_t su = 0; su < 2; su++)
            for (uint32_t du = 0; du < 2; du++)
                if ((su || du) && !Module(compiler, true,
                                          header + (m ? ms : "") + (su ? "#define XE_SOURCE_UINT 1\n" : "") + (du ? "#define XE_DEST_UINT 1\n" : "") +
                                              kTransferCommon + kTransferPacking + kTransferColor,
                                          unit->colorUint[m][su][du], error))
                    return false;
    // The cross-aspect transfers through a 16_16 transfer view (kTransferDepthToColor XE_DEST_UINT, kTransferColorToDepth
    // XE_SOURCE_UINT), 1x and multisampled sources.
    const std::string sourceUint = "#define XE_SOURCE_UINT 1\n", destUint = "#define XE_DEST_UINT 1\n";
    for (uint32_t m = 0; m < 2; m++)
        if (!Module(compiler, true, header + (m ? ms : "") + swap + destUint + kTransferCommon + kTransferPacking + kDepthPacking + kTransferDepthToColor,
                    unit->depthToColorUint[m], error) ||
            !Module(compiler, true, header + (m ? ms : "") + swap + sourceUint + kTransferCommon + kTransferPacking + kDepthUnpacking + kTransferColorToDepth,
                    unit->colorToDepthUint[m], error) ||
            !Module(compiler, true, header + (m ? ms : "") + swap + bit + sourceUint + kTransferCommon + kTransferPacking + kDepthUnpacking + kTransferColorToDepth,
                    unit->colorToStencilUint[m], error) ||
            !Module(compiler, true, header + (m ? ms : "") + sourceUint + kTransferCommon + kTransferPacking + kDumpTexel + kResolveDump, unit->dumpUint[m], error))
            return false;
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): compiled only when the run asks for it.
    if (ResolveResident() && (!Module(compiler, true, header + kTransferCommon + kTransferPacking + kResolveResident, unit->resident1x, error) ||
                              !Module(compiler, true, header + ms + kTransferCommon + kTransferPacking + kResolveResident, unit->residentMs, error)))
        return false;
    // The sample dump image (kResolveDump): R32_UINT, rendered then copied to the staging buffer; held for the run. FM4 Recomp: the
    // resident image (kResolveResident; FM4_RESOLVE_RESIDENT only) the same way, at kResidentWidth x kResidentHeight.
    for (Target* t : {&unit->dump, ResolveResident() ? &unit->resident : nullptr})
    {
        if (t == nullptr)
            continue;
        Target& d = *t;
        d.format = VK_FORMAT_R32_UINT, d.samples = 1;
        d.width = t == &unit->dump ? kDumpWidth : kResidentWidth, d.height = t == &unit->dump ? kDumpHeight : kResidentHeight;
        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = d.format;
        image.extent = {d.width, d.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!Ok(vkCreateImage(device, &image, nullptr, &d.image), "vkCreateImage (EDRAM dump)", error))
            return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, d.image, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = MemoryType(unit->dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkRenderPass pass = VK_NULL_HANDLE;
        if (!Ok(vkAllocateMemory(device, &alloc, nullptr, &d.memory), "vkAllocateMemory (EDRAM dump)", error) ||
            !Ok(vkBindImageMemory(device, d.image, d.memory, 0), "vkBindImageMemory (EDRAM dump)", error) ||
            !View(d.image, d.format, VK_IMAGE_ASPECT_COLOR_BIT, d.view, error) || !Pass(d.format, 1, false, pass, error))
            return false;
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &d.view;
        fb.width = d.width, fb.height = d.height, fb.layers = 1;
        if (!Ok(vkCreateFramebuffer(device, &fb, nullptr, &d.framebuffer), "vkCreateFramebuffer (EDRAM dump)", error))
            return false;
    }
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = kStagingTotalBytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;  // SRC: the self-check uploads through it
    if (!Ok(vkCreateBuffer(device, &buffer, nullptr, &unit->staging), "vkCreateBuffer (EDRAM staging)", error))
        return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, unit->staging, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        MemoryType(unit->dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    return Ok(vkAllocateMemory(device, &allocate, nullptr, &unit->stagingMemory), "vkAllocateMemory (EDRAM staging)", error) &&
           Ok(vkBindBufferMemory(device, unit->staging, unit->stagingMemory, 0), "vkBindBufferMemory (EDRAM staging)", error) &&
           Ok(vkMapMemory(device, unit->stagingMemory, 0, kStagingTotalBytes, 0, &unit->stagingMapped), "vkMapMemory (EDRAM staging)", error);
}

}  // namespace

// ---- The isolated GPU check of the 4x -> 2x depth transfer (FM4_SELFCHECK_EDRAM=depth4x2x) -------------------------

bool RecordDepthSelfCheck(VkCommandBuffer c, std::string& error)
{
    if (!unit)
        return Fail(error, "the EDRAM unit is not initialized");
    // edram_vk_plan.h DepthSelfCheck*: known words in the 1x key, transferred 1x -> 4x (the observed path) and 4x -> 2x
    // (the new one) over tiles 1024-1055 (two rows of 16), every sample of both dumped through the depth resolve's dump.
    const Key k1 = Key::Depth(1024, 16, Msaa::X1, edram::DepthFormat::D24FS8), k4 = Key::Depth(1024, 16, Msaa::X4, edram::DepthFormat::D24FS8),
              k2 = Key::Depth(1024, 16, Msaa::X2, edram::DepthFormat::D24FS8);
    Target* t1 = GetTarget(c, k1, error);
    Target* t4 = t1 ? GetTarget(c, k4, error) : nullptr;
    Target* t2 = t4 ? GetTarget(c, k2, error) : nullptr;
    if (t2 == nullptr)
        return false;
    if (t1->width != kDepthSelfCheckWidth || t4->width != kDepthSelfCheckWidth / 2 || t2->width != kDepthSelfCheckWidth)
        return Fail(error, "self-check: unexpected host target widths");
    const uint32_t w = kDepthSelfCheckWidth, rows = kDepthSelfCheckRows1x;
    float* depth = static_cast<float*>(unit->stagingMapped);
    uint8_t* stencil = static_cast<uint8_t*>(unit->stagingMapped) + size_t(w) * rows * 4;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t x = 0; x < w; x++)
            depth[size_t(r) * w + x] = DepthSelfCheckDepth(x, r), stencil[size_t(r) * w + x] = uint8_t(DepthSelfCheckStencil(x, r));
    // The upload: the depth aspect as 32-bit floats, the stencil aspect as bytes (Vulkan's buffer layouts of
    // D32_SFLOAT_S8_UINT aspects); the submission makes the host writes visible.
    Transition(c, *t1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy upload[2]{};
    upload[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    upload[0].imageExtent = {w, rows, 1};
    upload[1] = upload[0];
    upload[1].bufferOffset = VkDeviceSize(w) * rows * 4;
    upload[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    vkCmdCopyBufferToImage(c, unit->staging, t1->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, upload);
    Rest(c, *t1);
    if (!ExecuteTransfers(c, *t4, {{1024, 1056, k1}}, error) || !ExecuteTransfers(c, *t2, {{1024, 1056, k4}}, error))
        return false;
    const uint32_t format = uint32_t(edram::DepthFormat::D24FS8);
    if (!RecordDump(c, *t4, true, format, Msaa::X4, {0, 0, w / 2, rows / 2}, DumpLayoutFor(Msaa::X4, w / 2, rows / 2), kDepthSelfCheckDump4x, error) ||
        !RecordDump(c, *t2, true, format, Msaa::X2, {0, 0, w, rows / 2}, DumpLayoutFor(Msaa::X2, w, rows / 2), kDepthSelfCheckDump2x, error))
        return false;
    // FM4 Recomp: depth between formats: the 1x kD24FS8 key into the 1x kD24S8 key at the same
    // tiles, dumped as kD24S8, then back into the kD24FS8 key, dumped as kD24FS8 (edram_vk_plan.h ConvertDepthFormat).
    const Key k1s = Key::Depth(1024, 16, Msaa::X1, edram::DepthFormat::D24S8);
    Target* t1s = GetTarget(c, k1s, error);
    if (t1s == nullptr)
        return false;
    if (!ExecuteTransfers(c, *t1s, {{1024, 1056, k1}}, error) ||
        !RecordDump(c, *t1s, true, uint32_t(edram::DepthFormat::D24S8), Msaa::X1, {0, 0, w, rows}, DumpLayoutFor(Msaa::X1, w, rows),
                    kDepthSelfCheckDumpS8, error) ||
        !ExecuteTransfers(c, *t1, {{1024, 1056, k1s}}, error) ||
        !RecordDump(c, *t1, true, format, Msaa::X1, {0, 0, w, rows}, DumpLayoutFor(Msaa::X1, w, rows), kDepthSelfCheckDumpRoundTrip, error))
        return false;
    VkBufferMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = unit->staging;
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
    return true;
}

DepthSelfCheckResult VerifyDepthSelfCheck()
{
    DepthSelfCheckResult result;
    const uint32_t w = kDepthSelfCheckWidth, h = kDepthSelfCheckRows1x / 2;
    const uint8_t* staging = static_cast<const uint8_t*>(unit->stagingMapped);
    auto check = [&](const uint32_t* words, const DumpLayout& l, uint32_t samples, bool is4x, uint64_t& checked, uint64_t& mismatches) {
        for (uint32_t y = 0; y < l.height; y++)
            for (uint32_t x = 0; x < l.width; x++)
                for (uint32_t g = 0; g < samples; g++)
                {
                    const uint32_t got = words[l.Word(x, y, g)];
                    const uint32_t want = is4x ? DepthSelfCheckExpected4x(x, y, g) : DepthSelfCheckExpected2x(x, y, g);
                    checked++;
                    if (got != want && mismatches++ == 0)
                    {
                        char text[128];
                        std::snprintf(text, sizeof text, "%s (%u, %u) guest sample %u: %08X, expected %08X", is4x ? "4x" : "2x", x, y, g, got, want);
                        result.first = text;
                    }
                }
    };
    check(reinterpret_cast<const uint32_t*>(staging + kDepthSelfCheckDump4x), DumpLayoutFor(Msaa::X4, w / 2, h), 4, true, result.checked4x, result.mismatches4x);
    check(reinterpret_cast<const uint32_t*>(staging + kDepthSelfCheckDump2x), DumpLayoutFor(Msaa::X2, w, h), 2, false, result.checked2x, result.mismatches2x);
    // FM4 Recomp: the format change and the way back, every 1x pixel (32 rows).
    auto check1x = [&](uint64_t offset, bool roundTrip, uint64_t& checked, uint64_t& mismatches) {
        const DumpLayout l = DumpLayoutFor(Msaa::X1, w, kDepthSelfCheckRows1x);
        const uint32_t* words = reinterpret_cast<const uint32_t*>(staging + offset);
        for (uint32_t y = 0; y < l.height; y++)
            for (uint32_t x = 0; x < l.width; x++)
            {
                const uint32_t got = words[l.Word(x, y, 0)];
                const uint32_t want = roundTrip ? DepthSelfCheckExpectedRoundTrip(x, y) : DepthSelfCheckExpectedS8(x, y);
                checked++;
                if (got != want && mismatches++ == 0 && result.first.empty())
                {
                    char text[128];
                    std::snprintf(text, sizeof text, "%s (%u, %u): %08X, expected %08X", roundTrip ? "round trip" : "kD24S8", x, y, got, want);
                    result.first = text;
                }
            }
    };
    check1x(kDepthSelfCheckDumpS8, false, result.checkedS8, result.mismatchesS8);
    check1x(kDepthSelfCheckDumpRoundTrip, true, result.checkedRoundTrip, result.mismatchesRoundTrip);
    return result;
}

const EdramHooks* InitializeEdramVk(const VulkanDevice& device, SpirvCompiler& compiler, uint8_t* guestBase, ResolveWriteObserver observer,
                                    std::string& error)
{
    if (unit)
        return Fail(error, "the EDRAM unit is already initialized"), nullptr;
    if (guestBase == nullptr || observer == nullptr)
        return Fail(error, "the EDRAM unit needs the writable guest base and the guest-write observer"), nullptr;
    unit = std::make_unique<Unit>();
    unit->dev = device;
    unit->guestBase = guestBase;
    unit->observer = observer;
    if (!Create(compiler, error))
    {
        Shutdown();  // nothing was submitted: every created object is idle
        return nullptr;
    }
    return &kHooks;
}

}  // namespace runtime::video
