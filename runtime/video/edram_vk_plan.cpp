// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// See edram_vk_plan.h. X = xenia-canary 67f16c4e315f src/xenia/gpu (BSD-3-Clause), re-derived, never copied.
#include "edram_vk_plan.h"
#include "draw_state.h"
#include "front_buffer.h"  // TiledOffset (X texture_util.cc:443-459 GetTiledOffset2D, reviewed)
#include "render_targets_vk.h"
#include "parallel_rows.h"  // FM4 Recomp (M7): the read-back conversions split by rows
#include "texture_untile.h"  // TiledOffset2D, EndianSwap16Pair (X texture_util.cc:443-458; endian.xesli:25-31) for 16 bpp destinations
#include <cstdlib>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace runtime::video {

using edram::ColorFormat;
using edram::DepthFormat;
using edram::Key;
using edram::Msaa;

VkFormat HostColorFormat(ColorFormat storage)
{
    switch (storage)
    {
    case ColorFormat::k8_8_8_8: return VK_FORMAT_R8G8B8A8_UNORM;
    case ColorFormat::k2_10_10_10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case ColorFormat::k2_10_10_10_Float: return VK_FORMAT_R16G16B16A16_SFLOAT;  // X vulkan_render_target_cache.cc:1601-1603
    case ColorFormat::k32_Float: return VK_FORMAT_R32_SFLOAT;  // X :1618-1619 (transfers and clears via R32_UINT, :1643-1644)
    case ColorFormat::k16_16: return VK_FORMAT_R16G16_SNORM;   // X :1604-1608 (the guest's -32..32 as -1..1: pipeline_key.cpp)
    // FM4 Recomp (alpha, RT format 5): X :1609-1613; the guest's -32..32 as -1..1 by the shader's XE_COLOR0_SCALE (draw_record.cpp).
    case ColorFormat::k16_16_16_16: return VK_FORMAT_R16G16B16A16_SNORM;
    default: return VK_FORMAT_UNDEFINED;  // edram::SupportedKey refuses the rest in M4
    }
}

VkFormat HostDepthFormat(DepthFormat format, bool d24s8Supported)
{
    return format == DepthFormat::D24S8 && d24s8Supported ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
}

namespace {

const char* MsaaName(Msaa m) { return m == Msaa::X1 ? "1x" : m == Msaa::X2 ? "2x" : m == Msaa::X4 ? "4x" : "msaa 3"; }

std::string KeyName(Key k)
{
    char text[96];
    std::snprintf(text, sizeof text, "%s key %06X (base %u, %u tiles/row, %s, format %u)", k.IsDepth() ? "depth" : "colour", k.packed, k.Base(),
                  k.PitchTilesAt32bpp(), MsaaName(k.MsaaMode()), k.Format());
    return text;
}

bool Refuse(std::string& why, std::string text)
{
    why = std::move(text);
    return false;
}

}  // namespace

edram::BindRegisters BindRegistersFor(const RenderTarget& rt, bool needColor, bool needDepth)
{
    edram::BindRegisters r;
    r.surfaceInfo = (rt.pitch & 0x3FFF) | (rt.msaa & 3) << 16 | rt.hizPitch << 18;
    r.modeControl = uint32_t(edram::ModeControl::ColorDepth);
    r.colorMask = needColor ? 0xF : 0;
    r.depthControl = needDepth ? 1 : 0;
    r.colorInfo[0] = (rt.colorBase & 0x7FF) | (rt.colorFormatRaw & 0xF) << 16 | (uint32_t(rt.colorExpBias) & 0x3F) << 20;
    r.depthInfo = (rt.depthBase & 0x7FF) | (rt.depthFormat & 1) << 16;
    return r;
}

TransferKind ClassifyTransfer(Key source, Key dest, std::string& why)
{
    const std::string name = KeyName(source) + " -> " + KeyName(dest);
    auto refuse = [&](const char* reason) { why = "ownership transfer " + name + ": " + reason; return TransferKind::Refused; };
    if (source.Empty() || dest.Empty())
        return refuse("an empty key");
    if (source == dest)
        return refuse("a key as its own source");
    // Storage 4 (16_16) through its R16G16_UINT transfer view (TransferViewFormat; X :3545-3576 packs it, :3692-3711
    // unpacks it, the same for every transfer mode).
    const auto colorStorage = [](Key k)
    {
        return k.Format() == uint32_t(ColorFormat::k8_8_8_8) || k.Format() == uint32_t(ColorFormat::k2_10_10_10) ||
               k.Format() == uint32_t(ColorFormat::k2_10_10_10_Float) || k.Format() == uint32_t(ColorFormat::k16_16);
    };
    // FM4 Recomp (alpha, RT format 5): a 64 bpp colour key (storage 5) moves to and from colour keys only (kTransferColor64). Its
    // depth <-> colour transfers (X packs two depth samples into one 64 bpp sample, :3303-3333, and picks a 32 bpp half for a
    // depth destination, :3080-3102) are not modelled yet: refused by name, so the run that needs one says so.
    // FM4 Recomp (alpha, Car Select): colour -> depth from a 64 bpp key is modelled (the half of the source sample by
    // the destination pixel, X :2686-2777 and :3080-3102, then the column swap at the source's 64 bpp width, :2912-2936: MapTransferSource,
    // TransferWordOf64); depth -> colour into one stays refused.
    if (source.IsDepth() && dest.Is64bpp())
        return refuse("depth -> colour into a 64 bpp key (storage 5) is not modelled yet (X :3303-3333)");
    if (!source.IsDepth() && dest.IsDepth() && source.Is64bpp())
    {
        const Msaa sm = source.MsaaMode(), dm = dest.MsaaMode();
        if (uint32_t(sm) > 2 || uint32_t(dm) > 2)
            return refuse("msaa_samples 3 is undefined");
        return TransferKind::ColorToDepth;
    }
    if (source.IsDepth() && !dest.IsDepth())  // X kDepthToColor (:4910-4913); edram_vk_plan.h for the accepted shape
    {
        // Any pitches: X splits the destination tile index by the destination's pitch (:2516-2577) and the source tile index
        // by the source's (:2956-2987), one path for every transfer mode (observed:
        // the 4x kD24FS8 key 7082D0, 16 tiles a row, into the 4x 2_10_10_10 key 910000, 32 tiles a row).
        // Any MSAA pair of MapTransferTexel (X :2779-2864, the same rules for every 32bpp mode); a multisampled source is
        // read per sample (X :2307-2358 the MS depth and stencil textures, :3052-3080 their per-sample fetches).
        const Msaa sm = source.MsaaMode(), dm = dest.MsaaMode();
        if (uint32_t(sm) > 2 || uint32_t(dm) > 2)
            return refuse("msaa_samples 3 is undefined");
        // FM4 Recomp: 1x <-> 2x through MapTransferTexel's rule (X :2868-2898), per destination sample as every other pair
        // (observed: the 1x kD24FS8 key 602040 into the 2x 8_8_8_8 key 082800).
        // Both depth formats: kD24FS8 through Depth32To20e4, kD24S8 through Depth32ToUnorm24 (X :3591-3609).
        if (!colorStorage(dest))
            return refuse("depth -> colour into a storage format other than 0, 2, 3 and 4");
        return TransferKind::DepthToColor;
    }
    // Different bases go through the tile offset of MapEdramTexel (X uses one tile-offset path for every transfer mode,
    // vulkan_render_target_cache.cc:2938-2954), different pitches through X's separate source pitch (:2956-2987): accepted
    // between colour keys (observed: the 1280-wide 7e3 key, 16 tiles a row, into the
    // 80-wide 32_FLOAT key, 1 tile a row), depth -> colour (above) and into a depth key (observed:
    // the 4x kD24FS8 keys 7082D0 and 688400 (2x), 16 tiles a row, into the 4x kD24FS8 key 710400, 32 a row).
    // The depth, stencil-bit and colour -> depth shaders read their source through the same XeSourceTexel.
    if (uint32_t(source.MsaaMode()) > 2 || uint32_t(dest.MsaaMode()) > 2)
        return refuse("msaa_samples 3 is undefined");
    // The MSAA pairs MapTransferTexel implements (X :2779-2898): equal, 1x <-> 4x, 4x <-> 2x (native 2x attachments: the
    // shape FM4's HDR resolve needs) and 1x <-> 2x (observed:
    // the 1x 8_8_8_8 key 003800 into the 2x 8_8_8_8 key 082800).
    const Msaa sm = source.MsaaMode(), dm = dest.MsaaMode();
    // Into a 2x depth key: from a 4x depth key (FM4's 3D scene after the welcome binds the 2x
    // kD24FS8 key 688400 over tiles the 4x kD24FS8 key 708400 owns), through the 4x -> 2x rule of MapTransferTexel (X
    // :2786-2802), or from a colour key (observed: the 1x 8_8_8_8 key 003800 into the 2x kD24FS8
    // key 682850), through its rule and X kColorToDepth, or from a 1x depth key (observed: the 1x
    // kD24FS8 key 602040 into 682850), through the 1x -> 2x rule (X :2887-2898); all use the depth and stencil-bit passes
    // every depth destination uses, per destination sample. 2x depth -> 2x depth is never observed.
    if (dest.IsDepth() && dm == Msaa::X2 && source.IsDepth() && sm == Msaa::X2)
        return refuse("a transfer into a 2x depth target from a 2x depth key (never observed)");
    if (!source.IsDepth() && dest.IsDepth())  // X kColorToDepth + kColorToStencilBit (:170-171, :178-180, :4904-4908)
    {
        if (!colorStorage(source))
            return refuse("colour -> depth from a storage format other than 0, 2, 3 and 4");
        // Both depth formats (X :4101-4125: kD24S8 through the unorm24 conversion, kD24FS8 through Depth20e4To32;
        // FM4 has a kD24S8 case).
        return TransferKind::ColorToDepth;
    }
    if (dest.IsDepth())
    {
        // FM4 Recomp: a format change is the same kind, the shader reinterpreting the guest bits (ConvertDepthFormat).
        // Any accepted MSAA pair; a multisampled source is read with sampler2DMS / usampler2DMS (kTransferDepth,
        // kTransferStencilBit; observed: 2x depth at 1024 -> 4x depth at 720).
        return TransferKind::Depth;
    }
    const auto storage = [](Key k)
    {
        return k.Format() == uint32_t(ColorFormat::k8_8_8_8) || k.Format() == uint32_t(ColorFormat::k2_10_10_10) ||
               k.Format() == uint32_t(ColorFormat::k2_10_10_10_Float) || k.Format() == uint32_t(ColorFormat::k16_16) ||
               k.Format() == uint32_t(ColorFormat::k32_Float) || k.Format() == uint32_t(ColorFormat::k16_16_16_16);
    };
    if (!storage(source) || !storage(dest))
        return refuse("a colour storage format other than 0 (8_8_8_8), 2 (2_10_10_10), 3 (2_10_10_10_FLOAT, 7e3), 4 (16_16), 5 (16_16_16_16) and "
                      "14 (32_FLOAT)");
    // An unchanged sample count with equal bases, pitches and formats is the same key (refused above): here the formats
    // differ, and the pixel goes through the 32-bit packing (X :3387-3700) at the same pixel and sample (MapTransferTexel).
    // FM4 Recomp (alpha, RT format 5): a 64 bpp side (storage 5) goes through its two 32-bit guest words (MapTransferSource,
    // TransferWordOf64 / TransferTexelOf64; X :2607-2777, :3005-3023, :3080-3102, :3114-3370), every MSAA pair as for 32 bpp.
    return TransferKind::Color;
}

