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
#include "guest_writes.h"  // FM4_RESOLVE_HOST_WRITE: GuestRangeHostWrite
#include "kernel/graphics.h"
#include "kernel/virtual_memory.h"
#include <cstdio>
#include <algorithm>
#include <deque>
#include <mutex>
#include <cstring>
#include <map>
#include <memory>
#include <cstdlib>
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
    uint pitchTiles;    // the destination key's pitch in tiles, Key::PitchTiles (X :2516-2577: it splits the destination tile index)
    uint tileOffset;    // (destination base - source base) & 2047
    uint sourceMsaa;    // Xenos msaa_samples: 0 1x, 1 2x, 2 4x
    uint destMsaa;
    uint x0, y0, width, height;  // the sample dump (kResolveDump): the source rectangle
    uint sourcePitchTiles;       // the source key's pitch in tiles, Key::PitchTiles (X :2956-2965: it splits the source tile index)
} xe_transfer;
// FM4 Recomp (alpha, RT format 5): XE_SOURCE_64 / XE_DEST_64 name a 64 bpp side (storage 5; kTransferColor64 only). A key's pitch
// is then doubled (Key::PitchTiles, X :4823, :5223-5224) and its tile 40 samples wide (X :2540-2545, :2912-2913).
#ifdef XE_SOURCE_64
const uint xeSource64 = 1u;
#else
const uint xeSource64 = 0u;
#endif
#ifdef XE_DEST_64
const uint xeDest64 = 1u;
#else
const uint xeDest64 = 0u;
#endif
// FM4 Recomp (alpha, RT format 5): 64 -> 32 bpp, the half of the source sample that is the destination's word (XeSourceTexel sets
// it; edram_vk_plan.h TransferSource::half).
uint xe_half = 0u;
// The pixel size of a tile (80x16 samples at 32 bpp): 1x 80x16, 2x 80x8, 4x 40x8 (X render_target_cache.cc:355-359); FM4 Recomp
// (alpha, RT format 5): half as wide at 64 bpp (40x16 samples: X vulkan_render_target_cache.cc:2540-2545).
uvec2 XeTileSize(uint msaa, uint is64)
{
    return uvec2((msaa == 2u ? 40u : 80u) >> is64, msaa == 0u ? 16u : 8u);
}
// FM4 Recomp (alpha, RT format 5): X :2833-2903, the common Y and sample of a 1x/2x source of another sample count (native 2x host
// samples), for the pairs with one 64 bpp side (the same-bpp chain in XeSourceTexel has it inline).
void XeCommonY(uint sm, uint dm, uint ds, uint py, inout uint y, inout uint s)
{
    if (sm == 2u || sm == dm)
        return;
    if (dm == 2u)
    {
        if (sm == 1u)
            s = ((ds >> 1u) & 1u) ^ 1u;
        else
            y = (py << 1u) | ((ds >> 1u) & 1u);
    }
    else if (sm == 1u)
    {
        s = (py & 1u) ^ 1u;
        y = py >> 1u;
    }
    else
        y = (py << 1u) | ((ds ^ 1u) & 1u);
}
// edram_vk_plan.cpp MapEdramTexel without the colour/depth column swap: X vulkan_render_target_cache.cc:2516-2577
// (destination tile and tile pixel); the MSAA rule on the tile pixel, MapTransferTexel (:2597-2601; 4x -> 1x :2804-2818;
// 1x -> 4x :2823-2828, :2857-2864; 4x -> 2x :2786-2802; 2x -> 4x :2823-2828, :2843-2849; 2x <-> 1x :2868-2898, native 2x host samples);
// :2938-2954 (source tile = destination tile + tileOffset, & 2047). xy the source pixel, z its host sample.
ivec3 XeSourceTexel(uvec2 d)
{
#ifdef XE_SAMPLE_SHADING
    uint sm = xe_transfer.sourceMsaa, dm = xe_transfer.destMsaa, ds = uint(gl_SampleID);  // one draw for every destination sample
#else
    uint sm = xe_transfer.sourceMsaa, dm = xe_transfer.destMsaa, ds = xe_transfer.destSample;
#endif
    uvec2 tile = d / XeTileSize(dm, xeDest64), p = d % XeTileSize(dm, xeDest64);
    uint s = dm == 0u ? 0u : ds;
#if defined(XE_DEST_64) && !defined(XE_SOURCE_64)
    // FM4 Recomp (alpha, RT format 5): 32 -> 64 bpp (edram_vk_plan.cpp MapTransferSource; X :2607-2685), the low word's texel.
    uint py = p.y;
    if (sm == 2u)
    {
        if (dm == 2u)
        {
            s = ds & 2u;
            p.x = (p.x << 1u) | (ds & 1u);
        }
        else if (dm == 1u)
            s = ((ds ^ 1u) & 1u) << 1u;
        else
        {
            s = (py & 1u) << 1u;
            p.y = py >> 1u;
        }
    }
    else
        p.x = dm == 2u ? (p.x << 2u) | ((ds & 1u) << 1u) : p.x << 1u;
    uint y = p.y;  // a plain local as the inout argument
    XeCommonY(sm, dm, ds, py, y, s);
    p.y = y;
    if (sm == 0u)
        s = 0u;
#elif defined(XE_SOURCE_64) && !defined(XE_DEST_64)
    // FM4 Recomp (alpha, RT format 5): 64 -> 32 bpp (MapTransferSource; X :2686-2777) and the half holding the destination's word.
    uint px = p.x, py = p.y;
    if (dm == 2u)
    {
        if (sm == 2u)
        {
            s = (ds & ~1u) | (px & 1u);
            p.x = px >> 1u;
        }
        xe_half = ds & 1u;
    }
    else
    {
        if (sm == 2u)
        {
            s = (px >> 1u) & 1u;
            if (dm == 1u)
                s |= ((ds ^ 1u) & 1u) << 1u;
            else
            {
                s |= (py & 1u) << 1u;
                p.y = py >> 1u;
            }
            p.x = px >> 2u;
        }
        else
            p.x = px >> 1u;
        xe_half = px & 1u;
    }
    uint y = p.y;  // a plain local as the inout argument
    XeCommonY(sm, dm, ds, py, y, s);
    p.y = y;
    if (sm == 0u)
        s = 0u;
#else
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
#endif
#ifdef XE_SWAP_COLUMNS
    // Colour <-> depth: the 40-sample column swap within the source tile (X :2916-2936), half = 80 >> (1 + source 4x).
    uint halfTile = sm == 2u ? 20u : 40u;
    p.x = p.x < halfTile ? p.x + halfTile : p.x - halfTile;
#endif
    uint t = (tile.y * xe_transfer.pitchTiles + tile.x + xe_transfer.tileOffset) & 2047u;
    return ivec3(ivec2(uvec2(t % xe_transfer.sourcePitchTiles, t / xe_transfer.sourcePitchTiles) * XeTileSize(sm, xeSource64) + p), int(s));
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

// FM4 Recomp (alpha, RT format 5): a colour transfer with a 64 bpp side (storage 5, 16_16_16_16, read and written through its
// R16G16B16A16_UINT view: XE_SOURCE_64 implies XE_SOURCE_UINT, XE_DEST_64 XE_DEST_UINT). edram_vk_plan.cpp MapTransferSource,
// TransferWordOf64 and TransferTexelOf64 are the same arithmetic:
//   - 64 -> 64: the texel itself (X :3254-3273, the R16G16B16A16_UINT components passed through);
//   - 64 -> 32: the source sample's half xe_half as the guest word, r | g << 16 or b | a << 16 (X :3080-3102, :3545-3576), unpacked
//     into the destination's storage format as every 32 bpp transfer does (XeUnpack, XeUnpackUint);
//   - 32 -> 64: the low and the high word from two source texels (X :3005-3023: sample | 1 from a 4x source, x | 1 otherwise), each the
//     32 bpp source's guest word (XePack, XePackUint as in kTransferColor), split into the four 16-bit components (X :3352-3370).
const char* const kTransferColor64 = R"(
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
#ifndef XE_SOURCE_64
uint XeSourceWord(ivec2 xy, int s)
{
#ifdef XE_SOURCE_UINT
    return XePackUint(texelFetch(xe_source, xy, s), xe_transfer.sourceFormat);
#else
    return XePack(texelFetch(xe_source, xy, s), xe_transfer.sourceFormat);
#endif
}
#endif
void main()
{
    ivec3 t = XeSourceTexel(uvec2(gl_FragCoord.xy));
#ifdef XE_SOURCE_MS
    int s = t.z;
#else
    int s = 0;
#endif
#ifdef XE_SOURCE_64
    uvec4 v = texelFetch(xe_source, t.xy, s);
#ifdef XE_DEST_64
    xe_word = v;
#else
    uint word = xe_half != 0u ? (v.b & 0xFFFFu) | ((v.a & 0xFFFFu) << 16u) : (v.r & 0xFFFFu) | ((v.g & 0xFFFFu) << 16u);
#ifdef XE_DEST_UINT
    xe_word = XeUnpackUint(word, xe_transfer.destFormat);
#else
    xe_color = XeUnpack(word, xe_transfer.destFormat);
#endif
#endif
#else
    ivec2 t1 = t.xy;
    int s1 = s;
    if (xe_transfer.sourceMsaa == 2u)
        s1 = s | 1;
    else
        t1.x |= 1;
    uint w0 = XeSourceWord(t.xy, s), w1 = XeSourceWord(t1, s1);
    xe_word = uvec4(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu, w1 >> 16u);
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

// FM4 Recomp (FM4_TRANSFER_STENCIL_EXPORT): the depth transfers in one draw: gl_FragDepth as kTransferDepth / kTransferColorToDepth
// and all eight stencil bits through gl_FragStencilRefARB (GL_ARB_shader_stencil_export -> SPV_EXT_shader_stencil_export ->
// Metal [[stencil]]), the pipeline replacing with write mask 0xFF, so no stencil clear and no per-bit pass. The values written
// are those of the nine-pass path (render_targets_vk.h 4 anticipated this; X vulkan_render_target_cache.cc uses the extension
// the same way when available). With XE_SAMPLE_SHADING (sampleRateShading) one draw covers every destination sample.
const char* const kTransferDepthStencilExport = R"(
#ifdef XE_SOURCE_MS
layout(set = 0, binding = 0) uniform sampler2DMS xe_source_depth;
layout(set = 0, binding = 1) uniform usampler2DMS xe_source_stencil;
#else
layout(set = 0, binding = 0) uniform sampler2D xe_source_depth;
layout(set = 0, binding = 1) uniform usampler2D xe_source_stencil;
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
    gl_FragDepth = xe_transfer.sourceFormat == xe_transfer.destFormat
                       ? depth
                       : XeWordDepth(XeDepthStencilWord(depth, 0u, xe_transfer.sourceFormat), xe_transfer.destFormat);
    gl_FragStencilRefARB = int(stencil);
}
)";
const char* const kTransferColorToDepthStencilExport = R"(
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
    ivec3 t = XeSourceTexel(uvec2(gl_FragCoord.xy));
#ifdef XE_SOURCE_MS
    int s = t.z;
#else
    int s = 0;
#endif
#ifdef XE_SOURCE_UINT
    uint word = XePackUint(texelFetch(xe_source, t.xy, s), xe_transfer.sourceFormat);
#else
    uint word = XePack(texelFetch(xe_source, t.xy, s), xe_transfer.sourceFormat);
#endif
    gl_FragDepth = XeWordDepth(word, xe_transfer.destFormat);
    gl_FragStencilRefARB = int(word & 0xFFu);
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

// FM4 Recomp (FM4_RESOLVE_RESIDENT_DEPTH, resolve_resident.h): a depth resolve's resident words: one R32_UINT fragment per pixel of
// the resolve rectangle holding XeDepthStencilWord of the one selected guest sample (edram.cpp DecodeResolve leaves exactly one for
// depth; edram_vk_plan.cpp ResolveDepthToGuest stores words[l.Word(x, y, indices[0])]), i.e. the word the CPU write-back lands in
// guest memory before its endian swap. The push constants reuse TransferPush: destSample the guest sample index (2 bits), sourceFormat
// the depth format, sourceMsaa, x0 y0 the rectangle in the source.
const char* const kResolveResidentDepth = R"(
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
    ivec2 p = ivec2(uvec2(gl_FragCoord.xy) + uvec2(xe_transfer.x0, xe_transfer.y0));
    uint g = xe_transfer.destSample & 3u;
    // The host sample of guest sample g (edram_vk_plan.cpp GuestSampleOfHost, its own inverse; kDumpTexel).
    uint h = xe_transfer.sourceMsaa == 2u ? ((g & 1u) << 1u) | ((g >> 1u) & 1u) : xe_transfer.sourceMsaa == 1u ? (g ^ 1u) : 0u;
