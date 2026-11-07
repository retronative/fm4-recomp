// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// See edram_vk_plan.h. X = xenia-canary 67f16c4e315f src/xenia/gpu (BSD-3-Clause), re-derived, never copied.
#include "edram_vk_plan.h"
#include "draw_state.h"
#include "front_buffer.h"  // TiledOffset (X texture_util.cc:443-459 GetTiledOffset2D, reviewed)
#include "render_targets_vk.h"
#include <algorithm>
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
    const auto colorStorage = [](Key k)
    {
        return k.Format() == uint32_t(ColorFormat::k8_8_8_8) || k.Format() == uint32_t(ColorFormat::k2_10_10_10) ||
               k.Format() == uint32_t(ColorFormat::k2_10_10_10_Float);
    };
    if (source.IsDepth() && !dest.IsDepth())  // X kDepthToColor (:4910-4913); edram_vk_plan.h for the accepted shape
    {
        if (source.PitchTilesAt32bpp() != dest.PitchTilesAt32bpp())
            return refuse("depth -> colour between keys of different pitches (never observed)");
        if (source.MsaaMode() != Msaa::X1 || (dest.MsaaMode() != Msaa::X1 && dest.MsaaMode() != Msaa::X4))
            return refuse("depth -> colour from a 4x (or 2x) depth source or into a 2x target (needs multisampled depth and stencil sampling; never observed)");
        if (source.Format() != uint32_t(edram::DepthFormat::D24FS8))
            return refuse("depth -> colour from kD24S8 (X's RoundEven unorm24 packing; never observed)");
        if (!colorStorage(dest))
            return refuse("depth -> colour into a storage format other than 0, 2 and 3");
        return TransferKind::DepthToColor;
    }
    // Different bases go through the tile offset of MapEdramTexel (X uses one tile-offset path for every transfer mode,
    // vulkan_render_target_cache.cc:2938-2954); different pitches would need X's separate source pitch (not modelled).
    if (source.PitchTilesAt32bpp() != dest.PitchTilesAt32bpp())
        return refuse("between keys of different pitches (never observed; the texel mapping assumes equal pitches)");
    if (uint32_t(source.MsaaMode()) > 2 || uint32_t(dest.MsaaMode()) > 2)
        return refuse("msaa_samples 3 is undefined");
    // The MSAA pairs MapTransferTexel implements (X :2779-2864): equal, 1x <-> 4x, 4x <-> 2x (native 2x attachments: the
    // shape FM4's HDR resolve needs, the local capture draw_bases2 (not distributed)). 1x <-> 2x is never observed.
    const Msaa sm = source.MsaaMode(), dm = dest.MsaaMode();
    if (sm != dm && (sm == Msaa::X2 || dm == Msaa::X2) && (sm == Msaa::X1 || dm == Msaa::X1))
        return refuse("2x MSAA: 1x <-> 2x is not implemented (never observed; 4x <-> 2x and 2x <-> 2x are)");
    if (dest.IsDepth() && dm == Msaa::X2)
        return refuse("a transfer into a 2x depth target (never observed: the HDR resolve's depth clear claims its tiles under the cutout)");
    if (!source.IsDepth() && dest.IsDepth())  // X kColorToDepth + kColorToStencilBit (:170-171, :178-180, :4904-4908)
    {
        if (!colorStorage(source))
            return refuse("colour -> depth from a storage format other than 0, 2 and 3");
        if (dest.Format() != uint32_t(edram::DepthFormat::D24FS8))
            return refuse("colour -> depth into kD24S8 (X's unorm24 path; never observed)");
        return TransferKind::ColorToDepth;
    }
    if (dest.IsDepth())
    {
        if (source.Format() != dest.Format())
            return refuse("between depth formats (not modelled, unit 6b)");
        // Any accepted MSAA pair; a multisampled source is read with sampler2DMS / usampler2DMS (kTransferDepth,
        // kTransferStencilBit; observed: 2x depth at 1024 -> 4x depth at 720).
        return TransferKind::Depth;
    }
    const auto storage = [](Key k)
    {
        return k.Format() == uint32_t(ColorFormat::k8_8_8_8) || k.Format() == uint32_t(ColorFormat::k2_10_10_10) ||
               k.Format() == uint32_t(ColorFormat::k2_10_10_10_Float);
    };
    if (!storage(source) || !storage(dest))
        return refuse("a colour storage format other than 0 (8_8_8_8), 2 (2_10_10_10) and 3 (2_10_10_10_FLOAT, 7e3)");
    // An unchanged sample count with equal bases, pitches and formats is the same key (refused above): here the formats
    // differ, and the pixel goes through the 32-bit packing (X :3387-3700) at the same pixel and sample (MapTransferTexel).
    return TransferKind::Color;
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
    return {destX, destY, dest == Msaa::X1 ? 0 : destSample};
}