// FM4 Recomp (M7 GPU cost, FM4_HOST_MSAA_1X): the three former inline copies (edram_vk.cpp ExecuteTransfers, RecordDump, RecordResident).
uint32_t TransferSourceFlags(uint32_t hostSamples, bool uintView, bool colour64)
{
    return (hostSamples > 1 ? 1u : 0u) | (uintView ? 2u : 0u) | (colour64 ? 8u : 0u);
}

SourceTexel MapTransferTexel(uint32_t destX, uint32_t destY, uint32_t destSample, Msaa source, Msaa dest)
{
    if (source == Msaa::X4 && dest == Msaa::X1)  // X :2804-2818: pixels to samples
        return {destX >> 1, destY >> 1, (destX & 1) | (destY & 1) << 1};
    if (source == Msaa::X1 && dest == Msaa::X4)  // X :2823-2828 (x) and :2857-2864 (y): samples to pixels
        return {destX << 1 | (destSample & 1), destY << 1 | (destSample >> 1 & 1), 0};
    // Native 2x host samples: "top is 1 in Vulkan, bottom is 0" (X :2599-2601), so guest 2x sample g is host sample g ^ 1.
    if (source == Msaa::X4 && dest == Msaa::X2)  // X :2786-2793 (sample = x bit 0 | (dest sample ^ 1) << 1), :2800-2802 (x >> 1)
        return {destX >> 1, destY, (destX & 1) | ((destSample ^ 1) & 1) << 1};
    if (source == Msaa::X2 && dest == Msaa::X4)  // X :2823-2828 (x << 1 | dest sample bit 0), :2843-2849 ((dest sample >> 1) ^ 1)
        return {destX << 1 | (destSample & 1), destY, ((destSample >> 1) & 1) ^ 1};
    if (source == Msaa::X2 && dest == Msaa::X1)  // X :2868-2886: y bit 0 to the 2x sample (^ 1 native), y >> 1
        return {destX, destY >> 1, (destY & 1) ^ 1};
    if (source == Msaa::X1 && dest == Msaa::X2)  // X :2887-2898: the 2x sample (^ 1 native) to y bit 0
        return {destX, destY << 1 | ((destSample ^ 1) & 1), 0};
    return {destX, destY, dest == Msaa::X1 ? 0 : destSample};
}

SourceTexel MapCrossAspectTexel(uint32_t destX, uint32_t destY, uint32_t destSample, Key source, Key dest)
{
    return MapEdramTexel(destX, destY, destSample, source, dest);
}

SourceTexel MapEdramTexel(uint32_t destX, uint32_t destY, uint32_t destSample, Key source, Key dest)
{
    return MapTransferSource(destX, destY, destSample, source, dest).first;
}

// FM4 Recomp (alpha, RT format 5): MapEdramTexel generalized to 64 bpp keys (edram_vk_plan.h MapTransferSource). For two 32 bpp keys
// every step is the former MapEdramTexel's (PitchTiles() is PitchTilesAt32bpp() there, the tile sizes have no 64 bpp shift, and the
// same-bpp branch is MapTransferTexel).
TransferSource MapTransferSource(uint32_t destX, uint32_t destY, uint32_t destSample, Key source, Key dest)
{
    const Msaa sm = source.MsaaMode(), dm = dest.MsaaMode();
    const bool s64 = source.Is64bpp(), d64 = dest.Is64bpp();
    // X :2540-2545: a tile is 80 samples wide at 32 bpp, 40 at 64 bpp (>> dest_is_64bpp), halved again at 4x; :2550-2554 its height.
    const uint32_t dtw = edram::kTileWidthSamples >> (edram::SamplesXLog2(dm) + (d64 ? 1 : 0)), dth = edram::kTileHeightSamples >> edram::SamplesYLog2(dm);
    const uint32_t stw = edram::kTileWidthSamples >> (edram::SamplesXLog2(sm) + (s64 ? 1 : 0)), sth = edram::kTileHeightSamples >> edram::SamplesYLog2(sm);
    // X :2516-2577: the destination's tile (split by its pitch, X :4823 and :5223 GetPitchTiles, doubled at 64 bpp) and the pixel in it.
    const uint32_t destTile = (destY / dth) * dest.PitchTiles() + destX / dtw;
    const uint32_t px = destX % dtw, py = destY % dth;
    TransferSource r;
    if (s64 == d64)
        r.first = MapTransferTexel(px, py, destSample, sm, dm);  // X :2778-2831 "Same bit count", with the common Y below folded in
    else
    {
        uint32_t x = px, y = py, s = dm == Msaa::X1 ? 0 : destSample;
        if (d64)  // X :2607-2685: 32 bpp -> 64 bpp, the low word's texel (the high word's: `second` below)
        {
            if (sm == Msaa::X4)
            {
                if (dm == Msaa::X4)  // :2611-2627: s = ds & 2, x = BitFieldInsert(ds, x, 1, 31)
                    s = destSample & 2, x = px << 1 | (destSample & 1);
                else if (dm == Msaa::X2)  // :2628-2647 (native 2x host samples): s = (ds ^ 1) << 1
                    s = ((destSample ^ 1) & 1) << 1;
                else  // :2648-2662: s = BitFieldInsert(0, y, 1, 1), y >>= 1
                    s = (py & 1) << 1, y = py >> 1;
            }
            else  // :2663-2685: 4x x = BitFieldInsert(x << 2, ds, 1, 1); 1x/2x x <<= 1 (Y below)
                x = dm == Msaa::X4 ? px << 2 | (destSample & 1) << 1 : px << 1;
        }
        else  // X :2686-2777: 64 bpp -> 32 bpp, with the half of the source sample that holds the destination's word
        {
            if (dm == Msaa::X4)
            {
                if (sm == Msaa::X4)  // :2692-2707: s = BitFieldInsert(ds, x, 0, 1), x >>= 1
                    s = (destSample & ~1u) | (px & 1), x = px >> 1;
                r.half = destSample & 1;  // :2714-2717: the destination's horizontal sample
            }
            else
            {
                if (sm == Msaa::X4)  // :2722-2762: s = (x >> 1) & 1 and the vertical sample bit, x >>= 2
                {
                    s = (px >> 1) & 1;
                    if (dm == Msaa::X2)  // :2729-2746 (native 2x): bit 1 = ds ^ 1
                        s |= ((destSample ^ 1) & 1) << 1;
                    else  // :2747-2758: bit 1 = y & 1, y >>= 1
                        s |= (py & 1) << 1, y = py >> 1;
                    x = px >> 2;
                }
                else  // :2763-2770
                    x = px >> 1;
                r.half = px & 1;  // :2771-2774: the destination's horizontal pixel
            }
        }
        // X :2833-2903: the common Y (and sample) of a 1x/2x source of another sample count, independent of bits per sample.
        if (sm != Msaa::X4 && sm != dm)
        {
            if (dm == Msaa::X4)
            {
                if (sm == Msaa::X2)  // :2839-2853 (native 2x): (ds >> 1) ^ 1
                    s = ((destSample >> 1) & 1) ^ 1;
                else  // :2856-2864: y = BitFieldInsert(ds >> 1, y, 1, 31)
                    y = py << 1 | ((destSample >> 1) & 1);
            }
            else if (sm == Msaa::X2)  // :2867-2886: 2x -> 1x, s = (y & 1) ^ 1, y >>= 1
                s = (py & 1) ^ 1, y = py >> 1;
            else  // :2887-2898: 1x -> 2x, y = BitFieldInsert(ds ^ 1, y, 1, 31)
                y = py << 1 | ((destSample ^ 1) & 1);
        }
        if (sm == Msaa::X1)
            s = 0;
        r.first.x = x, r.first.y = y, r.first.sample = s;
    }
    if (source.IsDepth() != dest.IsDepth())  // X :2916-2936: only between colour and depth (never with a 64 bpp side: ClassifyTransfer)
    {
        const uint32_t half = edram::kTileWidthSamples >> (1 + edram::SamplesXLog2(sm) + (s64 ? 1 : 0));  // X :2912-2920
        r.first.x = r.first.x < half ? r.first.x + half : r.first.x - half;                              // X :2921-2935
    }
    const uint32_t sourceTile = (destTile + dest.Base() - source.Base()) & (edram::kTileCount - 1);  // X :2938-2954
    const uint32_t sourcePitch = source.PitchTiles();  // X :2956-2965, :5224: the source's own pitch splits its tile index
    r.first.x += (sourceTile % sourcePitch) * stw, r.first.y += (sourceTile / sourcePitch) * sth;  // X :2966-2987
    r.second = r.first;
    if (d64 && !s64)  // X :3005-3023: the high word is the next sample (a 4x source) or the next pixel (1x/2x)
    {
        if (sm == Msaa::X4)
            r.second.sample = r.first.sample | 1;
        else
            r.second.x = r.first.x | 1;
    }
    return r;
}

uint32_t TransferWordOf64(const uint32_t texel[4], uint32_t half)
{
    // X :3080-3102: the second half takes components 2-3 (half the component count, 4 >> 1); :3545-3576: red (or blue) the low half.
    const uint32_t lo = half ? texel[2] : texel[0], hi = half ? texel[3] : texel[1];
    return (lo & 0xFFFF) | (hi & 0xFFFF) << 16;
}

void TransferTexelOf64(uint32_t low, uint32_t high, uint32_t texel[4])
{
    // X :3352-3370: component i = BitFieldUExtract(packed[i >> 1], (i & 1) ? 16 : 0, 16).
    texel[0] = low & 0xFFFF, texel[1] = low >> 16, texel[2] = high & 0xFFFF, texel[3] = high >> 16;
}

VkRect2D UnitPassArea(const VkRect2D* rects, size_t count, uint32_t width, uint32_t height)
{
    if (count == 0)
        return {{0, 0}, {width, height}};
    uint32_t y0 = height, y1 = 0;
    for (size_t i = 0; i < count; i++)
    {
        const uint32_t top = rects[i].offset.y < 0 ? 0u : uint32_t(rects[i].offset.y);
        y0 = std::min(y0, top);
        y1 = std::max(y1, top + rects[i].extent.height);
    }
    y0 &= ~31u;
    y1 = std::min(height, (y1 + 31) & ~31u);
    if (y1 <= y0)
        return {{0, 0}, {width, height}};
    return {{0, int32_t(y0)}, {width, y1 - y0}};
}