#ifdef XE_SOURCE_MS
    float depth = texelFetch(xe_source_depth, p, int(h)).r;
    uint stencil = texelFetch(xe_source_stencil, p, int(h)).r;
#else
    float depth = texelFetch(xe_source_depth, p, 0).r;
    uint stencil = texelFetch(xe_source_stencil, p, 0).r;
#endif
    xe_word = XeDepthStencilWord(depth, stencil, xe_transfer.sourceFormat);
}
)";

// FM4 Recomp (FM4_RESOLVE_RESIDENT, resolve_resident.h): one R32_UINT fragment per pixel of the resolve rectangle, the word
// edram_vk_plan.cpp ResolveConvertPixel computes (X resolve.xesli:273-321, :651-704; pixel_formats.xesli:116-125) from the
// guest words of its selected samples, each the word the sample dump would hold (XePack of the host sample, kResolveDump), so
// both paths start from the same words. The push constants reuse TransferPush: destSamples4 the sample count, destSample the
// guest sample indices (2 bits each, ResolveSamples), destFormat the texture format (6, 7, 10, 25, 50, 54), stencilBit the
// dest_exp_bias_factor's float bits, pitchTiles the red/blue swap, tileOffset the destination number format (k_16_16's PackFixed16),
// x0 y0 the rectangle in the source. `precise`: no fused multiply-add, as the CPU's separate float multiply and add.
// FM4 Recomp (M7 CPU cost, FM4_RESOLVE_RESIDENT_1616): XE_SOURCE_UINT reads a 16_16 source (storage 4) raw through its R16G16_UINT
// transfer view as the sample dump does (kResolveDump: the SNORM view would lose 0x8000 and round int16 / 32767 before the scale), so
// the word is XePackUint's, exactly the dump word the CPU path unpacks.
const char* const kResolveResident = R"(
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
uint XeResolveUnorm(float v, float scale)
{
    precise float s = v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0;
    precise float t = s * scale + 0.5;
    return uint(t);
}
// FM4 Recomp (M7 CPU cost): edram_vk_plan.cpp UnpackResolveSample's 16_16 case, operation for operation: each half of the guest word as
// int16 (red the low half, X pixel_formats.xesli:367-371; bitfieldExtract on int sign-extends) times 32 / 32767 as one float multiply
// (0x3A800100 is the float nearest 32 / 32767, the CPU's `32.0f / 32767.0f`; written as bits so no constant folding is involved), then
// clamped at -32 (ShiftGlue 72b7dd6f fh1_native_edram.hlsli:217-220 DecodeSnorm16: both -1 encodings 0x8000 and 0x8001 are -32); blue and
// alpha 0. The inputs are finite, so `v > -32.0 ? v : -32.0` is the CPU's comparison exactly.
vec4 XeResolveUnpack1616(uint w)
{
    precise float r = float(bitfieldExtract(int(w), 0, 16)) * uintBitsToFloat(0x3A800100u);
    precise float g = float(bitfieldExtract(int(w), 16, 16)) * uintBitsToFloat(0x3A800100u);
    return vec4(r > -32.0 ? r : -32.0, g > -32.0 ? g : -32.0, 0.0, 0.0);
}
// FM4 Recomp (M7 CPU cost): edram_vk_plan.cpp PackFixed16, operation for operation (xenia-canary d119505 pixel_formats.xesli XePackFixed;
// ShiftGlue 72b7dd6f fh1_native_resolve_memory.cs.hlsl:218-232 PackChannel): number 3 (signed integer) clamps to [-32768, 32767], adds
// 0.5 away from zero by the sign of the unclamped value (one float add) and truncates toward zero (int(), as C++'s int32_t(float)),
// keeping the low 16 bits (uint(int) keeps the bit pattern); every other number packs UNORM at 65535 (PackUnorm, XeResolveUnorm). The
// CPU's NaN branch (-32768) is unreachable from storage 4 (finite int16 halves, a finite bias factor) and is not mirrored.
uint XeResolveFixed16(float v, uint number)
{
    if (number == 3u)
    {
        precise float clamped = v < -32768.0 ? -32768.0 : v > 32767.0 ? 32767.0 : v;
        precise float t = clamped + (v >= 0.0 ? 0.5 : -0.5);
        return uint(int(t)) & 0xFFFFu;
    }
    return XeResolveUnorm(v, 65535.0);
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
#ifdef XE_SOURCE_UINT
        // FM4 Recomp (M7 CPU cost): the 16_16 sample's raw halves as the guest word (kResolveDump), unpacked as the CPU unpacks that word.
#ifdef XE_SOURCE_MS
        uvec4 texel = texelFetch(xe_source, p, int(h));
#else
        uvec4 texel = texelFetch(xe_source, p, 0);
#endif
        vec4 s = XeResolveUnpack1616(XePackUint(texel, xe_transfer.sourceFormat));
#else
#ifdef XE_SOURCE_MS
        vec4 texel = texelFetch(xe_source, p, int(h));
#else
        vec4 texel = texelFetch(xe_source, p, 0);
#endif
        vec4 s = XeUnpack(XePack(texel, xe_transfer.sourceFormat), xe_transfer.sourceFormat);
#endif
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
    if (xe_transfer.destFormat == 10u)  // k_8_8 (edram_vk_plan.cpp ResolveConvertPixel; an R16_UINT attachment keeps the low 16 bits)
        xe_word = XeResolveUnorm(pixel.r, 255.0) | (XeResolveUnorm(pixel.g, 255.0) << 8u);
    else if (xe_transfer.destFormat == 25u)  // FM4 Recomp (M7 CPU cost): k_16_16, red the low half (pixel_formats.xesli:236-239), by the number format
        xe_word = XeResolveFixed16(pixel.r, xe_transfer.tileOffset) | (XeResolveFixed16(pixel.g, xe_transfer.tileOffset) << 16u);
    else if (xe_transfer.destFormat == 6u || xe_transfer.destFormat == 50u)
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
                                     Resident = 8, DepthExport = 9, ColorToDepthExport = 10,  // the two: FM4_TRANSFER_STENCIL_EXPORT
                                     ResidentDepth = 11 };  // FM4_RESOLVE_RESIDENT_DEPTH

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
    // FM4 Recomp (alpha, RT format 5): and storage 5 (16_16_16_16, R16G16B16A16_SNORM; view R16G16B16A16_UINT, X :1640-1642).
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
    // FM4 Recomp (M7 GPU cost): resolve clears not applied yet (render_hooks.h PendingClear), oldest first; applied by the next pass
    // that touches the target (a draw pass, a transfer pass into it) or flushed before any read of it (FlushClears; with
    // FM4_CLEAR_FLUSH_OVERLAP=1 before any read of their tiles: the others stay pending for the next pass or the next read).
    std::vector<PendingClear> pendingClears;
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
    bool gpuWords = false;    // FM4_RESOLVE_GPU_WORDS: at `offset` the converted words (ResolveWordsToGuest), not the samples
    uint64_t serial = 0;      // FM4_PARTIAL_RETIRE: the submission part that recorded it (render_hooks.h currentPartSerial)
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
    // FM4 Recomp (alpha, RT format 5): the colour transfer with a 64 bpp side (kTransferColor64), [source MS][source uint][dest uint]
    // [source 64 bpp][dest 64 bpp]; compiled where a 64 bpp side is on its transfer view (source 64 -> source uint, dest 64 -> dest uint).
    VkShaderModule colorWide[2][2][2][2][2] = {};
    // Through a 16_16 transfer view, [source MS]: depth -> colour into it (XE_DEST_UINT), colour -> depth and stencil bit from it
    // (XE_SOURCE_UINT).
    VkShaderModule depthToColorUint[2] = {}, colorToDepthUint[2] = {}, colorToStencilUint[2] = {};
    // The sample dump through a transfer view (kResolveDump XE_SOURCE_UINT), [source MS].
    VkShaderModule dumpUint[2] = {};
    // FM4_TRANSFER_STENCIL_EXPORT: the one-draw depth transfers, [source MS][sample shading]; compiled only with the extension.
    VkShaderModule depthExport[2][2] = {}, colorToDepthExport[2][2] = {}, colorToDepthExportUint[2][2] = {};
    bool stencilExport = false, sampleShading = false;
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
    size_t handedOff = 0;  // FM4 Recomp (FM4_ASYNC_RETIRE): pending[0, handedOff) are in a completion-worker batch (HandOff)
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): the converted words (kResolveResident) and the last recorded resolve (lastResolve).
    VkShaderModule resident1x = VK_NULL_HANDLE, residentMs = VK_NULL_HANDLE;
    VkShaderModule residentDepth1x = VK_NULL_HANDLE, residentDepthMs = VK_NULL_HANDLE;  // FM4_RESOLVE_RESIDENT_DEPTH
    // FM4 Recomp (M7 CPU cost, FM4_RESOLVE_RESIDENT_1616): kResolveResident reading a 16_16 source through its transfer view, [source MS].
    VkShaderModule residentUint[2] = {};
    Target resident;  // kResidentWidth x kResidentHeight R32_UINT, not an EDRAM key
    Target resident16;  // FM4 Recomp: the same in R16_UINT, for 16 bpp destinations (k_8_8)
    bool lastWords16 = false;  // the last converted resolve's words are in resident16
    ResidentResolve last;
    bool lastValid = false;
    uint64_t residentResolves = 0, checkedResolves = 0, checkedPixels = 0, checkMismatches = 0;
    uint8_t* guestBase = nullptr;               // main.cpp's writable guest memory (renderer.h Options::guestBase)
    ResolveWriteObserver observer = nullptr;    // guest_writes.h MarkGuestWritten, from the renderer
    uint64_t transfers = 0, transferDraws = 0, resolves = 0;
    uint64_t draws64 = 0;  // FM4 Recomp (alpha, RT format 5): draws bound to a 64 bpp colour key (AcquireTargets)
    uint64_t deferredClears = 0;  // FM4 Recomp (M7 GPU cost): resolve clears kept pending for the next pass (Clear)
    uint64_t deferredClearDraws = 0;  // FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): clear-class draws kept as pending clears instead of drawn (AcquireTargets)
    // FM4 Recomp (M7 GPU cost, FM4_CLEAR_FLUSH_OVERLAP): FlushClears calls that found pending clears, the flush passes they recorded, the
    // clears flushed and the clears kept pending past a read (none of their tiles read).
    uint64_t flushCalls = 0, flushPasses = 0, flushedClears = 0, keptClears = 0;
    // FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): transfers recorded as a clear of the destination (edram.h Transfer::value) and
    // their tiles; tiles recorded as constants (Ownership::SetClearValue); clears whose values could not be recorded (ClearRequestWord refused).
    uint64_t valueTransfers = 0, valueTiles = 0, clearValueTilesRecorded = 0, clearValuesRefused = 0;
    // FM4_CLEAR_VALUE_OWNERSHIP: clear colours with no exact guest word (a 7e3 word cannot hold them); Transfer::exact - 1 indexes this. Distinct
    // clear colours are few; past the cap a colour is simply not recorded (copied as before).
    std::vector<std::array<float, 4>> exactColours;
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
// FM4 Recomp (alpha, RT format 5): bit 3 a 64 bpp source (storage 5, through its transfer view: bit 1 too); a 64 bpp destination is
// dest.key.Is64bpp(). The pipeline key holds all four flag bits (sample and sample count moved up one bit).
bool Pipeline(PipelineKind kind, uint32_t sourceFlags, const Target& dest, uint32_t sample, VkPipeline& out, std::string& error)
{
    const bool sourceMs = (sourceFlags & 1) != 0, sourceUint = (sourceFlags & 2) != 0, destUint = UintStorage(dest);
    const bool sampleShading = (sourceFlags & 4) != 0;  // FM4_TRANSFER_STENCIL_EXPORT: one draw for every destination sample
    const bool source64 = (sourceFlags & 8) != 0, dest64 = !dest.depth && dest.key.Is64bpp();  // FM4 Recomp (alpha, RT format 5)
    const VkFormat passFormat = PassFormat(dest);
    const uint64_t key = uint64_t(kind) | uint64_t(sourceFlags & 15) << 4 | uint64_t(sample) << 8 | uint64_t(dest.samples) << 10 | uint64_t(passFormat) << 14;
    // FM4 Recomp (alpha, RT format 5): a 64 bpp side only in a colour transfer, and only through its transfer view (ClassifyTransfer
    // refuses the cross-aspect pairs; the sample dump and the resident conversion never see storage 5: DecodeResolve refuses its resolves).
    // FM4 Recomp (alpha, 64 bpp resolve): DecodeResolve now accepts its 1x resolves, which Resolve reads by vkCmdCopyImageToBuffer, not
    // through a pipeline, so this refusal still holds.
    if ((source64 || dest64) && (kind != PipelineKind::Color || (source64 && !sourceUint) || (dest64 && !destUint)))
        return Fail(error, "a 64 bpp (storage 5) side in an EDRAM pass other than a colour transfer through its transfer view");
    if (const auto it = unit->pipelines.find(key); it != unit->pipelines.end())
        return out = it->second, true;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!Pass(passFormat, dest.samples, dest.depth, pass, error))
        return false;
    // A transfer view: colour -> colour on either side; depth -> colour into it; colour -> depth (and stencil bit) from it.
    // ClassifyTransfer admits only storage 4 for the last two (storage 14 stays colour <-> colour). The sample dump reads it
    // (a storage-4 resolve source; its R32_UINT dump image is not a transfer view).
    // FM4 Recomp (M7 CPU cost): the resident conversion reads a storage-4 source the same way (kResolveResident XE_SOURCE_UINT).
    const bool uintVariant = kind == PipelineKind::Color ? (sourceUint || destUint)
                             : kind == PipelineKind::DepthToColor ? destUint
                             : kind == PipelineKind::ColorToDepth || kind == PipelineKind::ColorToStencilBit || kind == PipelineKind::Dump ||
                                     kind == PipelineKind::ColorToDepthExport || kind == PipelineKind::Resident
                                 ? sourceUint
                                 : false;
    if ((sourceUint || destUint) && !uintVariant)
        return Fail(error, "a transfer-view (storage 14 or 4) side in a transfer that has no uint variant");
    const VkShaderModule fragment = kind == PipelineKind::DepthExport ? unit->depthExport[sourceMs][sampleShading]
                                    : kind == PipelineKind::ColorToDepthExport
                                        ? (uintVariant ? unit->colorToDepthExportUint[sourceMs][sampleShading] : unit->colorToDepthExport[sourceMs][sampleShading])
                                    : kind == PipelineKind::Color && (source64 || dest64)  // FM4 Recomp (alpha, RT format 5)
                                        ? unit->colorWide[sourceMs][sourceUint][destUint][source64][dest64]
                                    : kind == PipelineKind::Color && uintVariant ? unit->colorUint[sourceMs][sourceUint][destUint]
                                    : kind == PipelineKind::DepthToColor && uintVariant ? unit->depthToColorUint[sourceMs]
                                    : kind == PipelineKind::ColorToDepth && uintVariant ? unit->colorToDepthUint[sourceMs]
                                    : kind == PipelineKind::ColorToStencilBit && uintVariant ? unit->colorToStencilUint[sourceMs]
                                    : kind == PipelineKind::Dump && uintVariant ? unit->dumpUint[sourceMs]
                                    : kind == PipelineKind::Resident && uintVariant ? unit->residentUint[sourceMs]  // FM4_RESOLVE_RESIDENT_1616
                                    : kind == PipelineKind::Color ? (sourceMs ? unit->colorFrom4x : unit->colorFrom1x)
                                    : kind == PipelineKind::Depth             ? (sourceMs ? unit->depthFromMs : unit->depthFrom1x)
                                    : kind == PipelineKind::DepthToColor      ? (sourceMs ? unit->depthToColorFromMs : unit->depthToColorFrom1x)
                                    : kind == PipelineKind::Dump              ? (sourceMs ? unit->dumpFromMs : unit->dumpFrom1x)
                                    : kind == PipelineKind::DumpDepth         ? (sourceMs ? unit->dumpDepthMs : unit->dumpDepth1x)
                                    : kind == PipelineKind::Resident          ? (sourceMs ? unit->residentMs : unit->resident1x)
                                    : kind == PipelineKind::ResidentDepth     ? (sourceMs ? unit->residentDepthMs : unit->residentDepth1x)
                                    : kind == PipelineKind::ColorToDepth      ? (sourceMs ? unit->colorToDepthMs : unit->colorToDepth1x)
                                    : kind == PipelineKind::ColorToStencilBit ? (sourceMs ? unit->colorToStencilMs : unit->colorToStencil1x)
                                                                              : (sourceMs ? unit->stencilFromMs : unit->stencilFrom1x);
    if (fragment == VK_NULL_HANDLE)
        return Fail(error, "EDRAM transfer pipeline: its fragment shader was not compiled (stencil export without VK_EXT_shader_stencil_export?)");
    const bool writesStencilAll = kind == PipelineKind::DepthExport || kind == PipelineKind::ColorToDepthExport;  // FM4_TRANSFER_STENCIL_EXPORT
    const bool writesDepth = kind == PipelineKind::Depth || kind == PipelineKind::ColorToDepth || writesStencilAll;
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
    multisample.pSampleMask = dest.samples > 1 && !sampleShading ? &mask : nullptr;
    multisample.sampleShadingEnable = sampleShading ? VK_TRUE : VK_FALSE;  // FM4_TRANSFER_STENCIL_EXPORT: the shader runs per sample (gl_SampleID)
    multisample.minSampleShading = 1.0f;
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
    if (writesStencilAll)  // the exported reference replaces all eight bits (write mask 0xFF, static)
    {
        depthStencil.stencilTestEnable = VK_TRUE;
        depthStencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
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

// FM4 Recomp (opt-in FM4_TRANSFER_STENCIL_EXPORT=1 or 2): see kTransferDepthStencilExport; needs VK_EXT_shader_stencil_export
// (vulkan_device.h). 1: one draw for every sample (sampleRateShading when offered); 2: one draw per sample (no sample shading).
uint32_t TransferStencilExport()
{
    static const uint32_t mode = [] {
        const char* v = std::getenv("FM4_TRANSFER_STENCIL_EXPORT");
        return v != nullptr && (v[0] == '1' || v[0] == '2') && v[1] == '\0' ? uint32_t(v[0] - '0') : 0u;
    }();
    return mode;
}

// FM4 Recomp (FM4_DIAG_TRANSFERS=1, diagnostic): ownership transfers by (source key -> destination key): count and tiles, printed
// with the renderer's pass tally (EdramHooks::diagTally).
// FM4 Recomp (FM4_DIAG_TRANSFERS=2, diagnostic): also each transfer's cause and its destination's first consumer (DiagConsume).
uint32_t DiagTransfersLevel()
{
    static const uint32_t level = [] {
        const char* v = std::getenv("FM4_DIAG_TRANSFERS");
        return v != nullptr && (v[0] == '1' || v[0] == '2') && v[1] == '\0' ? uint32_t(v[0] - '0') : 0u;
    }();
    return level;
}
bool DiagTransfers() { return DiagTransfersLevel() != 0; }
std::map<std::string, std::pair<uint64_t, uint64_t>> transferPairs;  // "source -> dest" -> (count, tiles)
std::string DiagKeyName(Key k)
{
    char text[64];
    std::snprintf(text, sizeof text, "%s@%u p%u %ux f%u", k.IsDepth() ? "depth" : "colour", k.Base(), k.PitchTilesAt32bpp(), 1u << uint32_t(k.MsaaMode()), k.Format());
    return text;
}
// FM4_DIAG_TRANSFERS=2 (diagnostic only; nothing here is read by a non-diagnostic path). `diagCause`: why ExecuteTransfers runs now
// ("bind" by default; Resolve names its copy and its clears). `diagAwaiting`: per destination key, the transfer rows ("pair [cause]")
// whose tiles nobody has used yet, with their (count, tiles); DiagConsume files them under the first use of the key (a draw binding
// it, a resolve reading or clearing it, a transfer reading it), the next transfer into it, or "none yet" at the tally.
const char* diagCause = "bind";
std::map<uint32_t, std::map<std::string, std::pair<uint64_t, uint64_t>>> diagAwaiting;
std::map<std::string, std::pair<uint64_t, uint64_t>> diagCauseTally;  // "pair [cause] -> first use: ..." -> (count, tiles)
void DiagConsume(Key k, const std::string& use)
{
    if (DiagTransfersLevel() < 2)
        return;
    const auto it = diagAwaiting.find(k.packed);
    if (it == diagAwaiting.end())
        return;
    for (const auto& [row, v] : it->second)
    {
        auto& t = diagCauseTally[row + " -> first use: " + use];
        t.first += v.first, t.second += v.second;
    }
    diagAwaiting.erase(it);
}
void DiagTally(uint64_t firstSwap, uint64_t lastSwap)
{
    // Level 1: the top 16 rows (as before); level 2: every row, then the cause / first-use rows, all of them.
    const auto print = [&](const char* what, const std::map<std::string, std::pair<uint64_t, uint64_t>>& tally, size_t top) {
        std::vector<std::pair<std::pair<uint64_t, uint64_t>, std::string>> rows;
        for (const auto& [name, v] : tally)
            rows.push_back({{v.second, v.first}, name});
        std::sort(rows.rbegin(), rows.rend());
        std::fprintf(stderr, "[diag transfers] swaps %llu-%llu, %s (tiles, count):", (unsigned long long)firstSwap, (unsigned long long)lastSwap, what);
        for (size_t i = 0; i < rows.size() && i < top; i++)
            std::fprintf(stderr, "%s %s (%llu, %llu)", i ? ";" : "", rows[i].second.c_str(), (unsigned long long)rows[i].first.first,
                         (unsigned long long)rows[i].first.second);
        std::fprintf(stderr, "\n");
    };
    const bool all = DiagTransfersLevel() >= 2;
    print("ownership transfers by source -> destination", transferPairs, all ? SIZE_MAX : 16);
    transferPairs.clear();
    if (!all)
        return;
    for (const auto& [key, rows] : diagAwaiting)  // still unused at the tally: filed as such and forgotten
        for (const auto& [row, v] : rows)
        {
            auto& t = diagCauseTally[row + " -> first use: none yet"];
            t.first += v.first, t.second += v.second;
        }
    diagAwaiting.clear();
    print("transfers by pair [cause] -> the destination's first use", diagCauseTally, SIZE_MAX);
    diagCauseTally.clear();
}

// FM4 Recomp (M7 GPU cost; opt-in FM4_CLEAR_FLUSH_OVERLAP=1, after SpeedBreaker 312ce69 runtime/gpu/renderer.cpp:2550-2558
// NFSMW_CLEAR_FLUSH_RECT and :2749-2780 FlushPendingClears, GPL-3.0): with it, FlushClears flushes only the pending clears a read
// of the given tiles needs (edram_vk_plan.h SelectClearFlush: those whose tiles the read touches and the older ones under them);
// the rest stay pending for the target's next pass or its next read, which flushes them in turn. Off: every pending clear (today).
bool ClearFlushOverlap()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_CLEAR_FLUSH_OVERLAP");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[edram] clear flush by overlap on (FM4_CLEAR_FLUSH_OVERLAP=1): a read of a target (transfer source, resolve copy) "
                                 "flushes only the pending clears on the tiles it reads and the older ones under them\n");
        return enabled;
    }();
    return on;
}

