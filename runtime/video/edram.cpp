// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The CPU-side EDRAM render-target model (edram.h). Every rule cites Xenia (X: xenia-canary 67f16c4e315f,
// src/xenia/gpu unless another directory is named; BSD-3-Clause), re-derived here, never copied.
#include "edram.h"
#include "front_buffer.h"  // TiledOffset: X texture_util.cc:443-459 GetTiledOffset2D (reviewed)
#include "texture_untile.h"  // TiledOffset2D, TiledAddressUpperBound2D (X texture_util.cc:443-458, :490-517) for 16 bpp destinations
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace runtime::video::edram {

namespace {

constexpr uint32_t kColorTargets = 4;

bool Refuse(std::string& refusal, std::string why)
{
    refusal = std::move(why);
    return false;
}

// Signed field of `bits` bits at `shift`.
constexpr int32_t SignedField(uint32_t v, uint32_t shift, uint32_t bits)
{
    const uint32_t field = (v >> shift) & ((1u << bits) - 1), sign = 1u << (bits - 1);
    return int32_t(field ^ sign) - int32_t(sign);
}

constexpr uint32_t AlignUp8(uint32_t v) { return (v + kResolveAlignment - 1) & ~(kResolveAlignment - 1); }

}  // namespace

// ---- Policy --------------------------------------------------------------------------------------------

bool SupportedKey(Key k, std::string& why)
{
    if (k.Empty() || k.PitchTilesAt32bpp() == 0)
        return Refuse(why, "a render target without a pitch");
    // 2x: a pixel is 1x2 samples (X render_target_cache.cc:591-592, shaders/edram.xesli:27-33); FM4's HDR resolve reads
    // its 4x 7e3 target as a 1280-wide 2x surface.
    if (k.IsDepth())
        return true;  // kD24S8 and kD24FS8
    switch (ColorFormat(k.Format()))
    {
    case ColorFormat::k8_8_8_8:
    case ColorFormat::k2_10_10_10:
    case ColorFormat::k2_10_10_10_Float:
        return true;  // the storage formats of 0, 1, 2, 10 and of 3, 12 (7e3: X xenos.h:394-404)
    case ColorFormat::k32_Float:
        // 32 bpp, its own storage format (X xenos.h:394-404 maps nothing onto it; xenos.h:323-327 Is64bpp lists 15 only):
        // FM4's HDR luminance reduction (RB_COLOR_INFO 000E0000).
        return true;
    case ColorFormat::k16_16:
        // FM4 Recomp: 32 bpp (two signed 16-bit components, the guest range -32..32); FM4's intro
        // (RB_COLOR_INFO 00540340). Host R16G16_SNORM (edram_vk_plan.h HostColorFormat). Its transfers go through
        // R16G16_UINT (edram_vk_plan.h TransferViewFormat); its resolves only into k_16_16 (DecodeResolve).
        return true;
    case ColorFormat::k16_16_Float:
        return Refuse(why, "colour format " + std::to_string(k.Format()) + " (16_16_FLOAT) is not supported yet");
    case ColorFormat::k16_16_16_16:
    case ColorFormat::k16_16_16_16_Float:
    case ColorFormat::k32_32_Float:
        return Refuse(why, "64 bpp colour format " + std::to_string(k.Format()) + " is not supported in M4");
    default:
        return Refuse(why, "colour format " + std::to_string(k.Format()) + " is unknown");
    }
}

// ---- RangeRectangles (X render_target_cache.cc:338-420) ----------------------------------------------------

uint32_t RangeRectangles(uint32_t startTiles, uint32_t endTiles, Key dest, Rect out[3])
{
    if (startTiles >= endTiles || endTiles > kTileCount || dest.PitchTiles() == 0)
        return 0;
    const uint32_t pitch = dest.PitchTiles();
    const uint32_t tileWidth = kTileWidthSamples >> (SamplesXLog2(dest.MsaaMode()) + (dest.Is64bpp() ? 1 : 0));  // :355-357
    const uint32_t tileHeight = kTileHeightSamples >> SamplesYLog2(dest.MsaaMode());                              // :358-359
    // A start below the base is the tail after wrapping (:364-368).
    const uint32_t localOffset = startTiles < dest.Base() ? kTileCount : 0;
    const uint32_t localStart = localOffset + startTiles - dest.Base(), localEnd = localOffset + endTiles - dest.Base();
    const uint32_t rowsStart = localStart / pitch, rowsEnd = (localEnd + pitch - 1) / pitch;  // :370-373
    const uint32_t rowFirstStart = localStart - rowsStart * pitch, rowLastEnd = pitch - (rowsEnd * pitch - localEnd);
    const uint32_t rows = rowsEnd - rowsStart;
    uint32_t n = 0;
    if (rows == 1 || rowFirstStart)  // :377-390
    {
        out[n++] = {rowFirstStart * tileWidth, rowsStart * tileHeight, ((rows == 1 ? rowLastEnd : pitch) - rowFirstStart) * tileWidth, tileHeight};
        if (rows == 1)
            return n;
    }
    uint32_t midRowsStart = rowsStart + 1, midRows = rows - 2;  // :391-399
    if (!rowFirstStart)
        --midRowsStart, ++midRows;
    if (rowLastEnd == pitch)
        ++midRows;
    if (midRows)  // :400-409
        out[n++] = {0, midRowsStart * tileHeight, pitch * tileWidth, midRows * tileHeight};
    if (rowLastEnd != pitch)  // :410-419
        out[n++] = {0, (rowsEnd - 1) * tileHeight, rowLastEnd * tileWidth, tileHeight};
    return n;
}

// ---- Ownership (X render_target_cache.cc:1539-1699) ------------------------------------------------------

uint32_t Ownership::Count(Key k) const
{
    uint32_t n = 0;
    for (uint32_t o : owner_)
        n += o == k.packed;
    return n;
}

namespace {

// Whether tile `tile` of `dest`'s layout lies entirely inside `cutout` (X render_target_cache.cc:422-498
// AddRectangle: a rectangle minus the cutout is empty exactly when the cutout contains it; here per tile).
bool TileInside(uint32_t tile, Key dest, const Rect& cutout)
{
    Rect r[3];
    if (RangeRectangles(tile, tile + 1, dest, r) != 1)
        return false;
    return r[0].x >= cutout.x && r[0].y >= cutout.y && r[0].x + r[0].width <= cutout.x + cutout.width && r[0].y + r[0].height <= cutout.y + cutout.height;
}

}  // namespace

bool Ownership::Change(Key dest, uint32_t start, uint32_t length, std::vector<Transfer>& transfers, std::string& refusal, const Rect* clearCutout)
{
    if (length == 0)
        return true;
    if (dest.Empty() || start > kTileCount - 1 || length > kTileCount)  // X :1543-1547 asserts
        return Refuse(refusal, "ownership change outside the EDRAM (start " + std::to_string(start) + ", length " + std::to_string(length) + ")");
    // X :1683-1697: the start wraps into dest's addressing period; a claim past tile 2047 continues at 0 up
    // to the start (the whole EDRAM at most).
    const uint32_t first = (dest.Base() + start) & (kTileCount - 1), end = first + length;
    const uint32_t extents[2][2] = {{first, std::min(end, kTileCount)}, {0, end > kTileCount ? std::min(end & (kTileCount - 1), first) : 0}};
    // Every owner change is accepted (FM4 Recomp: the depth-format refusal was the last one):
    // Depth -> colour is X's kDepthToColor (vulkan_render_target_cache.cc:4910-4913) and needs no host-depth owner: X tracks
    // those only for a depth destination (render_target_cache.cc:1553-1555, :1606-1609).
    // Colour -> depth is X's kColorToDepth (vulkan_render_target_cache.cc:4904-4908) with, for kD24FS8 on a float32 host, the
    // range's host-depth owner as a second source (render_target_cache.cc:1553-1555, :1606-1611): X writes that owner's float32
    // depth where its 20e4 conversion equals the colour word's depth bits, else the 20e4 bits converted
    // (vulkan_render_target_cache.cc:3741-4142). FM4 Recomp reduction: no host-depth owners, always the converted bits. That
    // differs from X only where the colour still holds the very depth bits the old depth target had, and then only below the
    // 20e4 quantum (edram_vk_plan.h ColorToDepth).
    // Depth -> depth between the formats is X's kDepthToDepth with the guest bits reinterpreted (edram_vk_plan.h
    // ConvertDepthFormat; FM4: tile 720, kD24FS8 -> kD24S8). X's host-depth owner refines only a kD24FS8
    // destination, so kD24FS8 -> kD24S8 is exact against X and kD24S8 -> kD24FS8 APPROXIMATE as colour -> depth above.
    for (const auto& e : extents)
        for (uint32_t t = e[0]; t < e[1]; t++)
        {
            const Key o = At(t);
            // :1599-1600: only a real, different owner is a source; :1601-1607: not a tile the clear overwrites.
            if (!o.Empty() && o != dest && !(clearCutout && clearCutout->width && clearCutout->height && TileInside(t, dest, *clearCutout)))
            {
                if (!transfers.empty() && transfers.back().endTiles == t && transfers.back().source == o)  // :1624-1632
                    transfers.back().endTiles = t + 1;
                else
                    transfers.push_back({t, t + 1, o});
            }
            owner_[t] = dest.packed;  // :1655
        }
    return true;
}

// ---- Bind (X render_target_cache.cc:574-1015) ---------------------------------------------------------------

Binding Bind(Ownership& ownership, const BindRegisters& regs, uint32_t heightUsed)
{
    Binding b;
    const SurfaceInfo surface = DecodeSurfaceInfo(regs.surfaceInfo);
    if (surface.msaa > uint32_t(Msaa::X4))  // :582-590
    {
        Refuse(b.refusal, "RB_SURFACE_INFO msaa_samples 3 is undefined (Xenos has 1x, 2x, 4x)");
        return b;
    }
    b.msaa = Msaa(surface.msaa);
    if (surface.pitch > kMaxSurfacePitch)  // :597-603
    {
        Refuse(b.refusal, "surface pitch " + std::to_string(surface.pitch) + " exceeds the guest maximum 8192");
        return b;
    }
    const uint32_t mode = regs.modeControl & 7;
    // X draw_util.cc:35-47: only kColorDepth and kDepth rasterize, and never with pitch 0 (:594-596 in Update).
    b.rasterizes = surface.pitch != 0 && (mode == uint32_t(ModeControl::ColorDepth) || mode == uint32_t(ModeControl::Depth));
    b.pitchTilesAt32bpp = PitchTilesAt32bpp(surface.pitch, b.msaa);
    b.ok = true;
    if (!b.rasterizes)
        return b;
    b.widthPixels = b.pitchTilesAt32bpp * (kTileWidthSamples >> SamplesXLog2(b.msaa));
    b.heightPixels = RenderTargetHeight(b.pitchTilesAt32bpp, b.msaa);

    // The targets the draw uses: [0] depth when depth or stencil testing is on (:627-636), [1..4] a colour
    // target with any write bit (:637-648; X also drops targets the pixel shader never writes, which the
    // model does not know: conservative). RB_MODECONTROL kDepth writes no colour (X draw_util.cc:726-731
    // GetNormalizedColorMask returns 0 unless kColorDepth).
    bool used[1 + kColorTargets] = {};
    uint32_t bases[1 + kColorTargets] = {};
    bool is64[1 + kColorTargets] = {};
    Key keys[1 + kColorTargets];
    // X draw_util.cc:60-79 GetNormalizedDepthControl: a depth test that always passes and never writes
    // (z_enable, !z_write_enable, zfunc kAlways = 7 in bits 4-6) needs no depth target; stencil is taken as is.
    uint32_t depthControl = regs.depthControl;
    if ((depthControl & 2) && !(depthControl & 4) && ((depthControl >> 4) & 7) == 7)
        depthControl &= ~2u;
    if (depthControl & 3)
    {
        const DepthInfo d = DecodeDepthInfo(regs.depthInfo);
        used[0] = true, bases[0] = d.base;
        keys[0] = Key::Depth(d.base, b.pitchTilesAt32bpp, b.msaa, DepthFormat(d.format));
    }
    const uint32_t colorMask = mode == uint32_t(ModeControl::ColorDepth) ? regs.colorMask : 0;
    for (uint32_t i = 0; i < kColorTargets; i++)
    {
        if (((colorMask >> (4 * i)) & 0xF) == 0)
            continue;
        const ColorInfo c = DecodeColorInfo(regs.colorInfo[i]);
        if (!IsKnownColorFormat(c.format))
        {
            Refuse(b.refusal, "RB_COLOR" + std::to_string(i) + "_INFO colour format " + std::to_string(c.format) + " is unknown");
            b.ok = false;
            return b;
        }
        used[1 + i] = true, bases[1 + i] = c.base, is64[1 + i] = Is64bpp(ColorFormat(c.format));
        keys[1 + i] = Key::Color(c.base, b.pitchTilesAt32bpp, b.msaa, StorageFormat(ColorFormat(c.format)));
    }
    // :688-719: a colour target with the same base as another used target drops the other when the other is
    // a higher-index colour target or the depth target (lower colour index and colour over depth win).
    for (uint32_t i = 1; i < 1 + kColorTargets; i++)
    {
        if (!used[i])
            continue;
        for (uint32_t j = 0; j < 1 + kColorTargets; j++)
            if (used[j] && j != i && (j == 0 || j > i) && bases[j] == bases[i])
                used[j] = false;
    }
    // The policy applies to the targets that survive the elimination, as X creates only those (:839-845).
    for (uint32_t i = 0; i < 1 + kColorTargets; i++)
        if (used[i] && !SupportedKey(keys[i], b.refusal))
        {
            b.ok = false;
            return b;
        }

    // :776-797: sorted by base, then by index.
    std::vector<std::pair<uint32_t, uint32_t>> sorted;
    for (uint32_t i = 0; i < 1 + kColorTargets; i++)
        if (used[i])
            sorted.emplace_back(bases[i], i);
    std::sort(sorted.begin(), sorted.end());
    if (sorted.empty())  // :730-752: nothing to bind (e.g. a memexport-only draw)
        return b;

    // :757-767: the rows the draw can touch, at most the host target's height.
    const uint32_t height = std::min(b.heightPixels, heightUsed);
    // :819-827 "as if 64 bpp": the smallest distance between bases bounds every target's claim.
    uint32_t maxDistance64 = kTileCount * 2;
    if (sorted.size() >= 2)
    {
        for (size_t i = 1; i < sorted.size(); i++)
            maxDistance64 = std::min(maxDistance64, (sorted[i].first - sorted[i - 1].first) << (is64[sorted[i - 1].second] ? 0 : 1));
        maxDistance64 = std::min(maxDistance64, (kTileCount + sorted[0].first - sorted.back().first) << (is64[sorted.back().second] ? 0 : 1));
    }
    // :844-846.
    const uint32_t lengthUsed32 = ((height << SamplesYLog2(b.msaa)) + kTileHeightSamples - 1) / kTileHeightSamples * b.pitchTilesAt32bpp;
    uint32_t lengths[1 + kColorTargets] = {};
    for (size_t i = 0; i < sorted.size(); i++)  // :847-857
    {
        const uint32_t index = sorted[i].second, base = sorted[i].first, shift = is64[index] ? 1 : 0;
        const uint32_t next = i + 1 < sorted.size() ? sorted[i + 1].first : kTileCount + sorted[0].first;
        lengths[index] = std::min(std::min(lengthUsed32 << shift, maxDistance64 >> (shift ^ 1)), next - base);
    }

    // :888-894: claim in base order; a refusal leaves the map as it was.
    const Ownership before = ownership;
    for (const auto& [base, index] : sorted)
    {
        (void)base;
        if (!ownership.Change(keys[index], 0, lengths[index], b.transfers[index], b.refusal))
        {
            ownership = before;
            b.ok = false;
            for (auto& t : b.transfers)
                t.clear();
            return b;
        }
    }
    b.depth = used[0] ? keys[0] : Key{};
    for (uint32_t i = 0; i < kColorTargets; i++)
        b.color[i] = used[1 + i] ? keys[1 + i] : Key{};
    for (uint32_t i = 0; i < 1 + kColorTargets; i++)
        b.lengthTiles[i] = used[i] ? lengths[i] : 0;
    return b;
}

// ---- Resolve decode (X draw_util.cc:759-1290) ----------------------------------------------------------------

int32_t FloatToFixed16p8(float f)  // X ui/graphics_util.cc:17-60
{
    if (!(std::fabs(f) >= 1.0f / 512.0f))
        return 0;
    if (f >= 32768.0f - 1.0f / 256.0f)
        return (1 << 23) - 1;
    if (f <= -32768.0f)
        return -32768 * 256;
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    union { int32_t s; uint32_t u; } r;
    r.u = (bits & ((1u << 23) - 1)) | (1u << 23);
    if (bits >> 31)
        r.s = -r.s;
    const int32_t exponent = int32_t((bits >> 23) & 255) - 127;
    const uint32_t extra = uint32_t(15 - exponent);
    if (extra)
    {
        r.u += (1u << (extra - 1)) - 1 + ((r.u >> extra) & 1);  // nearest even
        r.s >>= extra;
    }
    return r.s;
}

uint32_t GpuSwap32(uint32_t v, uint32_t endian)  // X xenos.h:1070-1086
{
    switch (endian & 3)
    {
    case 1: return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(v);
    case 3: return ((v >> 16) & 0xFFFF) | (v << 16);
    default: return v;
    }
}

namespace {

// X draw_util.cc:556-716 GetScissor without the surface-pitch clamp (GetResolveInfo passes false, :836-840):
// the window scissor plus the window offset unless disabled, intersected with the screen scissor, never
// negative; `extent` = max(br, tl) - tl. Returns {tl_x, tl_y, extent_x, extent_y}.
struct Scissor
{
    int32_t x, y, width, height;
};
Scissor ResolveScissor(const ResolveRegisters& r)
{
    int32_t tl[2] = {int32_t(r.windowScissorTl & 0x3FFF), int32_t((r.windowScissorTl >> 16) & 0x3FFF)};
    int32_t br[2] = {int32_t(r.windowScissorBr & 0x3FFF), int32_t((r.windowScissorBr >> 16) & 0x3FFF)};
    if (!(r.windowScissorTl >> 31))
    {
        const int32_t ox = SignedField(r.windowOffset, 0, 15), oy = SignedField(r.windowOffset, 16, 15);
        tl[0] += ox, tl[1] += oy, br[0] += ox, br[1] += oy;
    }
    const int32_t stl[2] = {SignedField(r.screenScissorTl, 0, 15), SignedField(r.screenScissorTl, 16, 15)};
    const int32_t sbr[2] = {SignedField(r.screenScissorBr, 0, 15), SignedField(r.screenScissorBr, 16, 15)};
    for (int i = 0; i < 2; i++)
    {
        tl[i] = std::max(std::max(tl[i], stl[i]), 0);
        br[i] = std::max(std::min(br[i], sbr[i]), 0);
        br[i] = std::max(br[i], tl[i]) - tl[i];
    }
    return {tl[0], tl[1], br[0], br[1]};
}

}  // namespace

ResolveInfo DecodeResolve(const ResolveRegisters& r)
{
    ResolveInfo info;
    auto refuse = [&](std::string why) { info.ok = false, info.refusal = std::move(why); return info; };
    if ((r.modeControl & 7) != uint32_t(ModeControl::Copy))
        return refuse("not a resolve: RB_MODECONTROL edram_mode " + std::to_string(r.modeControl & 7) + " is not kCopy (6)");
    // RB_COPY_CONTROL (X registers.h:796-810).
    const uint32_t srcSelect = r.copyControl & 7, sampleSelect = (r.copyControl >> 4) & 7, command = (r.copyControl >> 20) & 3;
    info.clearColor = (r.copyControl >> 8) & 1, info.clearDepth = (r.copyControl >> 9) & 1;
    if (command != uint32_t(CopyCommand::Raw) && command != uint32_t(CopyCommand::Convert))  // :773-780
        return refuse("resolve copy command " + std::to_string(command) + " is not raw (0) or convert (1)");
    info.command = CopyCommand(command);
    // The rectangle from vertex fetch constant 0: three 32_32_FLOAT vertices (:782-800). X xenos.h:1135-1148:
    // type bits 0-1 (3: vertex), endian bits 0-1 of the second word, size bits 2-25 in words.
    if ((r.fetch0 & 3) != 3 || ((r.fetch1 >> 2) & 0xFFFFFF) != 6)
        return refuse("resolve vertex fetch constant 0 is not a 6-word vertex fetch (type " + std::to_string(r.fetch0 & 3) + ", size " +
                      std::to_string((r.fetch1 >> 2) & 0xFFFFFF) + ")");
    const float halfPixel = (r.vtxCntl & 1) ? 0.0f : 0.5f;  // :801-803
    int32_t fixed[6];
    for (int i = 0; i < 6; i++)
    {
        const uint32_t bits = GpuSwap32(r.vertexWords[i], r.fetch1 & 3);
        float f;
        std::memcpy(&f, &bits, 4);
        fixed[i] = FloatToFixed16p8(f + halfPixel);
    }
    // :809-826: inclusive min, exclusive max, the top-left rule.
    int32_t x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8, y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
    int32_t x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8, y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
    if ((r.suScModeCntl >> 16) & 1)  // :830-836 the window offset
    {
        const int32_t ox = SignedField(r.windowOffset, 0, 15), oy = SignedField(r.windowOffset, 16, 15);
        x0 += ox, x1 += ox, y0 += oy, y1 += oy;
    }
    const Scissor s = ResolveScissor(r);  // :838-850
    x0 = std::clamp(x0, s.x, s.x + s.width), x1 = std::clamp(x1, s.x, s.x + s.width);
    y0 = std::clamp(y0, s.y, s.y + s.height), y1 = std::clamp(y1, s.y, s.y + s.height);
    // :854-861 align to 8: the origin down, the end up.
    x0 &= ~int32_t(kResolveAlignment - 1), y0 &= ~int32_t(kResolveAlignment - 1);
    x1 = int32_t(AlignUp8(uint32_t(x1))), y1 = int32_t(AlignUp8(uint32_t(y1)));
    const SurfaceInfo surface = DecodeSurfaceInfo(r.surfaceInfo);
    if (surface.msaa > uint32_t(Msaa::X4))  // :863-872
        return refuse("RB_SURFACE_INFO msaa_samples 3 is undefined (Xenos has 1x, 2x, 4x)");
    info.msaa = Msaa(surface.msaa);
    if (uint32_t(x1) > (surface.pitch & ~(kResolveAlignment - 1)))  // :874-883: X clamps and logs; refused here
        return refuse("resolve region " + std::to_string(x0) + " <= x < " + std::to_string(x1) + " is outside the surface pitch " + std::to_string(surface.pitch));
    if (y1 - y0 > int32_t(kMaxResolveSize))  // :886-892: X clamps and logs; refused here
        return refuse("resolve region is taller than " + std::to_string(kMaxResolveSize));
    if (x0 >= x1 || y0 >= y1)  // :895-901; nothing to copy or clear (X vulkan_render_target_cache.cc:1051-1054)
    {
        info.ok = true, info.empty = true;
        return info;
    }
    info.x0 = uint32_t(x0), info.y0 = uint32_t(y0), info.x1 = uint32_t(x1), info.y1 = uint32_t(y1);
    const uint32_t widthDiv8 = uint32_t(x1 - x0) >> 3, heightDiv8 = uint32_t(y1 - y0) >> 3;  // :903-906

    // The source (:915-917) and the sanitized sample selection (:918-928, SanitizeCopySampleSelect :495-545).
    info.isDepth = srcSelect >= kColorTargets;
    info.sourceIndex = info.isDepth ? kColorTargets : srcSelect;
    uint32_t sel = sampleSelect;
    if (info.msaa == Msaa::X4)
    {
        sel = std::min(sel, uint32_t(SampleSelect::k0123));
        if (info.isDepth && (sel == uint32_t(SampleSelect::k01) || sel == uint32_t(SampleSelect::k0123)))
            sel = uint32_t(SampleSelect::k0);
        else if (info.isDepth && sel == uint32_t(SampleSelect::k23))
            sel = uint32_t(SampleSelect::k2);
    }
    else if (info.msaa == Msaa::X2)
    {
        if (sel == uint32_t(SampleSelect::k2))
            sel = uint32_t(SampleSelect::k0);
        else if (sel == uint32_t(SampleSelect::k3))
            sel = uint32_t(SampleSelect::k1);
        else
            sel = std::min(sel, uint32_t(SampleSelect::k01));
        if (info.isDepth && sel == uint32_t(SampleSelect::k01))
            sel = uint32_t(SampleSelect::k0);
    }
    else
        sel = uint32_t(SampleSelect::k0);
    info.sampleSelect = SampleSelect(sel);

    info.pitchTilesAt32bpp = PitchTilesAt32bpp(surface.pitch, info.msaa);
    // RB_COPY_DEST_INFO (X registers.h:812-826): endian 0-2, array 3, slice 4-6, format 7-12, number 13-15, exp bias 16-21, swap 24.
    info.destEndian = r.copyDestInfo & 7;
    info.destPitch = r.copyDestPitch & 0x3FFF, info.destHeight = (r.copyDestPitch >> 16) & 0x3FFF;
    const uint32_t destFormatRaw = (r.copyDestInfo >> 7) & 0x3F, destNumber = (r.copyDestInfo >> 13) & 7;
    bool is64 = false;
    uint32_t sourceBase = 0;
    if (info.isDepth)
    {
        // A depth resolve (X draw_util.cc GetResolveInfo, line numbers of the 67f16c4 copy:
        // :1068-1093 the format, :1220-1236 the depth EDRAM info, :1268-1276 the patched
        // RB_COPY_DEST_INFO; GetCopyShader :1310-1324): the source
        // is the depth buffer at RB_DEPTH_INFO's base; the destination format is k_24_8 (22) for kD24S8 or k_24_8_FLOAT
        // (23) for kD24FS8 (X texture_info.h:170-180 DepthRenderTargetToTextureFormat) whatever copy_dest_format says
        // (Direct3D 9 writes k_8_8_8_8 there, X xenos.h:936-941); no exponent bias (:1220), no red/blue swap
        // (:1271-1274); always X's fast (bit-copy) shader of the guest word (depth << 8 | stencil) of the one selected
        // sample (the sanitization above leaves one sample for depth). copy_dest_format, copy_dest_number and the copy
        // command (raw or convert) change nothing in X's depth path, so they are not checked (the shape records them).
        const DepthInfo depth = DecodeDepthInfo(r.depthInfo);
        info.source = Key::Depth(depth.base, info.pitchTilesAt32bpp, info.msaa, DepthFormat(depth.format));
        if (std::string why; !SupportedKey(info.source, why))
            return refuse("depth resolve source: " + why);
        sourceBase = depth.base;
        info.destFormat = depth.format == uint32_t(DepthFormat::D24S8) ? 22u : 23u;
        info.destExpBias = 0, info.destSwap = false;
        const std::string shape = "depth resolve (" + std::string(depth.format ? "kD24FS8" : "kD24S8") + ", " + std::to_string(1u << uint32_t(info.msaa)) +
                                  "x, sample select " + std::to_string(uint32_t(info.sampleSelect)) + ", RB_COPY_DEST_INFO format " +
                                  std::to_string(destFormatRaw) + " number " + std::to_string(destNumber) + ", endian " +
                                  std::to_string(info.destEndian) + ", command " + std::to_string(command) + "): ";
        if ((r.copyDestInfo >> 3) & 1)
            return refuse(shape + "a resolve into a 3D (array) destination is not supported in M4");
        if (info.destEndian > 3)
            return refuse(shape + "resolve destination endian " + std::to_string(info.destEndian) + " (a 64/128-bit swap) is not supported in M4");
        if (info.destPitch == 0)
            return refuse(shape + "resolve destination pitch is 0");
    }
    else
    {
        // The source surface's Key at its original base (:1255-1278; the storage format as Bind keys it).
        const ColorInfo color = DecodeColorInfo(r.colorInfo[srcSelect]);
        if (!IsKnownColorFormat(color.format))
            return refuse("resolve source colour format " + std::to_string(color.format) + " is unknown");
        is64 = Is64bpp(ColorFormat(color.format));
        info.source = Key::Color(color.base, info.pitchTilesAt32bpp, info.msaa, StorageFormat(ColorFormat(color.format)));
        if (std::string why; !SupportedKey(info.source, why))
            return refuse("resolve source: " + why);
        sourceBase = color.base;

        // The destination (:929-1200).
        info.destFormat = destFormatRaw;
        info.destExpBias = SignedField(r.copyDestInfo, 16, 6);
        info.destSwap = (r.copyDestInfo >> 24) & 1;
        // Every destination-side refusal names the whole copy, so one stop records its complete shape (a 7e3 source's
        // resolve is always a converting one, X xenos.h:616-641, and its destination is not traced yet).
        const std::string shape = "resolve of colour " + std::to_string(srcSelect) + " (RB_COLOR_INFO format " + std::to_string(color.format) + ", storage " +
                                  std::to_string(info.source.Format()) + ", " + std::to_string(1u << uint32_t(info.msaa)) + "x, sample select " +
                                  std::to_string(uint32_t(info.sampleSelect)) + ") to texture format " + std::to_string(info.destFormat) + " (number " +
                                  std::to_string(destNumber) + ", exp bias " + std::to_string(info.destExpBias) + ", swap " +
                                  std::to_string(int(info.destSwap)) + ", endian " + std::to_string(info.destEndian) + ", command " + std::to_string(command) +
                                  "): ";
        if ((r.copyDestInfo >> 3) & 1)
            return refuse(shape + "a resolve into a 3D (array) destination is not supported in M4");
        if (info.destEndian > 3)
            return refuse(shape + "resolve destination endian " + std::to_string(info.destEndian) + " (a 64/128-bit swap) is not supported in M4");
        // 32 bpp destinations, and 10 (k_8_8, 16 bpp); bitwise-equivalent to the accepted sources (X xenos.h:616-641) or the
        // 8_8_8_8 <-> 2_10_10_10 unorm conversion that FM4's menus may use (HYPOTHESIS).
        // 36 = k_32_FLOAT (xenos.h:604), the bitwise equivalent of a 32_FLOAT render target (xenos.h:634-635).
        // 25 = k_16_16 (xenos.h:599) from a 16_16 source only (observed in FM4): never
        // bitwise equivalent (xenos.h:616-641 has no 16_16 case), so always X's full resolve (draw_util.cc:1312-1347).
        // 10 = k_8_8 (xenos.h:593; FM4: from an 8_8_8_8 source): never bitwise
        // equivalent either, so X's full 16 bpp resolve (draw_util.cc:1333-1334 kFull16bpp, shaders/resolve_full_16bpp.xesli).
        const bool rg16 = info.source.Format() == uint32_t(ColorFormat::k16_16);
        if (info.destFormat == 25 ? !rg16
                                  : info.destFormat != 6 && info.destFormat != 7 && info.destFormat != 10 && info.destFormat != 50 &&
                                        info.destFormat != 54 && info.destFormat != 36)
            return refuse(shape + "resolve destination texture format " + std::to_string(info.destFormat) +
                          " is not supported in M4 (6, 7, 10, 36, 50, 54 are; 25 from a 16_16 source)");
        // A 16 bpp store swaps with XeEndianSwap16 (resolve_full_16bpp.xesli:61-63), where only 8in16 does anything (endian.xesli
        // :25-31, texture_untile.h EndianSwap16Pair); 8in32 and 16in32 would be silent no-ops there: refused until observed.
        if (info.destFormat == 10 && info.destEndian > 1)
            return refuse(shape + "resolve destination endian " + std::to_string(info.destEndian) + " on a 16 bpp destination (only 0 and 8in16 are supported)");
        info.destBytesLog2 = info.destFormat == 10 ? 1 : 2;  // X draw_util.cc:1133 (texture_info_formats.inl:11 k_8_8: 16 bits)
        // RB_COPY_DEST_INFO copy_dest_number (X registers.h:819): Xenia 67f16c4 declares it and never reads it in its resolve path.
        // FM4 resolves its k_32_FLOAT target into texture format 36 with number 7 (kFloat, xenos.h:216-224; the local capture draw_loop2 (not distributed),
        // draw 31424): the natural value for float data. FM4 resolves its 16_16 target into texture format 25 with number 3
        // (kSignedInteger, xenos.h:221; the local capture scenario17 (not distributed) draw 484805); X packs UNORM whatever the number
        // (pixel_formats.xesli:144-147, :236-239) and so does this port: what the hardware does with number 3 is UNRESOLVED.
        // Accepted for those two pairings only; every other non-zero value stays refused.
        if (destNumber != 0 && !(destNumber == 7 && info.destFormat == 36) && !(destNumber == 3 && info.destFormat == 25))
            return refuse(shape + "resolve destination number format " + std::to_string(destNumber) + " is not unsigned repeating fraction (0)");
        // An exponent bias is X's full (converting) resolve's dest_exp_bias_factor (shaders/resolve.xesli:131-133), applied by
        // edram_vk_plan.cpp ResolveConvertToGuest; the range is the field's (6-bit signed, -32..31).
        if (info.destPitch == 0)
            return refuse(shape + "resolve destination pitch is 0");
        if (info.command == CopyCommand::Raw)
            return refuse(shape + "resolve copy command raw (0) has not been observed; convert (1) only in M4");
    }
    // :1146-1178 with bpp_log2 2: the base granularity is 32x32 texels (X xenos.h:1188-1195), so the copy's
    // origin is the 32x32 tile of (x0, y0) and the offset within it; the extent is X texture_util.h:297-303
    // GetTiledAddressLowerBound2D to texture_util.cc:490-518 GetTiledAddressUpperBound2D (+ 0x400 << 2).
    if (info.destBytesLog2 == 1)
    {
        // FM4 Recomp: a 16 bpp destination (k_8_8), the same lines with bpp_log2 1: the base granularity is 64x64 texels
        // (xenos.h:1188-1195: 7 - min(2, 1) = 6), the lower bound stays the 32x32 tile of (x0, y0) (texture_util.h:297-303),
        // the upper bound adds 0xC00 (texture_util.cc:507-511). texture_untile.h's token-for-token transcriptions.
        info.destOffsetX = info.x0 & 63, info.destOffsetY = info.y0 & 63;
        info.destBase = r.copyDestBase + TiledOffset2D(info.x0 & ~63u, info.y0 & ~63u, info.destPitch, 1);
        info.destExtentStart = r.copyDestBase + TiledOffset2D(info.x0 & ~31u, info.y0 & ~31u, info.destPitch, 1);
        info.destExtentLength = r.copyDestBase + TiledAddressUpperBound2D(info.x1, info.y1, info.destPitch, 1) - info.destExtentStart;
    }
    else
    {
        const uint32_t baseX = info.x0 & ~31u, baseY = info.y0 & ~31u;
        info.destOffsetX = info.x0 & 31, info.destOffsetY = info.y0 & 31;
        info.destBase = r.copyDestBase + TiledOffset(baseX, baseY, info.destPitch, 2);
        info.destExtentStart = info.destBase;
        const uint32_t extentEnd = r.copyDestBase + TiledOffset((info.x1 - 1) & ~31u, (info.y1 - 1) & ~31u, info.destPitch, 2) + (0x400u << 2);
        info.destExtentLength = extentEnd - info.destExtentStart;
    }
    info.copy = true;

    // The EDRAM span (:1202-1220, GetResolveEdramTileSpan :875-904).
    const uint32_t sx = SamplesXLog2(info.msaa), sy = SamplesYLog2(info.msaa);
    const uint32_t x0Samples = info.x0 << sx, y0Samples = info.y0 << sy;
    const uint32_t offsetTiles = (y0Samples / kTileHeightSamples) * info.pitchTilesAt32bpp + x0Samples / kTileWidthSamples;
    info.edramOffsetXDiv8 = (x0Samples % kTileWidthSamples) >> (sx + 3);
    info.edramOffsetYDiv8 = (y0Samples % kTileHeightSamples) >> (sy + 3);
    info.spanPitch = info.pitchTilesAt32bpp << (is64 ? 1 : 0);
    const uint32_t baseTiles = sourceBase + (offsetTiles << (is64 ? 1 : 0));  // the depth or colour base (X :1222-1256)
    const uint32_t xScale = 3 + sx + (is64 ? 1 : 0), yScale = 3 + sy;
    const uint32_t tx0 = (info.edramOffsetXDiv8 << xScale) / kTileWidthSamples;
    const uint32_t tx1 = (((info.edramOffsetXDiv8 + widthDiv8) << xScale) + kTileWidthSamples - 1) / kTileWidthSamples;
    const uint32_t ty0 = (info.edramOffsetYDiv8 << yScale) / kTileHeightSamples;
    const uint32_t ty1 = (((info.edramOffsetYDiv8 + heightDiv8) << yScale) + kTileHeightSamples - 1) / kTileHeightSamples;
    info.spanBase = baseTiles + ty0 * info.spanPitch + tx0;
    info.spanRowLength = tx1 - tx0, info.spanRows = ty1 - ty0;

    info.colorClear = r.colorClear, info.colorClearLo = r.colorClearLo, info.depthClear = r.depthClear;  // :1283-1285
    const DepthInfo depth = DecodeDepthInfo(r.depthInfo);  // :1224-1236
    info.depthBase = depth.base, info.depthFormat = DepthFormat(depth.format);
    info.ok = true;
    return info;
}

// ---- Resolve plan (X vulkan_render_target_cache.cc:1033-1200; render_target_cache.cc:1160-1360) -----------------

ResolvePlan PrepareResolve(Ownership& ownership, const ResolveInfo& info)
{
    ResolvePlan plan;
    auto refuse = [&](std::string why) { plan.ok = false, plan.refusal = std::move(why); return plan; };
    if (!info.ok)
        return refuse("resolve not decoded: " + info.refusal);
    if (info.empty)  // nothing to copy or clear: ownership unchanged (X vulkan_render_target_cache.cc:1051-1054)
    {
        plan.ok = true;
        return plan;
    }

    // The copy reads one host target, info.source, which first takes over the span's tiles (the header's note:
    // X dumps every owner into its EDRAM buffer instead, render_target_cache.cc:1074-1159). A tile no target
    // holds is refused before anything changes; a cross-aspect owner is refused by Change.
    const Ownership before = ownership;
    if (info.copy)
    {
        for (uint32_t row = 0; row < info.spanRows; row++)
            for (uint32_t i = 0; i < info.spanRowLength; i++)
            {
                const uint32_t tile = (info.spanBase + row * info.spanPitch + i) & (kTileCount - 1);
                if (ownership.At(tile).Empty())
                    return refuse("resolve reads tile " + std::to_string(tile) + " that no host target holds");
            }
        // Row by row (the span's rows are spanRowLength tiles at spanPitch apart), relative to the source's base
        // as Change takes it; adjacent rows of a full-width span coalesce into one transfer.
        const uint32_t relative = info.spanBase - info.source.Base();
        for (uint32_t row = 0; row < info.spanRows; row++)
        {
            std::string why;
            if (!ownership.Change(info.source, relative + row * info.spanPitch, info.spanRowLength, plan.copyTransfers, why))
            {
                ownership = before;
                plan.copyTransfers.clear();
                return refuse("resolve copy: " + why);
            }
        }
        plan.copy = true, plan.copySource = info.source;
        plan.copyRect = {info.x0, info.y0, info.x1 - info.x0, info.y1 - info.y0};
    }

    // Clears (X render_target_cache.cc:1160-1360 PrepareHostRenderTargetsResolveClear). A depth clear's
    // 20e4 value above 1.0 cannot be a Vulkan clear; refused before anything changes.
    const bool clearDepth = info.clearDepth, clearColor = info.clearColor && !info.isDepth;  // X draw_util.h:703-709
    if (!clearDepth && !clearColor)
    {
        plan.ok = true;
        return plan;
    }
    const uint32_t pitch32 = info.pitchTilesAt32bpp;  // :1168-1184 (the colour pitch >> 64 bpp; refused above for 64 bpp)
    const uint32_t sx = SamplesXLog2(info.msaa), sy = SamplesYLog2(info.msaa);
    const uint32_t pitchPixels = pitch32 * (kTileWidthSamples >> sx);  // :1211-1213
    const uint32_t rtHeight = RenderTargetHeight(pitch32, info.msaa);  // :1225-1226
    // The span's base relative to the original base, at 32 bpp (:1172-1183): the first tile of the rectangle.
    const uint32_t spanBaseOffset32 = info.spanBase - info.source.Base();
    const uint32_t baseOffsetRows = spanBaseOffset32 / pitch32;  // :1227-1228
    Rect rect;
    rect.x = std::min((spanBaseOffset32 - baseOffsetRows * pitch32) * (kTileWidthSamples >> sx) + (info.edramOffsetXDiv8 << 3), pitchPixels);
    rect.y = std::min(baseOffsetRows * (kTileHeightSamples >> sy) + (info.edramOffsetYDiv8 << 3), rtHeight);
    rect.width = std::min(info.x1 - info.x0, pitchPixels - rect.x);
    rect.height = std::min(info.y1 - info.y0, rtHeight - rect.y);
    if (rect.width == 0 || rect.height == 0)  // :1243-1247: outside the target, nothing to clear
    {
        plan.ok = true;
        return plan;
    }
    // :1252-1266: the tiles the rectangle covers, whole rows between its first and last tile.
    const uint32_t start32 = ((rect.y << sy) / kTileHeightSamples) * pitch32 + (rect.x << sx) / kTileWidthSamples;
    const uint32_t length32 = (((rect.y + rect.height - 1) << sy) / kTileHeightSamples) * pitch32 + ((rect.x + rect.width - 1) << sx) / kTileWidthSamples + 1 - start32;
    // :1267-1289: at most up to the base's next addressing period (64 bpp doubling refused above).
    uint32_t depthStart = 0, depthLength = 0, colorStart = 0, colorLength = 0;
    if (clearDepth)
        depthStart = std::min(start32, kTileCount), depthLength = std::min(start32 + length32, kTileCount) - depthStart;
    if (clearColor)
        colorStart = std::min(start32, kTileCount), colorLength = std::min(start32 + length32, kTileCount) - colorStart;
    const uint32_t depthBase = info.depthBase, colorBase = info.source.Base();
    if (depthLength && colorLength)  // :1290-1313: clear each only up to the other's start
    {
        const uint32_t dw = (depthBase + depthStart) & (kTileCount - 1), cw = (colorBase + colorStart) & (kTileCount - 1);
        depthLength = std::min(depthLength, (cw < dw ? kTileCount : 0) + cw - dw);
        colorLength = std::min(colorLength, (dw < cw ? kTileCount : 0) + dw - cw);
    }
    Key depthKey, colorKey;
    if (depthLength)
    {
        depthKey = Key::Depth(depthBase, pitch32, info.msaa, info.depthFormat);
        if (info.depthFormat == DepthFormat::D24FS8 && (info.depthClear >> 8) > 0xF00000u)
            return refuse("a 20e4 depth clear above 1.0 (RB_DEPTH_CLEAR " + std::to_string(info.depthClear) + ") cannot be a Vulkan clear");
    }
    if (colorLength)
        colorKey = info.source;
    std::string why;
    if (depthLength && !SupportedKey(depthKey, why))
        return refuse("resolve depth clear: " + why);
    if (!depthLength && !colorLength)  // :1341-1344 complete overlap
    {
        plan.ok = true;
        return plan;
    }
    if (depthLength)
    {
        plan.depthClear.target = depthKey, plan.depthClear.rect = rect, plan.depthClear.value = info.depthClear;
        if (!ownership.Change(depthKey, depthStart, depthLength, plan.depthClear.transfers, why, &rect))  // :1347-1352 with the cutout
        {
            ownership = before;
            return refuse("resolve depth clear: " + why);
        }
        plan.hasDepthClear = true;
    }
    if (colorLength)
    {
        plan.colorClear.target = colorKey, plan.colorClear.rect = rect, plan.colorClear.value = info.colorClear, plan.colorClear.valueLo = info.colorClearLo;
        if (!ownership.Change(colorKey, colorStart, colorLength, plan.colorClear.transfers, why, &rect))  // :1354-1359 with the cutout
        {
            ownership = before;
            return refuse("resolve colour clear: " + why);
        }
        plan.hasColorClear = true;
    }
    plan.ok = true;
    return plan;
}

// ---- Draw extent (X draw_extent_estimator.cc) ---------------------------------------------------------------

namespace {

float FloatBits(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// X draw_extent_estimator.cc:273-287 (bottom) and :303-308 (right): the window scissor's edge, + the window offset
// unless window_offset_disable, at most the screen scissor's edge.
int32_t ScissorEdge(const ExtentRegisters& r, bool y)
{
    int32_t edge = int32_t((r.windowScissorBr >> (y ? 16 : 0)) & 0x3FFF);
    if (!(r.windowScissorTl >> 31))
        edge += SignedField(r.windowOffset, y ? 16 : 0, 15);
    return std::min(edge, SignedField(r.screenScissorBr, y ? 16 : 0, 15));
}

}  // namespace

bool NeedsVertexMaxY(const ExtentRegisters& r)
{
    return ((r.clipCntl >> 16) & 1) && ScissorEdge(r, true) >= int32_t(kMaxSurfacePitch) && ScissorEdge(r, false) >= int32_t(kMaxSurfacePitch);
}

uint32_t EstimateMaxY(const ExtentRegisters& r, const uint32_t* vertexMaxY)
{
    uint32_t maxY = uint32_t(std::max(ScissorEdge(r, true), int32_t(0)));  // :287
    if ((r.clipCntl >> 16) & 1)  // :289-317
    {
        if (vertexMaxY && NeedsVertexMaxY(r))
            maxY = std::min(maxY, *vertexMaxY);
        return maxY;
    }
    // :318-363, the sums in X's order: window offset, half pixel, y offset, |y scale| (1 when disabled).
    float bottom = 0.0f;
    if ((r.suScModeCntl >> 16) & 1)
        bottom += float(SignedField(r.windowOffset, 16, 15));
    if (!(r.vtxCntl & 1))
        bottom += 0.5f;
    if ((r.vteCntl >> 3) & 1)
        bottom += FloatBits(r.vportYOffset);
    bottom += ((r.vteCntl >> 2) & 1) ? std::fabs(FloatBits(r.vportYScale)) : 1.0f;
    return uint32_t(std::min(float(maxY), std::max(0.0f, bottom)));  // :362, NaN and < 0 to 0
}

uint32_t VertexMaxY(const ExtentRegisters& r, const VertexPosition* positions, size_t count)
{
    const float yScale = ((r.vteCntl >> 2) & 1) ? FloatBits(r.vportYScale) : 1.0f;   // :147-149
    const float yOffset = ((r.vteCntl >> 3) & 1) ? FloatBits(r.vportYOffset) : 0.0f;  // :150-152
    float maxY = -FLT_MAX;
    for (size_t i = 0; i < count; i++)
    {
        float y = positions[i].y;
        if (!((r.vteCntl >> 8) & 1))  // :212-217
            y /= positions[i].w;
        y = y * yScale + yOffset;      // :219
        maxY = std::max(maxY, y);      // :240-242: NaN keeps the previous value
    }
    int32_t fixed = FloatToFixed16p8(maxY);  // :246
    if (!(r.vtxCntl & 1))                     // :253-255
        fixed += 128;
    if ((r.suScModeCntl >> 16) & 1)           // :256-258
        fixed += SignedField(r.windowOffset, 16, 15) * 256;
    const uint32_t msaa = (r.surfaceInfo >> 16) & 3;
    return (uint32_t(std::max(int32_t(0), fixed)) + (msaa == 0 ? 127u : 255u)) >> 8;  // :259-264
}

bool PassThroughPositions(const uint32_t* words, size_t wordCount, uint32_t endian, uint32_t strideDwords, uint32_t offsetDwords, uint32_t vertices,
                          std::vector<VertexPosition>& out, std::string& why)
{
    out.clear();
    if (strideDwords == 0 || offsetDwords + 3 > strideDwords)
        return Refuse(why, "a position xyz at dword " + std::to_string(offsetDwords) + " does not fit a " + std::to_string(strideDwords) + "-dword vertex");
    if (endian > 3)
        return Refuse(why, "vertex fetch endian " + std::to_string(endian) + " is not a 32-bit swap");
    if (vertices == 0)
        return true;
    if (size_t(vertices - 1) * strideDwords + offsetDwords + 2 > wordCount)
        return Refuse(why, "the vertex window (" + std::to_string(wordCount) + " dwords) is shorter than " + std::to_string(vertices) + " vertices of " +
                               std::to_string(strideDwords) + " dwords");
    for (uint32_t v = 0; v < vertices; v++)
        out.push_back({FloatBits(GpuSwap32(words[size_t(v) * strideDwords + offsetDwords + 1], endian)), 1.0f});
    return true;
}

}  // namespace runtime::video::edram