// FM4 Recomp (M7 GPU cost): `r` lies wholly inside `add` (the one rule of MergePendingClear and PushPendingClear).
static bool RectInside(const VkRect2D& r, const VkRect2D& add)
{
    return r.offset.x >= add.offset.x && r.offset.y >= add.offset.y && int64_t(r.offset.x) + r.extent.width <= int64_t(add.offset.x) + add.extent.width &&
           int64_t(r.offset.y) + r.extent.height <= int64_t(add.offset.y) + add.extent.height;
}

size_t MergePendingClear(std::vector<VkRect2D>& rects, VkRect2D add)
{
    const size_t before = rects.size();
    rects.erase(std::remove_if(rects.begin(), rects.end(), [&](const VkRect2D& r) { return RectInside(r, add); }), rects.end());
    const size_t dropped = before - rects.size();
    rects.push_back(add);
    return dropped;
}

size_t PushPendingClear(std::vector<PendingClear>& pending, const PendingClear& add)
{
    // FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): the same rule as MergePendingClear applied to the clears themselves (an older clear with
    // exactly the new rectangle is inside it and goes; matching rectangles by equality would keep it: TestPushPendingClear).
    const size_t before = pending.size();
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const PendingClear& old) { return RectInside(old.rect, add.rect); }), pending.end());
    const size_t dropped = before - pending.size();
    pending.push_back(add);
    return dropped;
}

edram::Rect PendingClearRect(const VkRect2D& r)
{
    const int64_t x0 = std::max<int64_t>(0, r.offset.x), y0 = std::max<int64_t>(0, r.offset.y);
    const int64_t x1 = int64_t(r.offset.x) + r.extent.width, y1 = int64_t(r.offset.y) + r.extent.height;
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
}

std::vector<size_t> SelectClearFlush(Key key, const std::vector<PendingClear>& pending, const edram::TileSet* read)
{
    std::vector<size_t> taken;
    if (read == nullptr)
    {
        for (size_t i = 0; i < pending.size(); i++)
            taken.push_back(i);
        return taken;
    }
    // FM4 Recomp: SpeedBreaker's `run` (the read, then each clear taken) as one tile set; newest first, so it holds every newer
    // clear taken when an older one is tested (an older clear kept under a newer taken one would land after it and undo it).
    edram::TileSet run = *read;
    for (size_t i = pending.size(); i-- > 0;)
    {
        const edram::TileSet tiles = edram::RectTiles(key, PendingClearRect(pending[i].rect));
        if ((tiles & run).any())
        {
            taken.push_back(i);
            run |= tiles;
        }
    }
    std::reverse(taken.begin(), taken.end());
    return taken;
}

bool ResolveComputeSource(const std::string& fragment, std::string& out, std::string& why)
{
    static const char* const output = "layout(location = 0) out uint xe_word;";
    static const char* const entry = "void main()\n{\n    ivec2 p = ivec2(uvec2(gl_FragCoord.xy) + uvec2(xe_transfer.x0, xe_transfer.y0));";
    out = fragment;
    for (const char* text : {output, entry})
    {
        const size_t at = out.find(text);
        if (at == std::string::npos || out.find(text, at + 1) != std::string::npos)
            return why = std::string("the resident resolve shader does not hold exactly one \"") + text + "\"", false;
    }
    out.replace(out.find(output), std::strlen(output), "layout(set = 1, binding = 0, r32ui) uniform writeonly uimage2D xe_dest;\nuint xe_word;");
    out.replace(out.find(entry), std::strlen(entry),
                "void XeResolveMain(uvec2 xe_pixel)\n{\n    ivec2 p = ivec2(xe_pixel + uvec2(xe_transfer.x0, xe_transfer.y0));");
    out += "layout(local_size_x = 8, local_size_y = 8) in;\n"
           "void main()\n"
           "{\n"
           "    uvec2 d = gl_GlobalInvocationID.xy;\n"
           "    if (d.x >= xe_transfer.width || d.y >= xe_transfer.height)\n"
           "        return;\n"
           "    XeResolveMain(d);\n"
           "    imageStore(xe_dest, ivec2(d), uvec4(xe_word, 0u, 0u, 0u));\n"
           "}\n";
    return true;
}

uint32_t Depth32To20e4(float depth)
{
    uint32_t u;
    std::memcpy(&u, &depth, 4);
    // X rb.cc:257-310 with remap_bias 0: below 2^-14 (0x38800000) the 20e4 denormal, else the exponent rebiased by -112.
    const uint32_t biased = u < 0x38800000u ? ((u & 0x7FFFFFu) | 0x800000u) >> std::min(113u - (u >> 23), 24u) : u - (112u << 23);
    return (biased >> 3) & 0xFFFFFFu;  // X rb.cc:323-325, truncating (depth_float24_round false)
}

float ConvertDepthFormat(float depth, edram::DepthFormat from, edram::DepthFormat to)
{
    return from == to ? depth : ColorWordDepth(PackDepthStencilWord(depth, 0, from), to);
}

float ColorWordDepth(uint32_t word, edram::DepthFormat format)
{
    return format == edram::DepthFormat::D24S8 ? UNorm24To32(word >> 8) : Float20e4To32(word >> 8);  // X :4101-4125
}

uint32_t Depth32ToUnorm24(float depth)
{
    const float d = depth > 0.0f ? std::min(depth, 1.0f) : 0.0f;          // edram_vk_plan.h (NaN -> 0)
    const float product = d * float(0xFFFFFF);                             // X :3600-3602, one float32 multiply
    return uint32_t(std::nearbyint(product));                              // X :3599 RoundEven (default rounding: to nearest even)
}

uint32_t PackDepthStencilWord(float depth, uint32_t stencil, edram::DepthFormat format)  // X :3591-3617
{
    return (stencil & 0xFF) | (format == edram::DepthFormat::D24S8 ? Depth32ToUnorm24(depth) : Depth32To20e4(depth)) << 8;
}

uint32_t Float32To7e3(float f)
{
    if (!(f > 0.0f))  // NaN, negatives, -0 (edram_vk_plan.h)
        return 0;
    uint32_t u;
    const float clamped = f < 31.875f ? f : 31.875f;  // X spirv_shader_translator_rb.cc:109-137 NClamp upper bound (+inf too)
    std::memcpy(&u, &clamped, 4);
    if (u < 0x3E800000u)  // below 0.25: a 7e3 denormal (X rb.cc:41-79)
        u = ((u & 0x7FFFFFu) | 0x800000u) >> std::min(125u - (u >> 23), 24u);
    else
        u -= 124u << 23;  // X rb.cc:80-86: the exponent rebiased from 127 to 3
    return ((u + 0x7FFFu + ((u >> 16) & 1u)) >> 16) & 0x3FFu;  // X rb.cc:95-106: round to nearest even
}