// FM4 Recomp (M7 GPU cost): the pending clears of `t` recorded in their own pass over the cleared rows (the former Clear() pass),
// before a read of the target that no pass of ours precedes (a transfer source, a resolve copy). No-op without pending clears.
// FM4 Recomp (FM4_CLEAR_FLUSH_OVERLAP): `read`, the EDRAM tiles the read touches (nullptr: all pending clears, as without the flag;
// callers pass it only with the flag): only SelectClearFlush's clears are recorded, in their order; the others stay in
// t.pendingClears in theirs. None selected: no pass.
bool FlushClears(VkCommandBuffer c, Target& t, std::string& error, const edram::TileSet* read = nullptr)
{
    if (t.pendingClears.empty())
        return true;
    std::vector<PendingClear> kept;  // FM4_CLEAR_FLUSH_OVERLAP: what stays pending after this flush
    if (read != nullptr)
    {
        unit->flushCalls++;
        const std::vector<size_t> taken = SelectClearFlush(t.key, t.pendingClears, read);
        if (taken.size() != t.pendingClears.size())
        {
            std::vector<PendingClear> flush;
            size_t next = 0;
            for (size_t i = 0; i < t.pendingClears.size(); i++)
                if (next < taken.size() && taken[next] == i)
                    flush.push_back(t.pendingClears[i]), next++;
                else
                    kept.push_back(t.pendingClears[i]);
            unit->keptClears += kept.size();
            if (DiagTransfersLevel() >= 2)  // FM4_DIAG_TRANSFERS=2 (diagnostic): clears kept past a read, by key and cause
            {
                auto& k = diagCauseTally["pending clears of " + DiagKeyName(t.key) + " kept past a read [" + diagCause + "]"];
                k.first++, k.second += kept.size();
            }
            t.pendingClears.swap(flush);  // the clears to record now; `kept` goes back below
            if (t.pendingClears.empty())
            {
                t.pendingClears.swap(kept);
                return true;
            }
        }
        unit->flushPasses++, unit->flushedClears += t.pendingClears.size();
    }
    if (DiagTransfersLevel() >= 2)  // FM4_DIAG_TRANSFERS=2 (diagnostic): a flush pass, by key and cause (count, rectangles)
    {
        auto& f = diagCauseTally["flush of the pending clears of " + DiagKeyName(t.key) + " [" + diagCause + "]"];
        f.first++, f.second += t.pendingClears.size();
    }
    const PassActivity activity("edram clear");  // FM4_DIAG_PASS_TIMES label
    std::vector<VkRect2D> rects;
    for (const PendingClear& pc : t.pendingClears)
        rects.push_back(pc.rect);
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    if (!Pass(PassFormat(t), t.samples, t.depth, begin.renderPass, error))
        return false;
    begin.framebuffer = PassFramebuffer(t);
    begin.renderArea = UnitPassArea(rects.data(), rects.size(), t.width, t.height);
    OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
    char unitKind[64];  // FM4_DIAG_PASS_TIMES label
    std::snprintf(unitKind, sizeof unitKind, "clear %s %ux%u x%u", t.depth ? "depth" : "colour", t.width, t.height, t.samples);
    UnitPassBegin(c, unitKind);
    vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
    for (const PendingClear& pc : t.pendingClears)
        {
            const VkClearRect r{pc.rect, 0, 1};
            vkCmdClearAttachments(c, 1, &pc.attachment, 1, &r);
        }
    vkCmdEndRenderPass(c);
    UnitPassEnd(c);
    t.pendingClears.clear();
    t.pendingClears.swap(kept);  // FM4_CLEAR_FLUSH_OVERLAP: the clears not flushed, in order (empty without the flag)
    return true;
}