SourceTexel MapCrossAspectTexel(uint32_t destX, uint32_t destY, uint32_t destSample, Key source, Key dest)
{
    return MapEdramTexel(destX, destY, destSample, source, dest);
}

SourceTexel MapEdramTexel(uint32_t destX, uint32_t destY, uint32_t destSample, Key source, Key dest)
{
    const uint32_t pitch = dest.PitchTilesAt32bpp();
    const uint32_t dtw = edram::kTileWidthSamples >> edram::SamplesXLog2(dest.MsaaMode()), dth = edram::kTileHeightSamples >> edram::SamplesYLog2(dest.MsaaMode());
    const uint32_t stw = edram::kTileWidthSamples >> edram::SamplesXLog2(source.MsaaMode()), sth = edram::kTileHeightSamples >> edram::SamplesYLog2(source.MsaaMode());
    // X :2516-2577: the destination's 32bpp tile and the pixel within it.
    const uint32_t destTile = (destY / dth) * pitch + destX / dtw;
    SourceTexel p = MapTransferTexel(destX % dtw, destY % dth, destSample, source.MsaaMode(), dest.MsaaMode());  // tile-relative
    if (source.IsDepth() != dest.IsDepth())  // X :2916: only between colour and depth
    {
        const uint32_t half = edram::kTileWidthSamples >> (1 + edram::SamplesXLog2(source.MsaaMode()));  // X :2919-2920
        p.x = p.x < half ? p.x + half : p.x - half;                                                          // X :2921-2935
    }
    const uint32_t sourceTile = (destTile + dest.Base() - source.Base()) & (edram::kTileCount - 1);       // X :2938-2954
    p.x += (sourceTile % pitch) * stw, p.y += (sourceTile / pitch) * sth;
    return p;
}

uint32_t Depth32To20e4(float depth)
{
    uint32_t u;
    std::memcpy(&u, &depth, 4);
    // X rb.cc:257-310 with remap_bias 0: below 2^-14 (0x38800000) the 20e4 denormal, else the exponent rebiased by -112.
    const uint32_t biased = u < 0x38800000u ? ((u & 0x7FFFFFu) | 0x800000u) >> std::min(113u - (u >> 23), 24u) : u - (112u << 23);
    return (biased >> 3) & 0xFFFFFFu;  // X rb.cc:323-325, truncating (depth_float24_round false)
}

float ColorWordDepth(uint32_t word) { return Float20e4To32(word >> 8); }

uint32_t PackDepthStencilWord(float depth, uint32_t stencil) { return (stencil & 0xFF) | Depth32To20e4(depth) << 8; }  // X :3614-3617

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
                            (sourceStorage == uint32_t(ColorFormat::k2_10_10_10) && (info.destFormat == 7 || info.destFormat == 54));
    if (!equivalent)  // X takes its full (converting) resolve shader here (draw_util.cc:1312-1337); never observed
        return Refuse(why, shape + "a resolve from storage format " + std::to_string(sourceStorage) + " into texture format " +
                               std::to_string(info.destFormat) + " is a converting resolve (not bitwise equivalent, X xenos.h:616-641): not implemented in M4");
    if (info.destBase != info.destExtentStart)
        return Refuse(why, "the resolve's destination base is not the start of its extent");
    if (info.x1 <= info.x0 || info.y1 <= info.y0)
        return Refuse(why, "an empty resolve rectangle");
    return true;
}