float Float7e3To32(uint32_t f10)
{
    f10 &= 0x3FF;
    if (f10 == 0)
        return 0.0f;
    uint32_t mantissa = f10 & 0x7F, exponent = f10 >> 7;
    if (exponent == 0)  // a denormal: normalize (X rb.cc:160-198: exponent findMSB(m) - 6, mantissa m << (7 - findMSB(m)))
    {
        const uint32_t msb = 31u - uint32_t(__builtin_clz(mantissa));
        exponent = msb - 6u;  // wraps below 0; (exponent + 124) below is still right modulo 2^32
        mantissa = (mantissa << (7u - msb)) & 0x7F;
    }
    const uint32_t bits = ((exponent + 124u) << 23) | (mantissa << 16);  // X rb.cc:210-233: the mantissa at bits 16-22
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint32_t PackTransferColor(const float rgba[4], uint32_t storage)
{
    const auto unorm = [](float v, float scale) { return uint32_t(v * scale + 0.5f); };  // X OpConvertFToU(v * scale + 0.5)
    if (storage == uint32_t(ColorFormat::k32_Float))  // X :3580-3585: the float's bits (the shader reads an R32_UINT view)
    {
        uint32_t bits;
        std::memcpy(&bits, &rgba[0], 4);
        return bits;
    }
    if (storage == uint32_t(ColorFormat::k8_8_8_8))
        return (unorm(rgba[0], 255.0f) & 0xFF) | (unorm(rgba[1], 255.0f) & 0xFF) << 8 | (unorm(rgba[2], 255.0f) & 0xFF) << 16 |
               (unorm(rgba[3], 255.0f) & 0xFF) << 24;
    if (storage == uint32_t(ColorFormat::k2_10_10_10_Float))  // X :3497-3542: alpha NClamp(a, 0, 1), NaN to 0
        return Float32To7e3(rgba[0]) | Float32To7e3(rgba[1]) << 10 | Float32To7e3(rgba[2]) << 20 |
               (unorm(rgba[3] > 0.0f ? std::min(rgba[3], 1.0f) : 0.0f, 3.0f) & 3) << 30;
    return (unorm(rgba[0], 1023.0f) & 0x3FF) | (unorm(rgba[1], 1023.0f) & 0x3FF) << 10 | (unorm(rgba[2], 1023.0f) & 0x3FF) << 20 |
           (unorm(rgba[3], 3.0f) & 3) << 30;
}

void UnpackTransferColor(uint32_t word, uint32_t storage, float rgba[4])
{
    if (storage == uint32_t(ColorFormat::k32_Float))  // X :3713-3726: the word is the float32 (written through an R32_UINT view)
    {
        std::memcpy(&rgba[0], &word, 4);
        rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }
    if (storage == uint32_t(ColorFormat::k8_8_8_8))
    {
        for (uint32_t i = 0; i < 4; i++)
            rgba[i] = float((word >> (8 * i)) & 0xFF) * (1.0f / 255.0f);
        return;
    }
    for (uint32_t i = 0; i < 3; i++)  // X :3669-3690 for 7e3
        rgba[i] = storage == uint32_t(ColorFormat::k2_10_10_10_Float) ? Float7e3To32(word >> (10 * i)) : float((word >> (10 * i)) & 0x3FF) * (1.0f / 1023.0f);
    rgba[3] = float(word >> 30) * (1.0f / 3.0f);
}

VkFormat TransferViewFormat(ColorFormat storage)
{
    switch (storage)
    {
    case ColorFormat::k32_Float: return VK_FORMAT_R32_UINT;   // X vulkan_render_target_cache.cc:1643-1644
    case ColorFormat::k16_16: return VK_FORMAT_R16G16_UINT;   // X :1637-1639
    case ColorFormat::k16_16_16_16: return VK_FORMAT_R16G16B16A16_UINT;  // FM4 Recomp (alpha, RT format 5): X :1640-1642
    default: return VK_FORMAT_UNDEFINED;
    }
}

uint32_t PackTransferUint(const uint32_t texel[4], uint32_t storage)
{
    if (storage == uint32_t(ColorFormat::k32_Float))  // X :3579-3585
        return texel[0];
    if (storage == uint32_t(ColorFormat::k16_16))  // X :3567-3574: BitFieldInsert(r, g, offset 16, width 16)
        return (texel[0] & 0xFFFF) | (texel[1] & 0xFFFF) << 16;
    return 0;
}

void UnpackTransferUint(uint32_t word, uint32_t storage, uint32_t texel[4])
{
    texel[0] = texel[1] = texel[2] = texel[3] = 0;
    if (storage == uint32_t(ColorFormat::k32_Float))  // X :3713-3726
        texel[0] = word;
    else if (storage == uint32_t(ColorFormat::k16_16))  // X :3701-3711: BitFieldUExtract(word, 0, 16), (word, 16, 16)
        texel[0] = word & 0xFFFF, texel[1] = word >> 16;
}

bool DecodeColorClear(uint32_t value, uint32_t storage, float rgba[4])
{
    if (storage == uint32_t(ColorFormat::k2_10_10_10_Float))  // X :5383-5390, with the shader-form decode (edram_vk_plan.h)
    {
        for (uint32_t j = 0; j < 3; j++)
            rgba[j] = Float7e3To32(value >> (j * 10));
        rgba[3] = float((value >> 30) & 3) * (1.0f / 3);
        return true;
    }
    if (storage == uint32_t(ColorFormat::k8_8_8_8))
    {
        for (uint32_t j = 0; j < 4; j++)
            rgba[j] = float((value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
        return true;
    }
    if (storage == uint32_t(ColorFormat::k2_10_10_10))
    {
        for (uint32_t j = 0; j < 3; j++)
            rgba[j] = float((value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
        rgba[3] = float((value >> 30) & 3) * (1.0f / 3);
        return true;
    }
    return false;
}

bool DecodeColorClearUint(uint32_t value, uint32_t storage, uint32_t rgba[4])
{
    if (storage == uint32_t(ColorFormat::k16_16))  // X :5393-5404: uint32[j] = (value >> 16j) & 0xFFFF, the raw halves
    {
        rgba[0] = value & 0xFFFF, rgba[1] = value >> 16, rgba[2] = rgba[3] = 0;
        return true;
    }
    if (storage != uint32_t(ColorFormat::k32_Float))
        return false;
    rgba[0] = value, rgba[1] = rgba[2] = rgba[3] = 0;  // X vulkan_render_target_cache.cc:5417-5421: uint32[0] = the clear word
    return true;
}

bool DecodeColorClearUint64(uint32_t value, uint32_t valueLo, uint32_t storage, uint32_t rgba[4])  // FM4 Recomp (alpha, 64 bpp resolve clear)
{
    if (storage != uint32_t(ColorFormat::k16_16_16_16))
        return false;
    const uint64_t clear = uint64_t(value) | uint64_t(valueLo) << 32;
    for (uint32_t j = 0; j < 4; j++)
        rgba[j] = uint32_t(clear >> (j * 16)) & 0xFFFF;
    return true;
}

float UNorm24To32(uint32_t n24)
{
    n24 &= 0xFFFFFF;
    return float(n24 + (n24 >> 23)) * (1.0f / float(1 << 24));
}

float Float20e4To32(uint32_t f24)
{
    f24 &= 0xFFFFFF;
    if (f24 == 0)
        return 0.0f;
    uint32_t mantissa = f24 & 0xFFFFF, exponent = f24 >> 20;
    if (exponent == 0)  // a denormal: normalize into the float (X xenos.cc:157-163)
    {
        const uint32_t shift = uint32_t(__builtin_clz(mantissa)) - (32 - 21);
        exponent = uint32_t(1 - int32_t(shift));
        mantissa = (mantissa << shift) & 0xFFFFF;
    }
    const uint32_t bits = ((exponent + 112) << 23) | (mantissa << 3);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void DecodeDepthClear(uint32_t value, DepthFormat format, float& depth, uint32_t& stencil)
{
    const uint32_t d24 = (value >> 8) & 0xFFFFFF;
    depth = format == DepthFormat::D24S8 ? UNorm24To32(d24) : Float20e4To32(d24);
    stencil = value & 0xFF;
}

// FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): edram_vk_plan.h.
bool ValueClear(Key dest, uint32_t word, const edram::Rect& rect, PendingClear& out)
{
    out = {};
    if (dest.Empty() || rect.width == 0 || rect.height == 0)
        return false;
    if (dest.IsDepth())
    {
        out.attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        DecodeDepthClear(word, DepthFormat(dest.Format()), out.attachment.clearValue.depthStencil.depth, out.attachment.clearValue.depthStencil.stencil);
    }
    else
    {
        out.attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        out.attachment.colorAttachment = 0;
        if (!DecodeColorClear(word, dest.Format(), out.attachment.clearValue.color.float32))  // storage 4 and 14 (transfer view) and the rest
            return false;
    }
    out.rect = {{int32_t(rect.x), int32_t(rect.y)}, {rect.width, rect.height}};
    return true;
}

// FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): edram_vk_plan.h.
bool ClearRequestWord(const ClearRequest& request, Key key, uint32_t& word)
{
    word = 0;
    if (key.Empty())
        return false;
    if (key.IsDepth())
    {
        if (!request.depth || !(request.depthValue >= 0.0f && request.depthValue <= 1.0f) || request.stencil > 0xFF)
            return false;
        const DepthFormat format = DepthFormat(key.Format());
        const uint32_t w = PackDepthStencilWord(request.depthValue, request.stencil, format);
        float depth = 0;
        uint32_t stencil = 0;
        DecodeDepthClear(w, format, depth, stencil);
        if (std::memcmp(&depth, &request.depthValue, sizeof depth) != 0 || stencil != request.stencil)
            return false;
        word = w;
        return true;
    }
    const uint32_t storage = key.Format();
    if (!request.color || (storage != uint32_t(ColorFormat::k8_8_8_8) && storage != uint32_t(ColorFormat::k2_10_10_10) &&
                           storage != uint32_t(ColorFormat::k2_10_10_10_Float)))
        return false;
    for (uint32_t i = 0; i < 4; i++)  // the format's range (PackTransferColor converts float -> uint: nothing negative or NaN reaches it)
    {
        const float limit = storage == uint32_t(ColorFormat::k2_10_10_10_Float) && i < 3 ? 31.875f : 1.0f;
        if (!(request.rgba[i] >= 0.0f && request.rgba[i] <= limit))
            return false;
    }
    const uint32_t w = PackTransferColor(request.rgba, storage);
    float back[4];
    if (!DecodeColorClear(w, storage, back) || std::memcmp(back, request.rgba, sizeof back) != 0)
        return false;
    word = w;
    return true;
}

edram::ResolveRegisters GatherResolveRegisters(RegisterRead read, const void* context)
{
    // Register indices: X register_table.inc, as edram.h ResolveRegisters lists them.
    edram::ResolveRegisters r;
    r.surfaceInfo = read(context, 0x2000);
    r.colorInfo[0] = read(context, 0x2001);
    r.depthInfo = read(context, 0x2002);
    for (uint32_t i = 1; i < 4; i++)
        r.colorInfo[i] = read(context, 0x2002 + i);  // RB_COLOR1_INFO .. RB_COLOR3_INFO 0x2003-0x2005
    r.modeControl = read(context, 0x2208);
    r.copyControl = read(context, 0x2318);
    r.copyDestBase = read(context, 0x2319);
    r.copyDestPitch = read(context, 0x231A);
    r.copyDestInfo = read(context, 0x231B);
    r.depthClear = read(context, 0x231D);
    r.colorClear = read(context, 0x231E);
    r.colorClearLo = read(context, 0x231F);
    r.vtxCntl = read(context, 0x2302);
    r.suScModeCntl = read(context, 0x2205);
    r.windowOffset = read(context, 0x2080);
    r.windowScissorTl = read(context, 0x2081);
    r.windowScissorBr = read(context, 0x2082);
    r.screenScissorTl = read(context, 0x200E);
    r.screenScissorBr = read(context, 0x200F);
    r.fetch0 = read(context, 0x4800);
    r.fetch1 = read(context, 0x4801);
    return r;
}

bool ResolveConvertible(const edram::ResolveInfo& info, uint32_t sourceStorage, std::string& why)
{
    if (!info.ok || !info.copy)
        return Refuse(why, "the resolve has no decoded copy");
    if (sourceStorage != info.source.Format() || info.source.IsDepth())
        return Refuse(why, "the read-back storage format " + std::to_string(sourceStorage) + " is not the source key's");
    const std::string shape = "resolve of " + KeyName(info.source) + ", sample select " + std::to_string(uint32_t(info.sampleSelect)) +
                              " to texture format " + std::to_string(info.destFormat) + " (exp bias " + std::to_string(info.destExpBias) + ", swap " +
                              std::to_string(int(info.destSwap)) + ", endian " + std::to_string(info.destEndian) + "): ";
    if (sourceStorage == uint32_t(ColorFormat::k2_10_10_10_Float))  // X draw_util.cc:1312-1347: always the full resolve shader
        return Refuse(why, shape + "a 7e3 source is never bitwise equivalent to a texture format (X xenos.h:616-641); X's converting resolve "
                                   "(unpack 7e3, average the selected samples, exp bias, swap, pack; shaders/resolve.xesli) is not implemented in M4");
    if (info.msaa != Msaa::X1 || info.sampleSelect != edram::SampleSelect::k0)
        return Refuse(why, shape + "a resolve from a " + MsaaName(info.msaa) + " source is not implemented in M4 (only 1x sources are observed)");
    const bool equivalent = (sourceStorage == uint32_t(ColorFormat::k8_8_8_8) && (info.destFormat == 6 || info.destFormat == 50)) ||
                            (sourceStorage == uint32_t(ColorFormat::k2_10_10_10) && (info.destFormat == 7 || info.destFormat == 54)) ||
                            (sourceStorage == uint32_t(ColorFormat::k32_Float) && info.destFormat == 36);  // xenos.h:634-635
    if (!equivalent)  // X takes its full (converting) resolve shader here (draw_util.cc:1312-1337); never observed
        return Refuse(why, shape + "a resolve from storage format " + std::to_string(sourceStorage) + " into texture format " +
                               std::to_string(info.destFormat) + " is a converting resolve (not bitwise equivalent, X xenos.h:616-641): not implemented in M4");
    if (info.destBase != info.destExtentStart)
        return Refuse(why, "the resolve's destination base is not the start of its extent");
    if (info.x1 <= info.x0 || info.y1 <= info.y0)
        return Refuse(why, "an empty resolve rectangle");
    return true;
}

namespace {
// FM4 Recomp (M7): the first texel (row-major) whose end, end(x, y), passes destBytes, as x | y << 32, or kNonePast; the rows are
// checked in parallel (ParallelRows), the first one found in row-major order wins, so the refusal names the serial loop's texel.
constexpr uint64_t kNonePast = ~uint64_t(0);
template <typename End>
uint64_t FirstPastEnd(uint32_t width, uint32_t height, uint32_t destBytes, End end)
{
    std::atomic<uint64_t> first{kNonePast};
    ParallelRows(height, width, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = 0; x < width; x++)
                if (end(x, y) > destBytes)
                {
                    const uint64_t at = uint64_t(x) | uint64_t(y) << 32, rank = uint64_t(y) * width + x;
                    uint64_t seen = first.load();
                    while ((seen == kNonePast || rank < uint64_t(uint32_t(seen >> 32)) * width + uint32_t(seen)) && !first.compare_exchange_weak(seen, at))
                    {
                    }
                    return;
                }
    });
    return first.load();
}
}  // namespace

bool ResolveToGuest(const edram::ResolveInfo& info, uint32_t sourceStorage, const uint32_t* texels, uint8_t* dest, uint32_t destBytes, std::string& why)
{
    if (!ResolveConvertible(info, sourceStorage, why))
        return false;
    const uint32_t width = info.x1 - info.x0, height = info.y1 - info.y0;
    // Every address first, so a refusal writes nothing.
    if (const uint64_t past = FirstPastEnd(width, height, destBytes, [&](uint32_t x, uint32_t y) {
            return uint64_t(TiledOffset(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 2)) + 4; });
        past != kNonePast)
        return Refuse(why, "resolve texel (" + std::to_string(uint32_t(past)) + ", " + std::to_string(uint32_t(past >> 32)) + ") lands past the destination's " +
                               std::to_string(destBytes) + " bytes");
    const bool is8888 = sourceStorage == uint32_t(ColorFormat::k8_8_8_8);
    ParallelRows(height, width, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            uint32_t w = texels[size_t(y) * width + x];
            // X resolve.xesli:769-797: the fast path swaps only 8_8_8_8 and the 2_10_10_10 family (32_FLOAT: no swap).
            if (info.destSwap && sourceStorage != uint32_t(ColorFormat::k32_Float))
                w = is8888 ? (w & ~0x00FF00FFu) | (w & 0xFFu) << 16 | ((w >> 16) & 0xFFu)
                           : (w & ~0x3FF003FFu) | (w & 0x3FFu) << 20 | ((w >> 20) & 0x3FFu);
            w = edram::GpuSwap32(w, info.destEndian);  // X resolve_fast_32bpp_1x2xmsaa.xesli:67-69 XeEndianSwap32
            std::memcpy(dest + TiledOffset(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 2), &w, 4);  // a little-endian store (arm64)
        }
    });
    return true;
}

// ---- The full (converting) resolve (edram_vk_plan.h) ------------------------------------------------------------------

bool ResolveUsesFastPath(const edram::ResolveInfo& info, uint32_t sourceStorage)
{
    std::string why;
    return info.destExpBias == 0 && ResolveConvertible(info, sourceStorage, why);  // draw_util.cc:1312-1318, restricted to 1x k0
}

uint32_t GuestSampleOfHost(Msaa msaa, uint32_t host)
{
    switch (msaa)
    {
    case Msaa::X4: return (host & 1) << 1 | (host >> 1 & 1);
    case Msaa::X2: return (host & 1) ^ 1;
    default: return 0;
    }
}

DumpLayout DumpLayoutFor(Msaa msaa, uint32_t width, uint32_t height)
{
    DumpLayout l;
    l.width = width, l.height = height;
    l.samples = msaa == Msaa::X4 ? 4 : msaa == Msaa::X2 ? 2 : 1;
    l.columns = l.samples >= 2 ? 2 : 1, l.rows = l.samples == 4 ? 2 : 1;
    return l;
}

bool ResolveSamples(const edram::ResolveInfo& info, uint32_t indices[4], uint32_t& count)
{
    const uint32_t sel = uint32_t(info.sampleSelect), samples = info.msaa == Msaa::X4 ? 4 : info.msaa == Msaa::X2 ? 2 : 1;
    if (sel <= uint32_t(edram::SampleSelect::k3))  // :206-210
        indices[0] = sel, count = 1;
    else if (sel == uint32_t(edram::SampleSelect::k01))  // the first sample 0 (:213-214), + the next row (:656-666): sample 1
        indices[0] = 0, indices[1] = 1, count = 2;
    else if (sel == uint32_t(edram::SampleSelect::k23))  // the first sample 2 (:210-211), + the next row: sample 3
        indices[0] = 2, indices[1] = 3, count = 2;
    else  // k0123: 0, the next row 1, the next column 2, its next row 3 (:656-692)
        indices[0] = 0, indices[1] = 1, indices[2] = 2, indices[3] = 3, count = 4;
    for (uint32_t i = 0; i < count; i++)
        if (indices[i] >= samples)
            return false;
    return true;
}

namespace {

// FM4 Recomp (alpha, 64 bpp resolve): one 16-bit EDRAM component of a 16_16 or 16_16_16_16 sample (pixel_formats.xesli:367-377
// XeUnpackR16G16Edram / XeUnpackR16G16B16A16Edram, with this port's clamp at -32: see the 16_16 branch below), factored out of it.
float UnpackR16Edram(uint32_t half)
{
    const float v = float(int16_t(uint16_t(half))) * (32.0f / 32767.0f);
    return v > -32.0f ? v : -32.0f;
}

void UnpackResolveSample(uint32_t w, uint32_t storage, float rgba[4])  // resolve.xesli:273-321
{
    if (storage == uint32_t(ColorFormat::k32_Float))  // :313-318 "Treat as 32_FLOAT": (f, 0, 0, 0)
    {
        std::memcpy(&rgba[0], &w, 4);
        rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }
    if (storage == uint32_t(ColorFormat::k8_8_8_8))  // pixel_formats.xesli XeUnpackR8G8B8A8UNorm
    {
        for (uint32_t i = 0; i < 4; i++)
            rgba[i] = float((w >> (8 * i)) & 0xFF) * (1.0f / 255.0f);
        return;
    }
    if (storage == uint32_t(ColorFormat::k16_16))  // :301-305: (XeUnpackR16G16Edram(w), 0, 0)
    {
        // pixel_formats.xesli:367-371: red the low half, green the high one, int16 * (32 / 32767): 0x7FFF is 32. Both -1 snorm
        // encodings (0x8001, 0x8000) are -1 before the scale, i.e. -32 after it: ShiftGlue 72b7dd6f fh1_native_edram.hlsli:217-220
        // DecodeSnorm16 "max(int16 / 32767, -1) * 32" (the D3D SNORM rule). Xenia 67f16c4 writes max(-1, int16 * 32 / 32767) (the
        // clamp after the scale, which flattens every value below -1 unit to -1; OBSERVED).
        // With snorm16_render_target_full_range (true by default,
        // render_target_cache.cc:188) X adds no +5 to the bias (draw_util.cc:1258-1266 behind vulkan_render_target_cache.h:143-147):
        // the host halves are the guest's (pipeline_key.h ColorExpBiasAccepted), read raw through the R16G16_UINT view
        // (edram_vk.cpp kResolveDump).
        for (uint32_t i = 0; i < 2; i++)
            rgba[i] = UnpackR16Edram(w >> (16 * i));  // FM4 Recomp (alpha, 64 bpp resolve): the same expression, shared
        rgba[2] = rgba[3] = 0.0f;
        return;
    }
    for (uint32_t i = 0; i < 3; i++)  // :331-335 XeUnpackR10G10B10A2UNorm, :328-349 XeUnpackR10G10B10A2Float
        rgba[i] = storage == uint32_t(ColorFormat::k2_10_10_10_Float) ? Float7e3To32(w >> (10 * i)) : float((w >> (10 * i)) & 0x3FF) * (1.0f / 1023.0f);
    rgba[3] = float(w >> 30) * (1.0f / 3.0f);
}

// A 16-bit channel in the destination's number format (xenia-canary d119505289d540f61ae3d4ba6168f1145625277a pixel_formats.xesli
// XePackFixed, "according to the Direct3D 11.3 functional specification"; the same arithmetic in ShiftGlue 72b7dd6f
// fh1_native_resolve_memory.cs.hlsl:218-232 PackChannel): 3 (signed integer) rounds the value half away from zero within
// [-32768, 32767] and keeps the low 16 bits; HLSL's max(NaN, x) is x, so NaN lands on -32768. Every other number packs UNORM
// (0, and the unexercised 1, 2 and 7: canary's "anything unexpected" branch).
uint32_t PackFixed16(float v, uint32_t number)
{
    if (number == 3)
    {
        const float clamped = !(v == v) ? -32768.0f : v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v;
        return uint32_t(int32_t(clamped + (v >= 0.0f ? 0.5f : -0.5f))) & 0xFFFFu;
    }
    return uint32_t(std::min(std::max(v, 0.0f), 1.0f) * 65535.0f + 0.5f);  // PackUnorm(v, 65535) below
}

uint32_t PackUnorm(float v, float scale)  // pixel_formats.xesli:116-125: uint(saturate(v) * scale + 0.5); NaN saturates to 0
{
    const float s = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
    return uint32_t(s * scale + 0.5f);
}

// FM4 Recomp (alpha, 64 bpp resolve): one component of xesl_packHalf2x16 (pixel_formats.xesli:269-277; GLSL packHalf2x16, HLSL
// f32tof16): the IEEE 754 binary32 -> binary16 conversion, rounding to nearest even, subnormals kept, overflow to infinity, NaN
// kept as a quiet NaN. APPROXIMATE: GLSL leaves the rounding of packHalf2x16 to the implementation and Xenos's own conversion is
// not documented; the values FM4's 16_16_16_16 source can hold (int16 * 32 / 32767, times 2^bias) lie far from a rounding tie.
uint32_t PackHalf(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u, exponent = (bits >> 23) & 0xFFu, mantissa = bits & 0x7FFFFFu;
    if (exponent == 0xFFu)  // infinity, or NaN with the quiet bit set
        return sign | 0x7C00u | (mantissa ? 0x200u | (mantissa >> 13) : 0u);
    const int32_t e = int32_t(exponent) - 127 + 15;  // the binary16 biased exponent
    if (e >= 31)
        return sign | 0x7C00u;
    if (e <= 0)
    {
        if (e < -10)  // below half the smallest subnormal (2^-25): zero
            return sign;
        // A subnormal: the significand with its implicit bit, shifted to units of 2^-24, rounded to nearest even (a carry into
        // bit 10 gives the smallest normal, which is the right encoding).
        const uint32_t significand = mantissa | 0x800000u, shift = uint32_t(14 - e);
        const uint32_t kept = significand >> shift, rest = significand & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        return sign | (kept + (rest > halfway || (rest == halfway && (kept & 1)) ? 1u : 0u));
    }
    const uint32_t kept = uint32_t(e) << 10 | mantissa >> 13, rest = mantissa & 0x1FFFu;
    return sign | (kept + (rest > 0x1000u || (rest == 0x1000u && (kept & 1)) ? 1u : 0u));  // a carry may round up to infinity
}

}  // namespace

uint32_t ResolveConvertPixel(const edram::ResolveInfo& info, uint32_t sourceStorage, const uint32_t* samplesByGuestIndex)
{
    uint32_t indices[4], count = 0;
    ResolveSamples(info, indices, count);
    float pixel[4];
    UnpackResolveSample(samplesByGuestIndex[indices[0]], sourceStorage, pixel);
    // dest_exp_bias_factor: the float with exponent bias + 127 (resolve.xesli:131-133), halved per averaging stage.
    const uint32_t biasBits = (uint32_t(info.destExpBias) << 23) + 0x3F800000u;  // modulo 2^32, as X's int arithmetic
    float exp;
    std::memcpy(&exp, &biasBits, 4);
    for (uint32_t i = 1; i < count; i++)
    {
        float s[4];
        UnpackResolveSample(samplesByGuestIndex[indices[i]], sourceStorage, s);
        for (uint32_t c = 0; c < 4; c++)
            pixel[c] += s[c];
    }
    if (count >= 2)
        exp *= 0.5f;  // :651
    if (count == 4)
        exp *= 0.5f;  // :670
    for (float& c : pixel)
        c *= exp;  // :695-698
    if (info.destSwap)  // :699-704 .bgra
        std::swap(pixel[0], pixel[2]);
    if (info.destFormat == 36)  // pixel_formats.xesli:247-252 (the default, "Treat as 32_FLOAT"): the red float's bits
    {
        uint32_t bits;
        std::memcpy(&bits, &pixel[0], 4);
        return bits;
    }
    // k_8_8 (pixel_formats.xesli:177-180 in XePack16bpp4PixelsInUInt2: XePackR8G8B8A8UNorm(float4(pixel_0.rg, pixel_1.rg)), :116-119),
    // so one pixel's 16 bits are uint(saturate(r) * 255 + 0.5) | uint(saturate(g) * 255 + 0.5) << 8; the upper half is 0 here.
    if (info.destFormat == 10)
        return PackUnorm(pixel[0], 255.0f) | PackUnorm(pixel[1], 255.0f) << 8;
    // k_16_16: red and green only, red the low half (pixel_formats.xesli:236-239), each in the destination's number format
    // (PackFixed16: FM4's number 3 packs the biased value as a signed 16-bit integer; Xenia 67f16c4 packed UNORM whatever the number,
    // xenia-canary d119505 added the number formats).
    if (info.destFormat == 25)
        return PackFixed16(pixel[0], info.destNumber) | PackFixed16(pixel[1], info.destNumber) << 16;
    if (info.destFormat == 6 || info.destFormat == 50)  // XePackR8G8B8A8UNorm
        return PackUnorm(pixel[0], 255.0f) | PackUnorm(pixel[1], 255.0f) << 8 | PackUnorm(pixel[2], 255.0f) << 16 | PackUnorm(pixel[3], 255.0f) << 24;
    return PackUnorm(pixel[0], 1023.0f) | PackUnorm(pixel[1], 1023.0f) << 10 | PackUnorm(pixel[2], 1023.0f) << 20 | PackUnorm(pixel[3], 3.0f) << 30;  // XePackR10G10B10A2UNorm
}

void ResolveConvertPixel64(const edram::ResolveInfo& info, uint32_t low, uint32_t high, uint32_t out[2])
{
    // FM4 Recomp (alpha, 64 bpp resolve): resolve.xesli:368-389 XeResolveUnpack64bpp2Samples (k_16_16_16_16: XeUnpackR16G16B16A16Edram,
    // pixel_formats.xesli:373-377: red the low half of the low word, green its high half, blue and alpha the high word's), the exponent
    // bias factor (resolve.xesli:131-133, :695-698; one sample, so no halving), the swap (.bgra, :699-704), then XePack64bpp4Pixels'
    // k_16_16_16_16_FLOAT branch (pixel_formats.xesli:269-277): packHalf2x16(r, g), packHalf2x16(b, a), the first component the low half.
    float pixel[4] = {UnpackR16Edram(low), UnpackR16Edram(low >> 16), UnpackR16Edram(high), UnpackR16Edram(high >> 16)};
    const uint32_t biasBits = (uint32_t(info.destExpBias) << 23) + 0x3F800000u;  // as ResolveConvertPixel
    float exp;
    std::memcpy(&exp, &biasBits, 4);
    for (float& c : pixel)
        c *= exp;
    if (info.destSwap)
        std::swap(pixel[0], pixel[2]);
    out[0] = PackHalf(pixel[0]) | PackHalf(pixel[1]) << 16;
    out[1] = PackHalf(pixel[2]) | PackHalf(pixel[3]) << 16;
}

bool ResolveFullConvertible(const edram::ResolveInfo& info, uint32_t sourceStorage, std::string& why)
{
    if (!info.ok || !info.copy)
        return Refuse(why, "the resolve has no decoded copy");
    if (sourceStorage != info.source.Format() || info.source.IsDepth())
        return Refuse(why, "the read-back storage format " + std::to_string(sourceStorage) + " is not the source key's");
    const std::string shape = "resolve of " + KeyName(info.source) + ", sample select " + std::to_string(uint32_t(info.sampleSelect)) +
                              " to texture format " + std::to_string(info.destFormat) + " (exp bias " + std::to_string(info.destExpBias) + ", swap " +
                              std::to_string(int(info.destSwap)) + ", endian " + std::to_string(info.destEndian) + "): ";
    const bool rg16 = sourceStorage == uint32_t(ColorFormat::k16_16);
    // FM4 Recomp (alpha, 64 bpp resolve): storage 5 (16_16_16_16) only into 32 (k_16_16_16_16_FLOAT), number 7, 1x sample 0, the 64 bpp
    // layout DecodeResolve sets (destBytesLog2 3), endian 0 or 8in16; and 32 only from storage 5.
    const bool rgba16 = sourceStorage == uint32_t(ColorFormat::k16_16_16_16);
    if (rgba16 != (info.destFormat == 32))
        return Refuse(why, shape + "texture format 32 pairs only with a 16_16_16_16 source (storage 5), and storage 5 only with 32");
    if (rgba16)
    {
        if (info.msaa != Msaa::X1 || info.sampleSelect != edram::SampleSelect::k0 || info.destNumber != 7 || info.destBytesLog2 != 3 ||
            info.destEndian > 1)
            return Refuse(why, shape + "not the 1x, sample 0, number 7, 64 bpp, endian 0 or 8in16 resolve DecodeResolve accepts from storage 5");
        if (info.destBase != info.destExtentStart)
            return Refuse(why, shape + "the destination base is not the start of its extent");
        if (info.x1 <= info.x0 || info.y1 <= info.y0)
            return Refuse(why, shape + "an empty resolve rectangle");
        return true;
    }
    if (info.destBytesLog2 == 3)
        return Refuse(why, shape + "a 64 bpp destination layout from a 32 bpp source");
    if (sourceStorage != uint32_t(ColorFormat::k8_8_8_8) && sourceStorage != uint32_t(ColorFormat::k2_10_10_10) &&
        sourceStorage != uint32_t(ColorFormat::k2_10_10_10_Float) && sourceStorage != uint32_t(ColorFormat::k32_Float) && !rg16)
        return Refuse(why, shape + "a source storage format other than 0, 2, 3, 4, 5 and 14");
    // 16_16 resolves only into k_16_16 (25), the observed pairing (edram::DecodeResolve), and 25 only from 16_16.
    if (rg16 != (info.destFormat == 25))
        return Refuse(why, shape + "texture format 25 pairs only with a 16_16 source (storage 4), and storage 4 only with 25");
    // A multisampled 32_FLOAT source: refused until observed (the 1x copy keeps the bits; the sample dump reads a transfer
    // view's raw words since storage 4, edram_vk.cpp kResolveDump XE_SOURCE_UINT, but no 32_FLOAT dump has been exercised).
    if (sourceStorage == uint32_t(ColorFormat::k32_Float) && info.msaa != Msaa::X1)
        return Refuse(why, shape + "a multisampled 32_FLOAT source (never observed)");
    if (!rg16 && info.destFormat != 6 && info.destFormat != 7 && info.destFormat != 10 && info.destFormat != 50 && info.destFormat != 54 &&
        info.destFormat != 36)
        return Refuse(why, shape + "a destination format other than 6, 7, 10, 36, 50 and 54");
    // k_8_8 (10, 16 bpp): from the UNORM / 7e3 sources (storage 0, 2, 3) whose unpack is exercised; a 32_FLOAT source is never
    // observed into it. The destination layout must be DecodeResolve's 16 bpp one, and its endian 0 or 8in16 (XeEndianSwap16).
    if (info.destFormat == 10 && sourceStorage == uint32_t(ColorFormat::k32_Float))
        return Refuse(why, shape + "a 32_FLOAT source into k_8_8 (never observed)");
    if ((info.destFormat == 10) != (info.destBytesLog2 == 1) || (info.destBytesLog2 == 1 && info.destEndian > 1))
        return Refuse(why, shape + "the destination's bytes per texel or endian is not the 16 bpp layout DecodeResolve sets for k_8_8");
    uint32_t indices[4], count = 0;
    if (!ResolveSamples(info, indices, count))
        return Refuse(why, shape + "a selected sample the source does not have");
    if (info.destBase != info.destExtentStart)
        return Refuse(why, shape + "the destination base is not the start of its extent");
    if (info.x1 <= info.x0 || info.y1 <= info.y0)
        return Refuse(why, shape + "an empty resolve rectangle");
    return true;
}

bool ResolveConvertToGuest(const edram::ResolveInfo& info, uint32_t sourceStorage, const uint32_t* words, uint8_t* dest, uint32_t destBytes,
                           std::string& why)
{
    if (!ResolveFullConvertible(info, sourceStorage, why))
        return false;
    const uint32_t width = info.x1 - info.x0, height = info.y1 - info.y0;
    if (info.destBytesLog2 == 3)
    {
        // FM4 Recomp (alpha, 64 bpp resolve): `words` is the 1x 16_16_16_16 source's rectangle as vkCmdCopyImageToBuffer gives it
        // (edram_vk.cpp Resolve): 8 bytes a pixel, row-major, the R16G16B16A16 texel's components in order, little-endian, i.e. the
        // EDRAM sample's two words low (r | g << 16) then high (b | a << 16) (TransferWordOf64; X resolve.xesli:368-389 reads a 64 bpp
        // sample as packed.xy). Each pixel's two destination words are XeEndianSwap 8in16 per 16-bit component (xenos.h:200-207
        // Endian128; texture_untile.h EndianSwap16Pair on each word, as ShiftGlue 72b7dd6f fh1_native_resolve_memory.cs.hlsl:343-353
        // does for its 16_16_16_16_FLOAT pack) stored low word first at TiledOffset2D(destOffsetX + x, destOffsetY + y, destPitch, 3)
        // (resolve.xesli:161-184 XeResolveDestPixelAddress with bpp_log2 3, texture_util.cc:443-458). Xenia's 64 bpp shader stores pixel
        // pairs as one R32G32B32A32 (resolve.xesli:186-197 XeResolveDestRightConsecutiveBlocksOffset; resolve_full_64bpp.xesli itself
        // is not in the local sparse checkout); at bpp_log2 3 pixel x | 1 is 8 bytes after x & ~1 (x bit 0 is bit 3 of the micro
        // offset, texture_util.cc:451-454), so per-pixel 8-byte stores at the per-pixel address write the same bytes.
        auto address = [&](uint32_t x, uint32_t y) { return TiledOffset2D(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 3); };
        if (const uint64_t past = FirstPastEnd(width, height, destBytes, [&](uint32_t x, uint32_t y) { return uint64_t(address(x, y)) + 8; });
            past != kNonePast)
            return Refuse(why, "resolve texel (" + std::to_string(uint32_t(past)) + ", " + std::to_string(uint32_t(past >> 32)) + ") lands past the destination's " +
                                   std::to_string(destBytes) + " bytes");
        ParallelRows(height, uint64_t(width) * 2, [&](uint32_t y0, uint32_t y1) {
            for (uint32_t y = y0; y < y1; y++)
                for (uint32_t x = 0; x < width; x++)
                {
                    const size_t i = (size_t(y) * width + x) * 2;
                    uint32_t packed[2];
                    ResolveConvertPixel64(info, words[i], words[i + 1], packed);
                    const uint32_t w[2] = {EndianSwap16Pair(packed[0], info.destEndian), EndianSwap16Pair(packed[1], info.destEndian)};
                    std::memcpy(dest + address(x, y), w, 8);  // little-endian stores (arm64), the low word first
                }
        });
        return true;
    }
    // FM4 Recomp: a 16 bpp destination (k_8_8) stores 2 bytes per texel at X's bpp_log2 1 tiled address (resolve_full_16bpp.xesli
    // :61-63 XeResolveDestPixelAddress(..., 1u), resolve.xesli:161-184 -> texture_util.cc:443-458, texture_untile.h TiledOffset2D).
    // X stores four pixels as one uint2 (pixel 0 in the low half); their tiled addresses are consecutive (x & 7 is the
    // linear micro term), so one 2-byte little-endian store per pixel writes the same bytes.
    const bool is16 = info.destBytesLog2 == 1;
    const uint32_t texelBytes = is16 ? 2 : 4;
    auto address = [&](uint32_t x, uint32_t y) {
        return is16 ? TiledOffset2D(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 1)
                    : TiledOffset(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 2);
    };
    // every address first: a refusal writes nothing
    if (const uint64_t past = FirstPastEnd(width, height, destBytes, [&](uint32_t x, uint32_t y) { return uint64_t(address(x, y)) + texelBytes; });
        past != kNonePast)
        return Refuse(why, "resolve texel (" + std::to_string(uint32_t(past)) + ", " + std::to_string(uint32_t(past >> 32)) + ") lands past the destination's " +
                               std::to_string(destBytes) + " bytes");
    const DumpLayout l = DumpLayoutFor(info.msaa, width, height);
    ParallelRows(height, uint64_t(width) * l.samples, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            uint32_t samples[4] = {};
            for (uint32_t g = 0; g < l.samples; g++)
                samples[g] = words[l.Word(x, y, g)];
            const uint32_t packed = ResolveConvertPixel(info, sourceStorage, samples);
            if (is16)
            {
                const uint16_t h = uint16_t(EndianSwap16Pair(packed, info.destEndian));  // resolve_full_16bpp.xesli:63 XeEndianSwap16
                std::memcpy(dest + address(x, y), &h, 2);  // a little-endian store (arm64)
                continue;
            }
            const uint32_t w = edram::GpuSwap32(packed, info.destEndian);  // resolve_full_32bpp.xesli:62-65
            std::memcpy(dest + address(x, y), &w, 4);
        }
    });
    return true;
}