// FM4 Recomp (FM4_DIAG_TRANSFERS=2, diagnostic): each distinct constant recorded or refused for a key (kind, rectangle, word, tiles).
void DiagClearValue(const char* kind, edram::Key key, const edram::Rect& rect, uint32_t word, uint64_t tiles)
{
    if (DiagTransfersLevel() < 2)
        return;
    static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t>> named;
    if (named.insert({key.packed, rect.x, rect.y, rect.width, rect.height, word, tiles}).second)
        std::fprintf(stderr, "[diag clear value] %s key %s: rect %u,%u %ux%u word %08X, %llu tile(s) recorded\n", kind, DiagKeyName(key).c_str(), rect.x, rect.y,
                     rect.width, rect.height, word, (unsigned long long)tiles);
}

// FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): the clear colour with no exact guest word, kept as the host floats (Transfer::exact = this
// index + 1; 0: none). Only a value transfer to the same format uses it (Ownership::Change), where the same floats through the same VkClear give the
// bits the source's clear gives, so the copy it replaces is exact. Uint-view storages are excluded (their clears are raw words).
uint32_t ExactColourIndex(const ClearRequest& request, const Target& t)
{
    if (t.depth || !request.color || UintStorage(t))
        return 0;
    std::array<float, 4> rgba;
    std::memcpy(rgba.data(), request.rgba, sizeof rgba);
    for (size_t i = 0; i < unit->exactColours.size(); i++)
        if (std::memcmp(unit->exactColours[i].data(), rgba.data(), sizeof rgba) == 0)
            return uint32_t(i) + 1;
    if (unit->exactColours.size() >= 64)
        return 0;
    unit->exactColours.push_back(rgba);
    return uint32_t(unit->exactColours.size());
}