bool ResolveToGuest(const edram::ResolveInfo& info, uint32_t sourceStorage, const uint32_t* texels, uint8_t* dest, uint32_t destBytes, std::string& why)
{
    if (!ResolveConvertible(info, sourceStorage, why))
        return false;
    const uint32_t width = info.x1 - info.x0, height = info.y1 - info.y0;
    // Every address first, so a refusal writes nothing.
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            if (uint64_t(TiledOffset(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 2)) + 4 > destBytes)
                return Refuse(why, "resolve texel (" + std::to_string(x) + ", " + std::to_string(y) + ") lands past the destination's " +
                                       std::to_string(destBytes) + " bytes");
    const bool is8888 = sourceStorage == uint32_t(ColorFormat::k8_8_8_8);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            uint32_t w = texels[size_t(y) * width + x];
            if (info.destSwap)  // X resolve.xesli:769-777
                w = is8888 ? (w & ~0x00FF00FFu) | (w & 0xFFu) << 16 | ((w >> 16) & 0xFFu)
                           : (w & ~0x3FF003FFu) | (w & 0x3FFu) << 20 | ((w >> 20) & 0x3FFu);
            w = edram::GpuSwap32(w, info.destEndian);  // X resolve_fast_32bpp_1x2xmsaa.xesli:67-69 XeEndianSwap32
            std::memcpy(dest + TiledOffset(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 2), &w, 4);  // a little-endian store (arm64)
        }
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

void UnpackResolveSample(uint32_t w, uint32_t storage, float rgba[4])  // resolve.xesli:273-300
{
    if (storage == uint32_t(ColorFormat::k8_8_8_8))  // pixel_formats.xesli XeUnpackR8G8B8A8UNorm
    {
        for (uint32_t i = 0; i < 4; i++)
            rgba[i] = float((w >> (8 * i)) & 0xFF) * (1.0f / 255.0f);
        return;
    }
    for (uint32_t i = 0; i < 3; i++)  // :331-335 XeUnpackR10G10B10A2UNorm, :328-349 XeUnpackR10G10B10A2Float
        rgba[i] = storage == uint32_t(ColorFormat::k2_10_10_10_Float) ? Float7e3To32(w >> (10 * i)) : float((w >> (10 * i)) & 0x3FF) * (1.0f / 1023.0f);
    rgba[3] = float(w >> 30) * (1.0f / 3.0f);
}

uint32_t PackUnorm(float v, float scale)  // pixel_formats.xesli:116-125: uint(saturate(v) * scale + 0.5); NaN saturates to 0
{
    const float s = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
    return uint32_t(s * scale + 0.5f);
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
    if (info.destFormat == 6 || info.destFormat == 50)  // XePackR8G8B8A8UNorm
        return PackUnorm(pixel[0], 255.0f) | PackUnorm(pixel[1], 255.0f) << 8 | PackUnorm(pixel[2], 255.0f) << 16 | PackUnorm(pixel[3], 255.0f) << 24;
    return PackUnorm(pixel[0], 1023.0f) | PackUnorm(pixel[1], 1023.0f) << 10 | PackUnorm(pixel[2], 1023.0f) << 20 | PackUnorm(pixel[3], 3.0f) << 30;  // XePackR10G10B10A2UNorm
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
    if (sourceStorage != uint32_t(ColorFormat::k8_8_8_8) && sourceStorage != uint32_t(ColorFormat::k2_10_10_10) &&
        sourceStorage != uint32_t(ColorFormat::k2_10_10_10_Float))
        return Refuse(why, shape + "a source storage format other than 0, 2 and 3");
    if (info.destFormat != 6 && info.destFormat != 7 && info.destFormat != 50 && info.destFormat != 54)
        return Refuse(why, shape + "a destination format other than 6, 7, 50 and 54");
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
    for (uint32_t y = 0; y < height; y++)  // every address first: a refusal writes nothing
        for (uint32_t x = 0; x < width; x++)
            if (uint64_t(TiledOffset(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 2)) + 4 > destBytes)
                return Refuse(why, "resolve texel (" + std::to_string(x) + ", " + std::to_string(y) + ") lands past the destination's " +
                                       std::to_string(destBytes) + " bytes");
    const DumpLayout l = DumpLayoutFor(info.msaa, width, height);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            uint32_t samples[4] = {};
            for (uint32_t g = 0; g < l.samples; g++)
                samples[g] = words[l.Word(x, y, g)];
            const uint32_t w = edram::GpuSwap32(ResolveConvertPixel(info, sourceStorage, samples), info.destEndian);  // resolve_full_32bpp.xesli:62-65
            std::memcpy(dest + TiledOffset(info.destOffsetX + x, info.destOffsetY + y, info.destPitch, 2), &w, 4);
        }
    return true;
}

}  // namespace runtime::video