bool ResolveDepthConvertible(const edram::ResolveInfo& info, std::string& why)
{
    if (!info.ok || !info.copy || !info.isDepth || !info.source.IsDepth())
        return Refuse(why, "the resolve has no decoded depth copy");
    const std::string shape = "depth resolve of " + KeyName(info.source) + ", sample select " + std::to_string(uint32_t(info.sampleSelect)) +
                              " to texture format " + std::to_string(info.destFormat) + " (endian " + std::to_string(info.destEndian) + "): ";
    const uint32_t expected = info.source.Format() == uint32_t(edram::DepthFormat::D24S8) ? 22u : 23u;  // X texture_info.h:170-180
    if (info.destFormat != expected || info.destExpBias != 0 || info.destSwap)
        return Refuse(why, shape + "not the k_24_8 / k_24_8_FLOAT bit copy DecodeResolve sets up");
    uint32_t indices[4], count = 0;
    if (!ResolveSamples(info, indices, count) || count != 1)
        return Refuse(why, shape + "not one sample of the source (X sanitizes depth to one, draw_util.cc SanitizeCopySampleSelect)");
    if (info.destBase != info.destExtentStart)
        return Refuse(why, shape + "the destination base is not the start of its extent");
    if (info.x1 <= info.x0 || info.y1 <= info.y0)
        return Refuse(why, shape + "an empty resolve rectangle");
    return true;
}