bool ExecuteTransfers(VkCommandBuffer c, Target& dest, const std::vector<Transfer>& list, std::string& error)
{
    const PassActivity activity("edram transfer");  // FM4_DIAG_PASS_TIMES label
    // FM4 Recomp (M7 GPU cost): every transfer of the list goes to `dest`, so all of them are drawn in one render pass whose
    // render area covers only the rows they touch (edram_vk_plan.h UnitPassArea); before this, each transfer opened a pass
    // over the whole host target (~55 unit passes a race frame, each loading and storing 1024-2048 rows on a tile-based GPU).
    // Every source is transitioned to its read layout before the pass and back to rest after it (barriers end a pass).
    struct Item
    {
        const Transfer* t;
        Target* source;
        TransferKind kind;
        std::vector<VkRect2D> rects;
    };
    std::vector<Item> items;
    std::vector<VkRect2D> area;
    if (!list.empty())  // FM4_DIAG_TRANSFERS=2 (diagnostic): rows still awaiting a use of dest are superseded by this list
        DiagConsume(dest.key, std::string("another transfer into it [") + diagCause + "]");
    for (const Transfer& t : list)
    {
        Target& source = *unit->targets.at(t.source.packed);
        const TransferKind kind = ClassifyTransfer(t.source, dest.key, error);
        if (kind == TransferKind::Refused)
            return false;
        edram::Rect r[3];
        const uint32_t n = edram::RangeRectangles(t.startTiles, t.endTiles, dest.key, r);
        // FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): a value transfer (edram.h Transfer::value: every tile of the range held the
        // constant `word` in the source, same aspect and format) is not copied: the destination gets a pending clear of each rectangle
        // with the word decoded for it (edram_vk_plan.h ValueClear, the exactness argument there), pushed after its older pending
        // clears (PushPendingClear keeps the guest order and drops the ones inside), recorded by whichever pass touches the destination
        // next (this one, when an ordinary transfer of the list follows; the renderer's draw pass through AcquireTargets; FlushClears
        // before a read). The source is neither flushed nor read: its own pending clear of the constant stays pending for the source's
        // next pass. A destination storage cleared through its transfer view (ValueClear false) is copied as before.
        if (t.value)
        {
            PendingClear pcs[3];
            bool ok = n != 0;
            for (uint32_t i = 0; ok && i < n; i++)
            {
                ok = r[i].x + r[i].width <= dest.width && r[i].y + r[i].height <= dest.height;
                if (ok && t.exact != 0)  // the request's own colour into the same format (Change: same aspect and format): the source's clear, exactly
                {
                    pcs[i].attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, pcs[i].attachment.colorAttachment = 0;
                    std::memcpy(pcs[i].attachment.clearValue.color.float32, unit->exactColours[t.exact - 1].data(), sizeof pcs[i].attachment.clearValue.color.float32);
                    pcs[i].rect = {{int32_t(r[i].x), int32_t(r[i].y)}, {r[i].width, r[i].height}};
                }
                else if (ok)
                    ok = ValueClear(dest.key, t.word, r[i], pcs[i]);
            }
            if (ok)
            {
                for (uint32_t i = 0; i < n; i++)
                    PushPendingClear(dest.pendingClears, pcs[i]);
                const uint32_t tiles = t.endTiles - t.startTiles;
                unit->valueTransfers++, unit->valueTiles += tiles;
                clearValueTransfersSinceSwap++, clearValueTilesSinceSwap += tiles;  // render_hooks.h: the renderer's swap line
                if (DiagTransfers())
                {
                    auto& p = transferPairs[DiagKeyName(t.source) + " -> " + DiagKeyName(dest.key) + " (value)"];
                    p.first++, p.second += tiles;
                    if (DiagTransfersLevel() >= 2)  // FM4_DIAG_TRANSFERS=2: nothing reads the source; dest's tiles await their first use
                    {
                        auto& a = diagAwaiting[dest.key.packed][DiagKeyName(t.source) + " -> " + DiagKeyName(dest.key) + " (value) [" + diagCause + "]"];
                        a.first++, a.second += tiles;
                    }
                }
                if (unit->transferPairs[1ull << 63 | uint64_t(t.source.packed) << 32 | dest.key.packed]++ == 0)
                    std::fprintf(stderr, "[edram] first value transfer key %06X -> key %06X, tiles %u-%u, word %08X: a clear of the destination, no copy\n",
                                 t.source.packed, dest.key.packed, t.startTiles, t.endTiles, t.word);
                continue;
            }
        }
        if (DiagTransfers())
        {
            auto& p = transferPairs[DiagKeyName(t.source) + " -> " + DiagKeyName(dest.key)];
            p.first++, p.second += t.endTiles - t.startTiles;
            if (DiagTransfersLevel() >= 2)  // FM4_DIAG_TRANSFERS=2: the source is used (read) now; dest's tiles await their first use
            {
                DiagConsume(t.source, "transfer source into " + DiagKeyName(dest.key) + " [" + diagCause + "]");
                auto& a = diagAwaiting[dest.key.packed][DiagKeyName(t.source) + " -> " + DiagKeyName(dest.key) + " [" + diagCause + "]"];
                a.first++, a.second += t.endTiles - t.startTiles;
            }
        }
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
        area.insert(area.end(), rects.begin(), rects.end());
        items.push_back({&t, &source, kind, std::move(rects)});
    }
    if (items.empty())
        return true;
    // The sources are read: their pending clears first (FlushClears); then sampled by the fragment shader: out of their rest layout
    // before the pass, back after it. The destination's pending clears are the first commands of this pass (in guest order they
    // precede the transfer), so its render area covers them too.
    // FM4 Recomp (FM4_CLEAR_FLUSH_OVERLAP): each source is flushed once, for the union of the tiles its transfers read (a transfer's
    // [startTiles, endTiles) are EDRAM tiles, the same in every key's layout; the shader reads the source only in them).
    if (ClearFlushOverlap())
    {
        std::vector<std::pair<Target*, edram::TileSet>> reads;
        for (const Item& it : items)
        {
            auto r = std::find_if(reads.begin(), reads.end(), [&](const auto& p) { return p.first == it.source; });
            if (r == reads.end())
            {
                reads.emplace_back(it.source, edram::TileSet{});
                r = reads.end() - 1;
            }
            r->second |= edram::RangeTiles(it.t->startTiles, it.t->endTiles);
        }
        for (auto& [source, tiles] : reads)
            if (!FlushClears(c, *source, error, &tiles))
                return false;
    }
    else
        for (const Item& it : items)
            if (!FlushClears(c, *it.source, error))
                return false;
    for (const PendingClear& pc : dest.pendingClears)
        area.push_back(pc.rect);
    for (const Item& it : items)
        Transition(c, *it.source, it.source->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    if (!Pass(PassFormat(dest), dest.samples, dest.depth, begin.renderPass, error))
        return false;
    begin.framebuffer = PassFramebuffer(dest);
    begin.renderArea = UnitPassArea(area.data(), area.size(), dest.width, dest.height);
    OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
    char unitKind[64];  // FM4_DIAG_PASS_TIMES label
    std::snprintf(unitKind, sizeof unitKind, "transfer to %s %ux%u x%u", dest.depth ? "depth" : "colour", dest.width, dest.height, dest.samples);
    UnitPassBegin(c, unitKind);
    vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
    for (const PendingClear& pc : dest.pendingClears)
        {
            const VkClearRect r{pc.rect, 0, 1};
            vkCmdClearAttachments(c, 1, &pc.attachment, 1, &r);
        }
    dest.pendingClears.clear();
    const VkViewport viewport{0.0f, 0.0f, float(dest.width), float(dest.height), 0.0f, 1.0f};
    vkCmdSetViewport(c, 0, 1, &viewport);
    bool ok = true;
    for (const Item& it : items)
    {
        if (!ok)
            break;
        const Transfer& t = *it.t;
        Target& source = *it.source;
        const TransferKind kind = it.kind;
        const std::vector<VkRect2D>& rects = it.rects;
        // A storage-14 or storage-4 source is read through its transfer view (its second set).
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, unit->layout, 0, 1, UintStorage(source) ? &source.uintSource : &source.source, 0,
                                nullptr);
        // sourceFormat / destFormat: a colour key's storage format, or a depth key's depth format (kTransferDepthToColor,
        // kTransferColorToDepth, kTransferDepth between depth formats; the stencil-bit shaders read neither).
        // FM4 Recomp (alpha, RT format 5): the pitches are Key::PitchTiles (X :4823, :5223-5224 GetPitchTiles), doubled for a 64 bpp key
        // and equal to PitchTilesAt32bpp for every other; a 64 bpp source sets sourceFlags bit 3 (Pipeline).
        TransferPush push{dest.samples == 4 ? 1u : 0u, 0, source.key.Format(), dest.key.Format(), 0,
                          dest.key.PitchTiles(), (dest.key.Base() - source.key.Base()) & (edram::kTileCount - 1),
                          uint32_t(source.key.MsaaMode()), uint32_t(dest.key.MsaaMode()), 0, 0, 0, 0, source.key.PitchTiles()};
        const uint32_t sourceMs = (source.samples > 1 ? 1u : 0u) | (UintStorage(source) ? 2u : 0u) |
                                  (!source.depth && source.key.Is64bpp() ? 8u : 0u);  // Pipeline's sourceFlags
        if (kind == TransferKind::Color || kind == TransferKind::DepthToColor)
        {
            // Depth -> colour: the source's depth and stencil views (its descriptor set, bindings 0 and 1), per sample when multisampled.
            const PipelineKind pk = kind == TransferKind::Color ? PipelineKind::Color : PipelineKind::DepthToColor;
            for (uint32_t s = 0; ok && s < dest.samples; s++)
                push.destSample = s, ok = Draw3(c, pk, sourceMs, dest, s, push, rects, error);
        }
        else if (unit->stencilExport)
        {
            // FM4_TRANSFER_STENCIL_EXPORT: depth and all eight stencil bits in one draw for every sample (sampleRateShading) or one
            // draw per sample, instead of the stencil clear, the depth draw and eight stencil-bit draws per sample below.
            const bool fromColor = kind == TransferKind::ColorToDepth;
            const PipelineKind pk = fromColor ? PipelineKind::ColorToDepthExport : PipelineKind::DepthExport;
            if (unit->sampleShading && dest.samples > 1)
                ok = Draw3(c, pk, sourceMs | 4u, dest, 0, push, rects, error);
            else
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
        if (ok)
        {
            unit->transfers++;
            if (unit->transferPairs[uint64_t(t.source.packed) << 32 | dest.key.packed]++ == 0)
                std::fprintf(stderr, "[edram] first transfer key %06X (%u sample(s)) -> key %06X (%u sample(s)), tiles %u-%u\n", t.source.packed, source.samples,
                             dest.key.packed, dest.samples, t.startTiles, t.endTiles);
        }
    }
    vkCmdEndRenderPass(c);  // ended even on a refusal, so the command buffer stays well formed until the stop
    UnitPassEnd(c);
    for (const Item& it : items)
        Rest(c, *it.source);
    return ok;
}

// ---- Hooks --------------------------------------------------------------------------------------------------------

bool AcquireTargets(VkCommandBuffer c, const RenderTarget& rt, bool needColor, bool needDepth, const edram::Rect* depthCutout, const edram::Rect* colorCutout,
                    const ClearRequest* clear, HostTargets& out, std::string& error)
{
    if (!needColor && !needDepth)
        return Fail(error, "a draw that needs neither a colour nor a depth target");
    // The binding exactly as the renderer's pipeline key needs it (edram_vk_plan.h BindRegistersFor); the rows the
    // draw can touch are the scissor's bottom within the surface (draw_state.h RenderTarget::targetHeight).
    // FM4 Recomp (M7 CPU cost): no copy of the map to restore on a refusal: a refusal here stops the run (render_hooks.h acquireTargets).
    // FM4 Recomp (M7 GPU cost, FM4_TRANSFER_SKIP_UNWRITTEN): a depth target the draw only tests (draw_state.cpp depthWritten: no z write,
    // stencil off) is claimed read-only, so the tiles it borrowed can go back to their previous owner without a transfer.
    const edram::Binding b = edram::Bind(unit->ownership, BindRegistersFor(rt, needColor, needDepth), rt.targetHeight, depthCutout, colorCutout,
                                         needDepth && !rt.depthWritten);
    if (!b.ok)
        return Fail(error, b.refusal);
    lastBindTransferTiles = 0;  // FM4_DIAG_OVERWRITE
    for (const auto& list : {b.transfers[0], b.transfers[1]})
        for (const Transfer& t : list)
            if (!t.value)  // FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): a value transfer copies nothing
                lastBindTransferTiles += t.endTiles - t.startTiles;
    auto refuse = [&](std::string why) { return Fail(error, std::move(why)); };
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
    // FM4 Recomp (alpha, RT format 5): the draws bound to a 64 bpp colour key (storage 5), the evidence of the first validation run: the
    // first is logged with its key, the count at shutdown.
    if (color && color->key.Is64bpp() && unit->draws64++ == 0)
        std::fprintf(stderr, "[edram] first draw into a 64 bpp colour key %06X (storage %u, %u sample(s), %ux%u, %u transfer(s) into it before)\n",
                     color->key.packed, color->key.Format(), color->samples, color->width, color->height, unsigned(b.transfers[1].size()));
    if (DiagTransfersLevel() >= 2)  // FM4_DIAG_TRANSFERS=2 (diagnostic): this draw is the first use of what was just moved into its targets
    {
        if (depth)
            DiagConsume(depth->key, std::string(rt.depthWritten ? "draw writing depth/stencil: " : "draw testing depth only: ") + diagDrawSignature);
        if (color)
            DiagConsume(color->key, std::string("draw writing colour: ") + diagDrawSignature);
    }
    out = {};
    if (clear != nullptr)
    {
        // FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): the draw is not recorded; its clear joins each bound target's pending clears
        // after the transfers above (the tiles outside its rectangle hold the previous owner's content first, as a resolve clear's
        // do: Resolve below runs ExecuteTransfers then Clear), and the targets' older pending clears stay pending for the next pass
        // that touches them: the order of the clears on a target is the guest's. A clear-class draw into a target cleared through
        // its transfer view (storage 4 and 14: the renderer's draw pass uses the typed view) is refused, as is a bound target the
        // request carries no value for (the renderer proves both before asking: draw_record.h FindClearDraw).
        for (Target* t : {color, depth})
        {
            if (t == nullptr)
                continue;
            if (t->depth ? !clear->depth : !clear->color)
                return Fail(error, "clear draw: a bound target without a clear value");
            if (clear->rect.x + clear->rect.width > t->width || clear->rect.y + clear->rect.height > t->height)
                return Fail(error, "clear draw rectangle outside its host target");
            if (!t->depth && UintStorage(*t))
                return Fail(error, "clear draw into a storage format cleared through the transfer view (" + std::to_string(t->key.Format()) + ")");
            PendingClear pc;
            if (t->depth)
            {
                pc.attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                pc.attachment.clearValue.depthStencil.depth = clear->depthValue;
                pc.attachment.clearValue.depthStencil.stencil = clear->stencil;
            }
            else
            {
                pc.attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                pc.attachment.colorAttachment = 0;
                std::memcpy(pc.attachment.clearValue.color.float32, clear->rgba, sizeof pc.attachment.clearValue.color.float32);
            }
            pc.rect = {{int32_t(clear->rect.x), int32_t(clear->rect.y)}, {clear->rect.width, clear->rect.height}};
            PushPendingClear(t->pendingClears, pc);
            // FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): the tiles wholly inside the rectangle now hold the request's constant;
            // recorded as the guest word only when the word's own clear is bit for bit this clear (edram_vk_plan.h ClearRequestWord),
            // so that a later value transfer of it (ExecuteTransfers) is exactly the copy it replaces; else nothing is recorded and the
            // tiles are copied as before. The bind above claimed every tile of the rectangle for t->key (its cutout), so SetClearValue
            // finds them owned.
            if (ClearValueOwnership())
            {
                uint32_t word = 0;
                if (ClearRequestWord(*clear, t->key, word))
                {
                    const uint64_t tiles = unit->ownership.SetClearValue(t->key, clear->rect, word);
                    unit->clearValueTilesRecorded += tiles;
                    DiagClearValue("clear draw", t->key, clear->rect, word, tiles);
                }
                else if (const uint32_t exact = ExactColourIndex(*clear, *t); exact != 0)
                {
                    const uint64_t tiles = unit->ownership.SetClearValue(t->key, clear->rect, 0, exact);
                    unit->clearValueTilesRecorded += tiles;
                    DiagClearValue("clear draw (host colour)", t->key, clear->rect, exact, tiles);
                }
                else
                {
                    unit->clearValuesRefused++;
                    DiagClearValue(clear->color && !t->depth ? "clear draw NOT RECORDABLE (no exact word)" : "depth clear draw NOT RECORDABLE (no exact word)", t->key, clear->rect, 0, 0);
                }
            }
        }
        unit->deferredClearDraws++;
    }
    else
        // FM4 Recomp (M7 GPU cost): the acquired targets' pending resolve clears go first in the draw's pass (the renderer records them).
        for (Target* t : {color, depth})
            if (t != nullptr && !t->pendingClears.empty())
            {
                out.clears.insert(out.clears.end(), t->pendingClears.begin(), t->pendingClears.end());
                t->pendingClears.clear();
            }
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

// The clear of `rect` on `t` (X vulkan_render_target_cache.cc:5340-5431). FM4 Recomp (M7 GPU cost):
// not recorded here but kept pending on the target (render_hooks.h PendingClear); the next pass that touches `t` records
// it first (the renderer's draw pass through AcquireTargets, a transfer pass into t), or FlushClears before a read of t (with
// FM4_CLEAR_FLUSH_OVERLAP=1: before a read of its tiles). Older
// pending clears wholly inside this rectangle are dropped (edram_vk_plan.h MergePendingClear). Before this, every resolve clear
// opened its own full-target render pass (~21 a race frame).
bool Clear(VkCommandBuffer c, Target& t, const edram::Rect& rect, uint32_t value, uint32_t valueLo, std::string& error)
{
    (void)c;
    if (rect.x + rect.width > t.width || rect.y + rect.height > t.height)
        return Fail(error, "resolve clear rectangle outside its host target");
    PendingClear pc;
    if (t.depth)
    {
        pc.attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        DecodeDepthClear(value, edram::DepthFormat(t.key.Format()), pc.attachment.clearValue.depthStencil.depth, pc.attachment.clearValue.depthStencil.stencil);
    }
    else
    {
        pc.attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        pc.attachment.colorAttachment = 0;
        // Storage 14 and 4: the raw bits through the transfer view (X :5393-5404, :5417-5421); the others as floats (X :5366-5395).
        // FM4 Recomp (alpha, 64 bpp resolve clear): storage 5 takes RB_COLOR_CLEAR_LO too (DecodeColorClearUint64).
        if (t.key.Is64bpp() ? !(UintStorage(t) && DecodeColorClearUint64(value, valueLo, t.key.Format(), pc.attachment.clearValue.color.uint32))
            : UintStorage(t) ? !DecodeColorClearUint(value, t.key.Format(), pc.attachment.clearValue.color.uint32)
                           : !DecodeColorClear(value, t.key.Format(), pc.attachment.clearValue.color.float32))
            return Fail(error, "resolve colour clear of storage format " + std::to_string(t.key.Format()));
    }
    pc.rect = {{int32_t(rect.x), int32_t(rect.y)}, {rect.width, rect.height}};
    PushPendingClear(t.pendingClears, pc);  // FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): the merge, shared with the clear-class draws (AcquireTargets)
    unit->deferredClears++;
    // FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): the tiles wholly inside the rectangle hold the raw clear word (the value this
    // very clear decodes, so a value transfer of them is exact by construction: edram_vk_plan.h). PrepareResolve claimed them for t.key
    // with the rectangle as cutout, so SetClearValue finds them owned. Storage 4 and 14 (the transfer view) are left to the copy path.
    if (ClearValueOwnership() && !(!t.depth && UintStorage(t)))
    {
        const uint64_t tiles = unit->ownership.SetClearValue(t.key, rect, value);
        unit->clearValueTilesRecorded += tiles;
        DiagClearValue("resolve clear", t.key, rect, value, tiles);
    }
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
    const PassActivity activity("resolve dump");  // FM4_DIAG_PASS_TIMES label
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
    char unitKind[64];  // FM4_DIAG_PASS_TIMES label
    std::snprintf(unitKind, sizeof unitKind, "dump %s %ux%u x%u", depth ? "depth" : "colour", r.width, r.height, 1u << uint32_t(msaa));
    UnitPassBegin(c, unitKind);
    vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, unit->layout, 0, 1, uintSource ? &source.uintSource : &source.source, 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, float(dump.width), float(dump.height), 0.0f, 1.0f};
    vkCmdSetViewport(c, 0, 1, &viewport);
    const TransferPush push{0, 0, storage, 0, 0, 0, 0, uint32_t(msaa), 0, r.x, r.y, layout.width, layout.height, 0};
    // FM4 Recomp (alpha, RT format 5): bit 3 for a 64 bpp source, so Pipeline refuses it (DecodeResolve refuses its resolves first).
    const uint32_t sourceFlags = (source.samples > 1 ? 1u : 0u) | (uintSource ? 2u : 0u) | (!source.depth && source.key.Is64bpp() ? 8u : 0u);
    const bool ok = Draw3(c, depth ? PipelineKind::DumpDepth : PipelineKind::Dump, sourceFlags, dump, 0, push, {VkRect2D{{0, 0}, extent}}, error);
    vkCmdEndRenderPass(c);
    UnitPassEnd(c);
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
    const PassActivity activity("resolve resident");  // FM4_DIAG_PASS_TIMES label
    Target& dest = info.destBytesLog2 == 1 ? unit->resident16 : unit->resident;  // FM4 Recomp: k_8_8 words are 16 bits
    uint32_t indices[4] = {}, count = 0, packed = 0;
    if (!ResolveSamples(info, indices, count))
        return Fail(error, "resident resolve: a selected sample the source does not have");
    for (uint32_t i = 0; i < count; i++)
        packed |= indices[i] << (2 * i);
    const uint32_t biasBits = (uint32_t(info.destExpBias) << 23) + 0x3F800000u;  // as ResolveConvertPixel (modulo 2^32)
    // FM4 Recomp (M7 CPU cost, FM4_RESOLVE_RESIDENT_1616): a 16_16 source (storage 4) is read raw through its R16G16_UINT transfer view,
    // as RecordDump reads it (the same device check: integer formats need sampledImageIntegerSampleCounts for the source's count). The only
    // other transfer-view storage, 14, never converts here (ResidentConvertible; a fast 32_FLOAT copy is residentCopy).
    const bool uintSource = !source.depth && UintStorage(source);
    if (uintSource && storage != uint32_t(edram::ColorFormat::k16_16))
        return Fail(error, "resident resolve: a transfer-view source of storage " + std::to_string(storage) + " has no GPU conversion");
    if (uintSource && source.samples > 1 && !(unit->limits.sampledImageIntegerSampleCounts & source.samples))
        return Fail(error, "resident resolve of a " + std::to_string(source.samples) +
                               "-sample target through its transfer view: the device cannot sample that count (sampledImageIntegerSampleCounts)");
    // FM4_RESOLVE_RESIDENT_DEPTH: a depth source is sampled through its depth and stencil views (set bindings 0 and 1, as the dump).
    Transition(c, source, source.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    Transition(c, dest, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    if (!Pass(dest.format, 1, false, begin.renderPass, error))
        return false;
    begin.framebuffer = dest.framebuffer;
    begin.renderArea = {{0, 0}, {r.width, r.height}};
    OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
    char unitKind[64];  // FM4_DIAG_PASS_TIMES label
    std::snprintf(unitKind, sizeof unitKind, "resident %ux%u x%u", r.width, r.height, 1u << uint32_t(info.msaa));
    UnitPassBegin(c, unitKind);
    vkCmdBeginRenderPass(c, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, unit->layout, 0, 1, uintSource ? &source.uintSource : &source.source, 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, float(dest.width), float(dest.height), 0.0f, 1.0f};
    vkCmdSetViewport(c, 0, 1, &viewport);
    // tileOffset carries the destination number format (k_16_16's PackFixed16 branch; 0 for every other destination).
    const TransferPush push{count, packed, storage, info.destFormat, biasBits, info.destSwap ? 1u : 0u, info.destNumber, uint32_t(info.msaa), 0, r.x, r.y, r.width, r.height, 0};
    // FM4 Recomp (alpha, RT format 5): bit 3 for a 64 bpp source, so Pipeline refuses it (DecodeResolve refuses its resolves first).
    const uint32_t sourceFlags = (source.samples > 1 ? 1u : 0u) | (uintSource ? 2u : 0u) | (!source.depth && source.key.Is64bpp() ? 8u : 0u);
    const bool ok = Draw3(c, source.depth ? PipelineKind::ResidentDepth : PipelineKind::Resident, sourceFlags, dest, 0, push,
                          {VkRect2D{{0, 0}, {r.width, r.height}}}, error);
    vkCmdEndRenderPass(c);
    UnitPassEnd(c);
    Rest(c, source);
    if (!ok)
        return false;
    Transition(c, dest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    unit->residentResolves++;
    return true;
}

// FM4 Recomp (M7 CPU cost; opt-in FM4_RESOLVE_GPU_WORDS16=1, needs FM4_RESOLVE_GPU_WORDS=1 and FM4_RESOLVE_RESIDENT_K88=1): a k_8_8
// (16 bpp) resolve is written back from its R16_UINT resident words too (ResolveWordsToGuest's 16 bpp branch: swap + tile, 2 bytes a
// texel) instead of ResolveConvertToGuest's float unpack and pack of every texel (FM4's k_8_8 chain, 512x256 down to 8x8: ~218k texels
// a race frame). The words are the ones FM4_RESOLVE_RESIDENT_CHECK compared with the CPU
// conversion (0 differing pixels in each of the chain's 8 shapes). Inert under the check (ResolveGpuWords is).
bool ResolveGpuWords16()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_GPU_WORDS16");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && ResolveGpuWords() && ResolveResidentK88();
        if (asked)
            std::fprintf(stderr, enabled ? "[edram] resolve GPU words 16 bpp on (FM4_RESOLVE_GPU_WORDS16=1): a k_8_8 resolve is written back from its R16 words\n"
                                         : "[edram] FM4_RESOLVE_GPU_WORDS16=1 ignored: it needs FM4_RESOLVE_GPU_WORDS=1 and FM4_RESOLVE_RESIDENT_K88=1\n");
        return enabled;
    }();
    return on;
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
    // FM4 Recomp (alpha, 64 bpp resolve): a 64 bpp (storage 5) source is read 8 bytes a pixel by the direct copy below: its 1x
    // R16G16B16A16_SNORM texels to the staging buffer, raw (the copy keeps 0x8000; components r, g, b, a little-endian, i.e. the EDRAM
    // words r | g << 16 and b | a << 16, edram_vk_plan.cpp ResolveConvertToGuest). ResolveFullConvertible admits it only at 1x, so it
    // never takes the sample dump (one 32-bit word a sample; Pipeline refuses a 64 bpp Dump as well) nor a GPU-words path.
    const bool source64 = info.copy && !depthCopy && info.source.Is64bpp();
    const VkDeviceSize sampleBytes = source64 ? 8 : 4;
    if (info.copy)
    {
        if (depthCopy ? !ResolveDepthConvertible(info, error) : !fast && !ResolveFullConvertible(info, storage, error))
            return false;
        if (source64 && (fast || dumpPath))
            return Fail(error, "a resolve from a 64 bpp source on the fast or sample-dump path (only the 1x direct copy reads one)");
        if (uint64_t(layout.width) * layout.height * layout.samples * sampleBytes > kStagingBytes)
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
    const VkDeviceSize wordBytes = VkDeviceSize(layout.width) * layout.height * layout.samples * sampleBytes;  // FM4 Recomp (alpha, 64 bpp resolve): 8 for storage 5
    bool converted = false, check = false, gpuWords = false;
    VkDeviceSize checkOffset = 0;
    if (plan.copy)
    {
        Target* source = GetTarget(c, plan.copySource, error);
        diagCause = "resolve copy";  // FM4_DIAG_TRANSFERS=2 (diagnostic label only)
        // FM4 Recomp (FM4_CLEAR_FLUSH_OVERLAP): the copy reads only plan.copyRect of the source (the read-back copy, RecordDump,
        // RecordResident and the resident copy below all take `r`), so only the clears on its tiles must land first.
        edram::TileSet copyTiles;
        if (source != nullptr && ClearFlushOverlap())
            copyTiles = edram::RectTiles(source->key, plan.copyRect);
        const bool moved = source != nullptr && ExecuteTransfers(c, *source, plan.copyTransfers, error) &&
                           FlushClears(c, *source, error, ClearFlushOverlap() ? &copyTiles : nullptr);
        diagCause = "bind";
        if (!moved)
            return false;
        DiagConsume(plan.copySource, std::string(depthCopy ? "resolve copy of depth " : "resolve copy of colour ") + std::to_string(info.x1 - info.x0) + "x" +
                                         std::to_string(info.y1 - info.y0));  // FM4_DIAG_TRANSFERS=2
        const edram::Rect& r = plan.copyRect;
        if (r.x + r.width > source->width || r.y + r.height > source->height)
            return Fail(error, "resolve rectangle outside the source's host target");
        // FM4 Recomp (FM4_RESOLVE_GPU_WORDS): a resolve the GPU converts below is read back from those words instead.
        std::string unconvertible;
        // FM4 Recomp (M7 CPU cost): a 16_16 source (storage 4) only with FM4_RESOLVE_RESIDENT_1616 (kResolveResident XE_SOURCE_UINT).
        // FM4 Recomp (alpha, 64 bpp resolve): never for a 64 bpp source (ResidentConvertible has no storage 5 either; explicit here).
        const bool residentWords = ResolveResident() && !depthCopy && !source64 && (!fast || ResolveFullConvertible(info, storage, unconvertible)) &&
                                   ResidentConvertible(storage, info.destFormat, info.destBytesLog2) && (info.destBytesLog2 == 2 || ResolveResidentK88()) &&
                                   (storage != uint32_t(edram::ColorFormat::k16_16) || ResolveResident1616()) && r.width == info.x1 - info.x0 &&
                                   r.height == info.y1 - info.y0 && r.width <= kResidentWidth && r.height <= kResidentHeight;
        gpuWords = residentWords && ResolveGpuWords() && (info.destBytesLog2 == 2 || ResolveGpuWords16());  // FM4 Recomp (M7 CPU cost): k_8_8's R16 words too
        // FM4 Recomp (M7 waits): a fast 32_FLOAT (storage 14) resolve into k_32_FLOAT (36) is a bit copy (ResolveToGuest applies no
        // swap for it), so the resident words are the source texels themselves: vkCmdCopyImage from the R32_SFLOAT target into the
        // R32_UINT resident image (size-compatible, bit-exact, NaN payloads kept). Not under FM4_RESOLVE_RESIDENT_CHECK, whose
        // comparison is the converting path's. The read-back below stays the fast copy from the source.
        // FM4 Recomp (FM4_RESOLVE_RESIDENT_DEPTH): a depth resolve's one selected sample as the guest word (kResolveResidentDepth), for
        // the textures that read it as a raw 32-bit colour format (resolve_resident.h ResidentFormatPair); the read-back stays the
        // depth dump + ResolveDepthToGuest. Not under FM4_RESOLVE_RESIDENT_CHECK (its comparison is the converting path's).
        const bool residentDepth = !residentWords && ResolveResident() && ResolveResidentDepth() && !ResolveResidentCheck() && depthCopy &&
                                   info.destBytesLog2 == 2 && r.width == info.x1 - info.x0 && r.height == info.y1 - info.y0 &&
                                   r.width <= kResidentWidth && r.height <= kResidentHeight && source->depth && unit->residentDepth1x != VK_NULL_HANDLE;
        const bool residentCopy = !residentWords && !residentDepth && ResolveResident() && !ResolveResidentCheck() && !depthCopy && fast && storage == 14 &&
                                  info.destFormat == 36 && info.destBytesLog2 == 2 && r.width == info.x1 - info.x0 && r.height == info.y1 - info.y0 &&
                                  r.width <= kResidentWidth && r.height <= kResidentHeight && source->format == VK_FORMAT_R32_SFLOAT;
        if (!residentWords && !residentCopy && !residentDepth && ResolveResident())  // FM4_DIAG_CONSUMER (diagnostic): each resolve shape without GPU words, once
            if (const char* v = std::getenv("FM4_DIAG_CONSUMER"); v != nullptr && v[0] != '0')
            {
                static std::set<std::tuple<uint32_t, uint32_t, uint32_t, bool, bool, uint32_t, uint32_t>> named;
                if (named.insert({storage, info.destFormat, uint32_t(info.msaa), fast, depthCopy, r.width, r.height}).second)
                    std::fprintf(stderr, "[edram] resolve without GPU words: storage %u (%s) to texture format %u (bytes log2 %u), msaa %u, %s, %ux%u, bias %d, swap %d\n",
                                 storage, depthCopy ? "depth" : "colour", info.destFormat, info.destBytesLog2, uint32_t(info.msaa), fast ? "fast copy" : "converting",
                                 r.width, r.height, info.destExpBias, int(info.destSwap));
            }
        if (!gpuWords && !dumpPath)  // (FM4_RESOLVE_GPU_WORDS: the words are copied after RecordResident instead)
        {
            Transition(c, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {int32_t(r.x), int32_t(r.y), 0};
            copy.imageExtent = {r.width, r.height, 1};  // bufferRowLength 0: tightly packed, as ResolveToGuest reads it
            // FM4 Recomp (alpha, 64 bpp resolve): a storage-5 source gives 8 bytes a texel (R16G16B16A16_SNORM's texel size).
            copy.bufferOffset = offset;
            vkCmdCopyImageToBuffer(c, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, unit->staging, 1, &copy);
        }
        else if (!gpuWords && !RecordDump(c, *source, depthCopy, storage, info.msaa, r, layout, offset, error))
            return false;
        // FM4 Recomp (FM4_RESOLVE_RESIDENT): the converted words for the texture unit; the read-back above stays for guest memory.
        // A fast (bit-copy) resolve converts too when the converting path takes it: for 8_8_8_8 -> 6/50 and 2_10_10_10 -> 7/54 at
        // 1x its unpack and pack are the identity, so the words are the fast path's before the endian swap (test_edram_vk
        // TestFastResidentWords; 21 of 34 race resolves were fast and stayed non-resident).
        if (residentCopy)
        {
            // The source is in TRANSFER_SRC from the fast read-back copy above; the resident image takes the rectangle at (0, 0) and
            // is left in TRANSFER_SRC, as RecordResident leaves it.
            Transition(c, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            Transition(c, unit->resident, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.srcOffset = {int32_t(r.x), int32_t(r.y), 0};
            copy.extent = {r.width, r.height, 1};
            OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE)
            vkCmdCopyImage(c, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, unit->resident.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Transition(c, unit->resident, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            converted = true;
            unit->lastWords16 = false;
            unit->residentResolves++;
        }
        else if (residentWords || residentDepth)
        {
            if (!RecordResident(c, *source, storage, info, r, error))
                return false;
            converted = true;
            unit->lastWords16 = info.destBytesLog2 == 1;
            if (gpuWords)  // FM4_RESOLVE_GPU_WORDS: the converted words are the read-back (row-major, tightly packed)
            {
                VkBufferImageCopy words{};
                words.bufferOffset = offset;
                words.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                words.imageExtent = {r.width, r.height, 1};
                OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE): RecordResident ended any open pass; explicit for the site check
                // FM4 Recomp (M7 CPU cost, FM4_RESOLVE_GPU_WORDS16): k_8_8's words are in resident16 (RecordResident's dest; the same image
                // the check copy below reads), left in TRANSFER_SRC_OPTIMAL by RecordResident as the 32-bit image is; 2 bytes a texel.
                vkCmdCopyImageToBuffer(c, info.destBytesLog2 == 1 ? unit->resident16.image : unit->resident.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       unit->staging, 1, &words);
            }
            if (ResolveResidentCheck())  // CheckResident compares the 32-bit words, or the 16-bit ones of k_8_8
            {
                check = true;
                checkOffset = (offset + wordBytes + 255) & ~VkDeviceSize(255);
                VkBufferImageCopy words{};
                words.bufferOffset = checkOffset;
                words.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                words.imageExtent = {r.width, r.height, 1};
                OutsideRenderPass(c);  // FM4 Recomp (FM4_PASS_MERGE): RecordResident ended any open pass; explicit for the site check
                vkCmdCopyImageToBuffer(c, info.destBytesLog2 == 1 ? unit->resident16.image : unit->resident.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       unit->staging, 1, &words);
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
        diagCause = "resolve clear";  // FM4_DIAG_TRANSFERS=2 (diagnostic label only)
        const bool moved = t != nullptr && ExecuteTransfers(c, *t, clear->transfers, error);
        diagCause = "bind";
        if (!moved || !Clear(c, *t, clear->rect, clear->value, clear->valueLo, error))
            return false;
        DiagConsume(clear->target, "resolve clear");  // FM4_DIAG_TRANSFERS=2
    }
    if (plan.copy)
    {
        unit->pending.push_back({info, storage, fast, depthCopy, offset, check, checkOffset, gpuWords, currentPartSerial});
        static bool logged64 = false;  // FM4 Recomp (alpha, 64 bpp resolve): the accepted shape, observed once a run
        if (source64 && !logged64)
        {
            logged64 = true;
            std::fprintf(stderr, "[edram] first 64 bpp resolve: storage %u %ux%u at %u,%u (1x) to texture format %u (number %u, exp bias %d, swap %d, "
                                 "endian %u), %u bytes at %08X\n",
                         storage, info.x1 - info.x0, info.y1 - info.y0, info.x0, info.y0, info.destFormat, info.destNumber, info.destExpBias,
                         int(info.destSwap), info.destEndian, info.destExtentLength, info.destExtentStart);
        }
        unit->stagingUsed = check      ? checkOffset + VkDeviceSize(info.x1 - info.x0) * (info.y1 - info.y0) * 4
                            : gpuWords ? offset + VkDeviceSize(info.x1 - info.x0) * (info.y1 - info.y0) * 4
                                       : offset + wordBytes;
        // Recorded: the range is stale and pending for the texture unit until the slot retires (guest_writes.h).
        unit->observer(info.destExtentStart, info.destExtentLength);
        ResidentResolve& l = unit->last;
        l = {};
        l.converted = converted;
        l.extentStart = info.destExtentStart, l.extentLength = info.destExtentLength;
        l.textureBase = regs.copyDestBase, l.destFormat = info.destFormat, l.destPitch = info.destPitch, l.destEndian = info.destEndian;
        l.x0 = info.x0, l.y0 = info.y0, l.width = info.x1 - info.x0, l.height = info.y1 - info.y0;
        l.bytesLog2 = info.destBytesLog2;
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
    const uint16_t* gpu16 = reinterpret_cast<const uint16_t*>(staging + p.checkOffset);  // FM4 Recomp: k_8_8's R16_UINT words
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
            const uint32_t want = ResolveConvertPixel(p.info, p.storage, samples),
                           got = p.info.destBytesLog2 == 1 ? gpu16[size_t(y) * width + x] : gpu[size_t(y) * width + x];
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

// FM4 Recomp (opt-in FM4_RESOLVE_HOST_WRITE=1): a resolve write-back unprotects its destination's watched pages in one pass
// (guest_writes.h GuestRangeHostWrite) instead of faulting on each (profiled: the resolve-row workers
// spent 45 % of their time in mprotect and the fault trampoline; 1,000-1,300 faults a swap).
bool ResolveHostWrite()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_HOST_WRITE");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[edram] FM4_RESOLVE_HOST_WRITE=1: a resolve write-back unwatches its destination range before writing\n");
        return enabled;
    }();
    return on;
}

bool Retired(uint64_t upToSerial, std::string& error)
{
    // In record order: overlapping destinations end as the last resolve wrote them. FM4_PARTIAL_RETIRE: only the parts up to
    // `upToSerial` are complete; the rest stay pending (their staging stays reserved until every one is written back).
    size_t written = 0;
    for (const Pending& p : unit->pending)
    {
        if (p.serial > upToSerial)
            break;  // recorded in serial order
        written++;
        uint32_t guest = 0;
        if (!GuestRange(p.info.destExtentStart, p.info.destExtentLength, guest, error))
            return Fail(error, "resolve destination: " + error);
        if (ResolveHostWrite())
            GuestRangeHostWrite(p.info.destExtentStart, p.info.destExtentLength);
        if (p.check)
            CheckResident(p);
        // A plain CPU store through main.cpp's writable base, before the renderer calls GuestWritesRetired. (No write-watch
        // fault hook is installed, guest_memory.h SetGuestWriteFaultHook; once one is, a watched page faults
        // here on the CP thread: an open question.)
        const uint32_t* words = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(unit->stagingMapped) + p.offset);
        const bool ok = p.gpuWords ? ResolveWordsToGuest(p.info, p.storage, words, unit->guestBase + guest, p.info.destExtentLength, error)
                        : p.depth  ? ResolveDepthToGuest(p.info, words, unit->guestBase + guest, p.info.destExtentLength, error)
                        : p.fast ? ResolveToGuest(p.info, p.storage, words, unit->guestBase + guest, p.info.destExtentLength, error)
                                 : ResolveConvertToGuest(p.info, p.storage, words, unit->guestBase + guest, p.info.destExtentLength, error);
        if (!ok)
            return false;
    }
    unit->pending.erase(unit->pending.begin(), unit->pending.begin() + long(written));
    if (unit->pending.empty())
        unit->stagingUsed = 0;
    return true;
}

// FM4 Recomp (opt-in FM4_ASYNC_RETIRE=1; render_hooks.h handOff / writeBackHandedOff / forget; runtime/video/async_retire.h): the
// same write-back as Retired, split so that the completion worker writes a part's resolves while the command processor goes on
// recording. The worker sees only its batch (copies of the entries, the staging and guest base pointers, which live until Shutdown,
// after the worker is joined); the pending list and the staging allocation stay the command processor's. A staging range is reused
// only after `forget`, which runs after GpuGuard::Retire confirmed the worker finished that part.
struct HandOffBatch
{
    uint64_t serial = 0;
    const uint8_t* staging = nullptr;
    uint8_t* guestBase = nullptr;
    std::vector<Pending> entries;
};
std::mutex handOffLock;
std::deque<HandOffBatch> handOffBatches;  // under handOffLock, in serial order

void HandOff(uint64_t serial)
{
    HandOffBatch b{serial, static_cast<const uint8_t*>(unit->stagingMapped), unit->guestBase, {}};
    while (unit->handedOff < unit->pending.size() && unit->pending[unit->handedOff].serial <= serial)
        b.entries.push_back(unit->pending[unit->handedOff++]);
    if (b.entries.empty())
        return;
    std::lock_guard<std::mutex> lock(handOffLock);
    handOffBatches.push_back(std::move(b));
}

bool WriteBackHandedOff(uint64_t serial, std::string& error)
{
    HandOffBatch b;
    {
        std::lock_guard<std::mutex> lock(handOffLock);
        if (handOffBatches.empty() || handOffBatches.front().serial > serial)
            return true;  // the part recorded no read-back resolve
        b = std::move(handOffBatches.front());
        handOffBatches.pop_front();
    }
    for (const Pending& p : b.entries)  // record order, as Retired
    {
        uint32_t guest = 0;
        if (!GuestRange(p.info.destExtentStart, p.info.destExtentLength, guest, error))
            return Fail(error, "resolve destination: " + error);
        if (ResolveHostWrite())
            GuestRangeHostWrite(p.info.destExtentStart, p.info.destExtentLength);  // atomics and mprotect (texture_cache.h BeforeHostWrite)
        const uint32_t* words = reinterpret_cast<const uint32_t*>(b.staging + p.offset);
        const bool ok = p.gpuWords ? ResolveWordsToGuest(p.info, p.storage, words, b.guestBase + guest, p.info.destExtentLength, error)
                        : p.depth  ? ResolveDepthToGuest(p.info, words, b.guestBase + guest, p.info.destExtentLength, error)
                        : p.fast   ? ResolveToGuest(p.info, p.storage, words, b.guestBase + guest, p.info.destExtentLength, error)
                                   : ResolveConvertToGuest(p.info, p.storage, words, b.guestBase + guest, p.info.destExtentLength, error);
        if (!ok)
            return false;
    }
    return true;
}

void Forget(uint64_t upToSerial)
{
    size_t written = 0;
    while (written < unit->handedOff && unit->pending[written].serial <= upToSerial)
        written++;
    unit->pending.erase(unit->pending.begin(), unit->pending.begin() + long(written));
    unit->handedOff -= written;
    if (unit->pending.empty())
        unit->stagingUsed = 0;
}

bool LastResolve(ResidentResolve& out, VkImage& words)
{
    if (!unit->lastValid)
        return false;
    out = unit->last;
    words = !out.converted ? VK_NULL_HANDLE : unit->lastWords16 ? unit->resident16.image : unit->resident.image;
    return true;
}

bool ResolveRoom() { return unit->stagingUsed + 2 * kStagingBytes <= kStagingTotalBytes; }

void Shutdown()
{
    if (!unit)
        return;
    const VkDevice device = unit->dev.device;
    std::fprintf(stderr, "[edram] shutdown: %zu host target(s), %llu transfer(s) in %llu draw(s), %llu resolve(s), %llu deferred clear(s), %llu clear draw(s) deferred\n",
                 unit->targets.size(), (unsigned long long)unit->transfers, (unsigned long long)unit->transferDraws, (unsigned long long)unit->resolves,
                 (unsigned long long)unit->deferredClears, (unsigned long long)unit->deferredClearDraws);  // FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): the last
    if (ClearFlushOverlap())  // FM4 Recomp (M7 GPU cost, FM4_CLEAR_FLUSH_OVERLAP)
        std::fprintf(stderr, "[edram] clear flush by overlap: %llu flush(es) with pending clears, %llu flush pass(es), %llu clear(s) flushed, %llu kept past a read\n",
                     (unsigned long long)unit->flushCalls, (unsigned long long)unit->flushPasses, (unsigned long long)unit->flushedClears,
                     (unsigned long long)unit->keptClears);
    if (ClearValueOwnership())  // FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP)
        std::fprintf(stderr, "[edram] clear-value ownership: %llu value transfer(s) of %llu tile(s) (clears, no copy); %llu tile(s) recorded as constants, %llu clear(s) not recordable\n",
                     (unsigned long long)unit->valueTransfers, (unsigned long long)unit->valueTiles, (unsigned long long)unit->clearValueTilesRecorded,
                     (unsigned long long)unit->clearValuesRefused);
    if (ResolveResident())
        std::fprintf(stderr, "[edram] resident resolves: %llu converted on the GPU; check: %llu resolve(s), %llu pixel(s), %llu mismatch(es)%s\n",
                     (unsigned long long)unit->residentResolves, (unsigned long long)unit->checkedResolves, (unsigned long long)unit->checkedPixels,
                     (unsigned long long)unit->checkMismatches, ResolveResidentCheck() ? "" : " (FM4_RESOLVE_RESIDENT_CHECK off)");
    if (unit->draws64 != 0)  // FM4 Recomp (alpha, RT format 5)
        std::fprintf(stderr, "[edram] 64 bpp colour targets: %llu draw(s) bound one\n", (unsigned long long)unit->draws64);
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
    for (Target* t : {&unit->dump, &unit->resident, &unit->resident16})
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
                             unit->colorToStencilUint[0], unit->colorToStencilUint[1], unit->dumpUint[0], unit->dumpUint[1], unit->resident1x, unit->residentMs, unit->residentDepth1x, unit->residentDepthMs,
                             unit->residentUint[0], unit->residentUint[1],
                             unit->depthExport[0][0], unit->depthExport[0][1], unit->depthExport[1][0], unit->depthExport[1][1], unit->colorToDepthExport[0][0],
                             unit->colorToDepthExport[0][1], unit->colorToDepthExport[1][0], unit->colorToDepthExport[1][1], unit->colorToDepthExportUint[0][0],
                             unit->colorToDepthExportUint[0][1], unit->colorToDepthExportUint[1][0], unit->colorToDepthExportUint[1][1]})
        vkDestroyShaderModule(device, m, nullptr);  // a VK_NULL_HANDLE (an export module without the extension) is allowed
    for (auto& bySourceUint : unit->colorWide)  // FM4 Recomp (alpha, RT format 5): the uncompiled entries are VK_NULL_HANDLE
        for (auto& byDestUint : bySourceUint)
            for (auto& bySource64 : byDestUint)
                for (auto& byDest64 : bySource64)
                    for (VkShaderModule m : byDest64)
                        vkDestroyShaderModule(device, m, nullptr);
    vkDestroyBuffer(device, unit->staging, nullptr);
    vkFreeMemory(device, unit->stagingMemory, nullptr);  // implicitly unmapped
    transferSkipStats = nullptr;  // FM4_TRANSFER_SKIP_UNWRITTEN: it points into the unit
    unit.reset();
}

const EdramHooks kHooks{AcquireTargets, Resolve, Retired, Shutdown, LastResolve, ResolveRoom, DiagTransfers() ? &DiagTally : nullptr,  // FM4_DIAG_TRANSFERS
                        HandOff, WriteBackHandedOff, Forget};  // FM4 Recomp (FM4_ASYNC_RETIRE)

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
        // FM4 Recomp (alpha, RT format 5): storage 5 (16_16_16_16), blended as X's host path (X :1609-1613), and its transfer view
        // (X :1640-1642).
        {VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
         "R16G16B16A16_SNORM"},
        {VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "R16G16B16A16_UINT"},
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
    if (unit->dev.stencilExport)  // FM4_TRANSFER_STENCIL_EXPORT: [source MS][sample shading]
    {
        const std::string ext = "#extension GL_ARB_shader_stencil_export : require\n", shading = "#define XE_SAMPLE_SHADING 1\n",
                          uintSource = "#define XE_SOURCE_UINT 1\n";
        for (uint32_t m = 0; m < 2; m++)
            for (uint32_t sh = 0; sh < 2; sh++)
            {
                const std::string defines = header + ext + (m ? ms : "") + (sh ? shading : "");
                if (!Module(compiler, true, defines + kTransferCommon + kDepthPacking + kDepthUnpacking + kTransferDepthStencilExport, unit->depthExport[m][sh], error) ||
                    !Module(compiler, true, defines + swap + kTransferCommon + kTransferPacking + kDepthUnpacking + kTransferColorToDepthStencilExport,
                            unit->colorToDepthExport[m][sh], error) ||
                    !Module(compiler, true, defines + swap + uintSource + kTransferCommon + kTransferPacking + kDepthUnpacking + kTransferColorToDepthStencilExport,
                            unit->colorToDepthExportUint[m][sh], error))
                    return false;
            }
    }
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
    // FM4 Recomp (alpha, RT format 5): the colour transfers with a 64 bpp side (kTransferColor64), [MS][su][du][s64][d64]: a 64 bpp
    // source is always read through its R16G16B16A16_UINT view (su), a 64 bpp destination always written through it (du).
    for (uint32_t m = 0; m < 2; m++)
        for (uint32_t su = 0; su < 2; su++)
            for (uint32_t du = 0; du < 2; du++)
                for (uint32_t s64 = 0; s64 < 2; s64++)
                    for (uint32_t d64 = 0; d64 < 2; d64++)
                        if ((s64 || d64) && (su || !s64) && (du || !d64) &&
                            !Module(compiler, true,
                                    header + (m ? ms : "") + (su ? "#define XE_SOURCE_UINT 1\n" : "") + (du ? "#define XE_DEST_UINT 1\n" : "") +
                                        (s64 ? "#define XE_SOURCE_64 1\n" : "") + (d64 ? "#define XE_DEST_64 1\n" : "") + kTransferCommon +
                                        kTransferPacking + kTransferColor64,
                                    unit->colorWide[m][su][du][s64][d64], error))
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
    if (ResolveResident() && ResolveResidentDepth() &&
        (!Module(compiler, true, header + kTransferCommon + kDepthPacking + kResolveResidentDepth, unit->residentDepth1x, error) ||
         !Module(compiler, true, header + ms + kTransferCommon + kDepthPacking + kResolveResidentDepth, unit->residentDepthMs, error)))
        return false;
    if (ResolveResident() && (!Module(compiler, true, header + kTransferCommon + kTransferPacking + kResolveResident, unit->resident1x, error) ||
                              !Module(compiler, true, header + ms + kTransferCommon + kTransferPacking + kResolveResident, unit->residentMs, error)))
        return false;
    // FM4 Recomp (M7 CPU cost, FM4_RESOLVE_RESIDENT_1616): the conversion of a 16_16 source read through its transfer view, [source MS].
    if (ResolveResident() && ResolveResident1616())
        for (uint32_t m = 0; m < 2; m++)
            if (!Module(compiler, true, header + (m ? ms : "") + sourceUint + kTransferCommon + kTransferPacking + kResolveResident, unit->residentUint[m], error))
                return false;
    // The sample dump image (kResolveDump): R32_UINT, rendered then copied to the staging buffer; held for the run. FM4 Recomp: the
    // resident image (kResolveResident; FM4_RESOLVE_RESIDENT only) the same way, at kResidentWidth x kResidentHeight.
    for (Target* t : {&unit->dump, ResolveResident() ? &unit->resident : nullptr, ResolveResident() ? &unit->resident16 : nullptr})
    {
        if (t == nullptr)
            continue;
        Target& d = *t;
        d.format = t == &unit->resident16 ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_UINT, d.samples = 1;
        d.width = t == &unit->dump ? kDumpWidth : kResidentWidth, d.height = t == &unit->dump ? kDumpHeight : kResidentHeight;
        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = d.format;
        image.extent = {d.width, d.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_DST: the resident image also takes a fast 32_FLOAT resolve by vkCmdCopyImage (the validation layer named the missing flag).
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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

// FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): edram_vk.h.
bool ClearValueOwnership()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_CLEAR_VALUE_OWNERSHIP");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] clear-value ownership on (FM4_CLEAR_VALUE_OWNERSHIP=1): tiles a pending clear covers wholly move to a key of the same "
                                 "format as a clear of the same word, not a copy (resolve clears; with FM4_CLEAR_DRAWS=1 the clear draws too)\n");
        return enabled;
    }();
    return on;
}

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
    unit->stencilExport = device.stencilExport && TransferStencilExport() != 0;  // FM4_TRANSFER_STENCIL_EXPORT
    unit->sampleShading = device.sampleRateShading && TransferStencilExport() == 1;
    if (TransferStencilExport() != 0)
        std::fprintf(stderr, "[edram] FM4_TRANSFER_STENCIL_EXPORT=%u: depth transfers in one draw%s through VK_EXT_shader_stencil_export%s\n",
                     TransferStencilExport(), unit->sampleShading ? " for every sample" : " per sample",
                     device.stencilExport ? "" : " - NOT offered by the device: the nine-pass path stays");
    unit->guestBase = guestBase;
    unit->observer = observer;
    // FM4 Recomp (M7 GPU cost, opt-in FM4_TRANSFER_SKIP_UNWRITTEN=1): edram.h Ownership::ShadowAt; the counts go on the swap line.
    if (const char* v = std::getenv("FM4_TRANSFER_SKIP_UNWRITTEN"); v != nullptr && v[0] == '1' && v[1] == '\0')
    {
        unit->ownership.skipUnwritten = true;
        transferSkipStats = &unit->ownership.Skipped();
        std::fprintf(stderr, "[edram] FM4_TRANSFER_SKIP_UNWRITTEN=1: tiles handed back to the key they were transferred from, unwritten by the "
                             "interim owner, change owner without a transfer (kD24FS8, colour storage 3, 4, 14)\n");
    }
    if (!Create(compiler, error))
    {
        Shutdown();  // nothing was submitted: every created object is idle
        return nullptr;
    }
    return &kHooks;
}

}  // namespace runtime::video