// FM4 Recomp: TiledOffset at 32 bpp (front_buffer.h) keeps the four texels of an aligned group (x & ~3) in one contiguous 16-byte run:
// x contributes bits 2-3 of the offset inside the 64-byte block (x & 3) and bit 5 (x & 4), nothing else below bit 6. So a tiled store
// computes one address per run and writes up to 16 bytes (tests/runtime/test_edram_vk.cpp TestTiledRuns checks the property over a
// grid and the result against a per-texel store). The row-worker time of the write-back was 12 % of the command processor's thread
// (6 ms of write-back a swap on the critical path).
bool TiledRunsFit(uint32_t destOffsetX, uint32_t destOffsetY, uint32_t destPitch, uint32_t width, uint32_t height, uint32_t destBytes, std::string& why)
{
    // FM4 Recomp (M7 CPU cost): the walk over every texel group costs ~4 % of the command-processor thread and the
    // same resolve shapes recur every frame: a shape that fitted once fits again (the answer depends only on the arguments).
    struct Shape
    {
        uint32_t x, y, pitch, w, h, bytes;
        bool operator==(const Shape& o) const { return x == o.x && y == o.y && pitch == o.pitch && w == o.w && h == o.h && bytes == o.bytes; }
    };
    static std::mutex fitLock;
    static std::vector<Shape> fitted;  // shapes proven to fit (a refusal is never cached: it stops the run)
    const Shape shape{destOffsetX, destOffsetY, destPitch, width, height, destBytes};
    {
        std::lock_guard<std::mutex> lock(fitLock);
        for (const Shape& f : fitted)
            if (f == shape)
                return true;
    }
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width;)
        {
            const uint32_t ax = destOffsetX + x, run = std::min<uint32_t>(4 - (ax & 3), width - x);
            const uint64_t end = uint64_t(TiledOffset(ax, destOffsetY + y, destPitch, 2)) + uint64_t(run) * 4;
            if (end > destBytes)
            {
                const uint64_t startByte = end - uint64_t(run) * 4;
                const uint32_t texel = x + uint32_t(destBytes > startByte ? (destBytes - startByte) / 4 : 0);
                return Refuse(why, "resolve texel (" + std::to_string(texel) + ", " + std::to_string(y) + ") lands past the destination's " +
                                       std::to_string(destBytes) + " bytes");
            }
            x += run;
        }
    std::lock_guard<std::mutex> lock(fitLock);
    if (fitted.size() < 4096)
        fitted.push_back(shape);
    return true;
}
// One tiled row of 32-bit words: `wordAt(x)` the texel's word before the endian swap.
// FM4_TILED_RUNS=1 (opt-in): the store in runs of four texels; neutral on the frame and on the workers' CPU
// (write-back 5.7 vs 5.8 ms a swap), so the per-texel store stays the default (a change is kept only when the frame improves).
bool TiledRuns()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_TILED_RUNS");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}
template <typename WordAt>
void StoreTiledRow32(uint8_t* dest, uint32_t destOffsetX, uint32_t ay, uint32_t destPitch, uint32_t width, uint32_t endian, WordAt wordAt)
{
    if (!TiledRuns())
    {
        for (uint32_t x = 0; x < width; x++)
        {
            const uint32_t w = edram::GpuSwap32(wordAt(x), endian);
            std::memcpy(dest + TiledOffset(destOffsetX + x, ay, destPitch, 2), &w, 4);
        }
        return;
    }
    for (uint32_t x = 0; x < width;)
    {
        const uint32_t ax = destOffsetX + x, run = std::min<uint32_t>(4 - (ax & 3), width - x);
        uint32_t w[4];
        for (uint32_t i = 0; i < run; i++)
            w[i] = edram::GpuSwap32(wordAt(x + i), endian);  // resolve_full_32bpp.xesli:62-65
        std::memcpy(dest + TiledOffset(ax, ay, destPitch, 2), w, size_t(run) * 4);
        x += run;
    }
}

// FM4 Recomp (M7 CPU cost): tile-major stores. Proof from TiledOffset at log2Bytes 2 (front_buffer.h, the formula of X
// texture_util.cc:443-458 GetTiledOffset2D): the pre-swizzle offset is macro + micro with macro = ((y >> 5) * TilesPerRow(pitch) +
// (x >> 5)) << 9 and micro = ((y & 14) << 5) + ((y & 1) << 4) + ((x & 4) << 3) + ((x & 3) << 2) <= 508 < 512, so `offset & ~0x1FF` is
// macro alone and the result is (macro << 3) + inTile = tile index * 4096 + inTile, where inTile = ((y & 16) << 7) +
// ((micro & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (micro & 0x3F) takes bit 11 from y & 16, bits 8-10 from y & 14,
// bits 6-7 from ((x >> 3) + ((y & 8) >> 2)) & 3, bit 5 from x & 4, bit 4 from y & 1 and bits 2-3 from x & 3 (bits 0-1 are 0). Hence
// (1) a 32x32 tile is one contiguous 4 KB block, the in-tile offset depends only on (x & 31, y & 31) and each of those ten bits owns an
// offset bit: a bijection onto the tile's 1,024 word slots; (2) the rows ye = y & ~1 and ye + 1 fill the contiguous 256-byte stripe at
// in-tile offset (y & 30) << 7, whose word s (0-63) holds the texel with x & 3 = s & 3, y & 1 = (s >> 2) & 1, (x >> 2) & 1 =
// (s >> 3) & 1 and ((x >> 3) + ((ye & 8) >> 2)) & 3 = s >> 4, i.e. run r = s >> 2 is the four consecutive texels from
// kTiledStripeRunX[(ye >> 3) & 1][r] of the row with parity r & 1 (edram_vk_plan.h; the static_assert below holds the table to the
// formula, tests/runtime/test_edram_vk.cpp TestTiledStripes holds both to Xenia's). The pitch only scales the tile index and the
// source's MSAA plays no part (the words are already one per destination texel), so the fast path covers every 32 bpp destination.
//
// StoreTiledRows32 writes the rectangle's `height` rows from a linear source (`src[y * stride + x]`). The work unit is a pair of
// destination rows (absolute ye even, ye + 1): for each tile column the pair covers whole (32 aligned texels inside the rectangle) it
// writes the stripe destination-sequentially, 16 runs of four words (four loads, the swaps, one 16-byte store; no address arithmetic per
// texel), so a thread that holds consecutive pairs writes a whole tile as 4 KB in order; the partial columns at the rectangle's sides and
// a row whose partner lies outside the rectangle keep the per-texel store. Destination-sequential rather than source-sequential: the
// writes go to guest memory that is cold (full 128-byte lines written whole, no partial-line fill), while the reads are two 128-byte
// source rows per stripe, hot in cache. The units (a lone leading row, the pairs, a lone trailing row) are built here and handed to
// ParallelRows, so no chunk boundary splits a pair; every byte a unit writes lies in a stripe of its own rows (the per-texel edge
// stores too: their row's stripe), so concurrent units never share a cache line and the result is the serial one byte for byte.
// TiledRunsFit has proven the per-texel addresses of the whole rectangle before this runs, and a stripe's 64 words are exactly those
// 64 texels' addresses (the bijection), so no store lands outside the proven set. FM4_TILED_STRIPES=0 restores the per-texel store
// (the A/B; the default changes only when the frame improves).
constexpr bool StripeTableMatchesTiledOffset()
{
    for (uint32_t ye = 0; ye < 32; ye += 2)
        for (uint32_t r = 0; r < 16; r++)
            for (uint32_t k = 0; k < 4; k++)
                if (TiledOffset(kTiledStripeRunX[(ye >> 3) & 1][r] + k, ye + (r & 1), 32, 2) != ((ye & 30) << 7) + (r * 4 + k) * 4)
                    return false;
    // the tile index part: tile (1, 2) at pitch 128 (4 tiles a row) is block 9, and the in-tile part is the same as at tile (0, 0)
    return TiledOffset(32 + 5, 64 + 3, 128, 2) == (9u << 12) + TiledOffset(5, 3, 32, 2);
}
static_assert(StripeTableMatchesTiledOffset(), "kTiledStripeRunX disagrees with TiledOffset");

bool TiledStripes()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_TILED_STRIPES");
        return v == nullptr || !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

void StoreTiledRows32(uint8_t* dest, uint32_t destOffsetX, uint32_t destOffsetY, uint32_t destPitch, uint32_t width, uint32_t height,
                      uint32_t endian, const uint32_t* src, size_t stride)
{
    const auto texels = [&](uint32_t y, uint32_t x0, uint32_t x1) {  // the per-texel store of row y (local), texels [x0, x1)
        if (x1 > x0)
            StoreTiledRow32(dest, destOffsetX + x0, destOffsetY + y, destPitch, x1 - x0, endian,
                            [&](uint32_t x) { return src[size_t(y) * stride + x0 + x]; });
    };
    // the tile columns the rectangle covers whole, absolute texels [fullX0, fullX1); none when fullX1 <= fullX0
    const uint32_t fullX0 = (destOffsetX + 31) & ~31u, fullX1 = (destOffsetX + width) & ~31u;
    if (height == 0)
        return;
    if (!TiledStripes() || fullX1 <= fullX0)
    {
        ParallelRows(height, width, [&](uint32_t y0, uint32_t y1) {
            for (uint32_t y = y0; y < y1; y++)
                texels(y, 0, width);
        });
        return;
    }
    const uint32_t leading = destOffsetY & 1;                  // 1: local row 0 is odd, its partner is above the rectangle
    const uint32_t paired = (height - leading) & ~1u;          // rows that pair up
    const uint32_t trailing = leading + paired < height ? 1 : 0;  // the last row's partner is below the rectangle
    const uint32_t units = leading + paired / 2 + trailing;
    ParallelRows(units, 2 * uint64_t(width), [&](uint32_t u0, uint32_t u1) {
        for (uint32_t u = u0; u < u1; u++)
        {
            if ((u == 0 && leading != 0) || (u + 1 == units && trailing != 0))
            {
                texels(u == 0 && leading != 0 ? 0 : height - 1, 0, width);
                continue;
            }
            const uint32_t y = 2 * u - leading;  // the pair's first (local) row; absolute ay is even
            const uint32_t ay = destOffsetY + y, rot = (ay >> 3) & 1;
            texels(y, 0, fullX0 - destOffsetX);
            texels(y + 1, 0, fullX0 - destOffsetX);
            texels(y, fullX1 - destOffsetX, width);
            texels(y + 1, fullX1 - destOffsetX, width);
            const uint32_t* rows[2] = {src + size_t(y) * stride, src + size_t(y + 1) * stride};
            for (uint32_t ax = fullX0; ax < fullX1; ax += 32)
            {
                uint8_t* stripe = dest + TiledOffset(ax, ay & ~31u, destPitch, 2) + ((ay & 30) << 7);  // the tile's block, the pair's stripe
                const uint32_t lx = ax - destOffsetX;
                for (uint32_t r = 0; r < 16; r++)
                {
                    const uint32_t* s = rows[r & 1] + lx + kTiledStripeRunX[rot][r];
                    uint32_t w[4];
                    for (uint32_t k = 0; k < 4; k++)
                        w[k] = edram::GpuSwap32(s[k], endian);  // resolve_full_32bpp.xesli:62-65
                    std::memcpy(stripe + r * 16, w, 16);
                }
            }
        }
    });
}

bool ResolveWordsToGuest(const edram::ResolveInfo& info, uint32_t sourceStorage, const uint32_t* words, uint8_t* dest, uint32_t destBytes,
                         std::string& why)
{
    if (!ResolveFullConvertible(info, sourceStorage, why))
        return false;
    const uint32_t width = info.x1 - info.x0, height = info.y1 - info.y0;
    if (info.destBytesLog2 == 1)
    {
        // FM4 Recomp (M7 CPU cost, FM4_RESOLVE_GPU_WORDS16): k_8_8's words are the R16_UINT resident image's halfwords (edram_vk.cpp
        // kResolveResident's destFormat 10 branch: r | g << 8, the low 16 bits of ResolveConvertPixel's result), tightly packed. Each
        // is stored exactly as ResolveConvertToGuest's 16 bpp branch stores ResolveConvertPixel's: XeEndianSwap16 of the halfword
        // (EndianSwap16Pair) at TiledOffset2D(destOffsetX + x, destOffsetY + y, destPitch, 1), 2 bytes; no float work. The bounds
        // check is that branch's (every address first; a refusal writes nothing).
        const uint16_t* words16 = reinterpret_cast<const uint16_t*>(words);
        auto address = [&](uint32_t x, uint32_t y) { return TiledOffset2D(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 1); };
        if (const uint64_t past = FirstPastEnd(width, height, destBytes, [&](uint32_t x, uint32_t y) { return uint64_t(address(x, y)) + 2; });
            past != kNonePast)
            return Refuse(why, "resolve texel (" + std::to_string(uint32_t(past)) + ", " + std::to_string(uint32_t(past >> 32)) + ") lands past the destination's " +
                                   std::to_string(destBytes) + " bytes");
        ParallelRows(height, width, [&](uint32_t y0, uint32_t y1) {
            for (uint32_t y = y0; y < y1; y++)
                for (uint32_t x = 0; x < width; x++)
                {
                    const uint16_t h = uint16_t(EndianSwap16Pair(words16[size_t(y) * width + x], info.destEndian));  // resolve_full_16bpp.xesli:63
                    std::memcpy(dest + address(x, y), &h, 2);  // a little-endian store (arm64)
                }
        });
        return true;
    }
    if (info.destBytesLog2 != 2)
        return Refuse(why, "GPU resolve words are 32 or 16 bpp; destination format " + std::to_string(info.destFormat) + " is neither");
    if (!TiledRunsFit(info.destOffsetX, info.destOffsetY, info.destPitch, width, height, destBytes, why))
        return false;
    StoreTiledRows32(dest, info.destOffsetX, info.destOffsetY, info.destPitch, width, height, info.destEndian, words, width);
    return true;
}

bool ResolveDepthToGuest(const edram::ResolveInfo& info, const uint32_t* words, uint8_t* dest, uint32_t destBytes, std::string& why)
{
    if (!ResolveDepthConvertible(info, why))
        return false;
    const uint32_t width = info.x1 - info.x0, height = info.y1 - info.y0;
    // every address first: a refusal writes nothing
    if (!TiledRunsFit(info.destOffsetX, info.destOffsetY, info.destPitch, width, height, destBytes, why))
        return false;
    uint32_t indices[4], count = 0;
    ResolveSamples(info, indices, count);
    const DumpLayout l = DumpLayoutFor(info.msaa, width, height);
    // FM4 Recomp (M7 CPU cost): the selected sample's words are linear in the dump: l.Word(x, y, g) = l.Word(0, 0, g) + y * (l.width *
    // l.columns) + x, so the tile-major store reads them as rows of that stride.
    StoreTiledRows32(dest, info.destOffsetX, info.destOffsetY, info.destPitch, width, height, info.destEndian, words + l.Word(0, 0, indices[0]),
                     size_t(l.width) * l.columns);
    return true;
}

float DepthSelfCheckDepth(uint32_t x, uint32_t row)
{
    return float(1 + (x * 37 + row * 1031) % 4095) / 8192.0f;  // k / 2^13, k < 2^12: exact in float32 and in 20e4
}

uint32_t DepthSelfCheckStencil(uint32_t x, uint32_t row)
{
    return (x * 7 + row * 13 + 1) & 0xFF;
}

uint32_t DepthSelfCheckWord(uint32_t x, uint32_t row)
{
    return PackDepthStencilWord(DepthSelfCheckDepth(x, row), DepthSelfCheckStencil(x, row), edram::DepthFormat::D24FS8);
}

uint32_t DepthSelfCheckExpected4x(uint32_t x, uint32_t y, uint32_t guestSample)
{
    // 4x guest sample g is host sample ((g & 1) << 1) | (g >> 1) (GuestSampleOfHost, its own inverse), at EDRAM sample
    // (2x + (h & 1), 2y + (h >> 1)) (X shaders/edram.xesli:23-33): column 2x + (g >> 1), row 2y + (g & 1).
    return DepthSelfCheckWord(2 * x + (guestSample >> 1 & 1), 2 * y + (guestSample & 1));
}

uint32_t DepthSelfCheckExpectedS8(uint32_t x, uint32_t row)
{
    const float s8 = ConvertDepthFormat(DepthSelfCheckDepth(x, row), edram::DepthFormat::D24FS8, edram::DepthFormat::D24S8);
    return PackDepthStencilWord(s8, DepthSelfCheckStencil(x, row), edram::DepthFormat::D24S8);
}

uint32_t DepthSelfCheckExpectedRoundTrip(uint32_t x, uint32_t row)
{
    const float s8 = ConvertDepthFormat(DepthSelfCheckDepth(x, row), edram::DepthFormat::D24FS8, edram::DepthFormat::D24S8);
    const float back = ConvertDepthFormat(s8, edram::DepthFormat::D24S8, edram::DepthFormat::D24FS8);
    return PackDepthStencilWord(back, DepthSelfCheckStencil(x, row), edram::DepthFormat::D24FS8);
}

uint32_t DepthSelfCheckExpected2x(uint32_t x, uint32_t y, uint32_t guestSample)
{
    return DepthSelfCheckWord(x, 2 * y + (guestSample & 1));  // X shaders/edram.xesli:23-33: 2x guest g at (x, 2y + g)
}

}  // namespace runtime::video
