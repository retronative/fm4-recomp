// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The CPU-side EDRAM render-target model (execution model unit 6,
// "EDRAM ownership core"): pure bookkeeping, no Vulkan type, so tests/runtime/test_edram.cpp checks it
// alone. The Xenos keeps its render targets in 10 MB of EDRAM, 2048 tiles of 80x16 samples (5120 bytes);
// a title describes them with RB_SURFACE_INFO (pitch, MSAA), RB_COLOR_INFO (base tile, format) and
// RB_DEPTH_INFO (base tile, format), and may describe the same tiles as different surfaces (FM4: a 640x360
// 4x-MSAA scene and 1280x720 1x surfaces on the same 720 + 720 tiles, VERIFIED on ForzaRecomp, not yet on
// our disc). This model decides, per draw, which host render target (one VkImage
// per Key) holds the newest contents of each tile, and what the GPU side must copy before a draw or a
// resolve can use a target: the transfer list. It is the design of Xenia's render target cache, re-derived
// and reduced (xenia-canary 67f16c4e315f src/xenia/gpu/render_target_cache.{h,cc}, draw_util.{h,cc},
// xenos.h, registers.h; BSD-3-Clause; cited per function as "X file:line"):
//   - Key (X render_target_cache.h:269-339 RenderTargetKey): base tile, pitch in tiles at 32 bpp, MSAA,
//     colour/depth, storage format (X xenos.h:394-404 GetStorageColorFormat and
//     render_target_cache.h:703-711 GetColorResourceFormat: 10 -> 2, 12 -> 3, GAMMA -> 8_8_8_8; the
//     "key by storage format" rule, ReCarbon). Never the height.
//   - Ownership: one owner Key per tile (X render_target_cache.h:629-700 OwnershipRange, kept as a
//     2048-entry array rather than X's range map; the results are identical because X merges adjacent equal ranges
//     and extends a transfer only across adjacent tiles with the same source, which a tile walk reproduces).
//     Change (X render_target_cache.cc:1539-1699 ChangeOwnership) claims a tile range for a Key and emits a
//     Transfer from every previous owner. FM4 Recomp reduction (unit 6b restores it): X's separate
//     "host depth" owners (render_target_cache.h:629-660, for a depth target whose host encoding differs
//     from the guest's) are not tracked; a transfer between the two depth formats is REFUSED (fail closed), and a
//     colour -> depth transfer always converts the colour word's depth bits (X would use the host-depth owner's
//     float32 where its 20e4 conversion equals those bits: vulkan_render_target_cache.cc:3741-4142; X consults the
//     owners only for a depth destination, render_target_cache.cc:1553-1555). Depth -> colour is accepted (X
//     kDepthToColor), colour -> depth too (X kColorToDepth; FM4's observed case is a draw that overwrites every
//     transferred depth and stencil sample).
//   - Bind (X render_target_cache.cc:574-1015 Update): the targets a draw uses, duplicate-base elimination,
//     the tiles each claims (X's mrt_edram_used_range_clamp_to_min rule, .cc:164-175, 799-828) and the
//     transfers. FM4 Recomp reduction: no DrawExtentEstimator (.cc:762-767); the caller passes the rows the
//     draw can touch (the window scissor's bottom) or "unknown", and the model is conservative (more tiles
//     claimed, so more transfers, never fewer).
//   - Resolve (X draw_util.cc:759-1290 GetResolveInfo, :875-904 GetResolveEdramTileSpan;
//     ui/graphics_util.cc:17-60 FloatToD3D11Fixed16p8): the rectangle, the source target and its tile span,
//     the destination's address, extent, format, endian; resolve clears (X render_target_cache.cc:1160-1360
//     PrepareHostRenderTargetsResolveClear). PrepareResolve requires every tile of the copied span to be
//     owned by the source Key (X instead dumps every owner into its EDRAM buffer, .cc:1074-1159: unit 6b).
// The M4 policy (Supported*): only what the first frames need plus the cheap neighbours; everything else
// is refused with a reason that the renderer turns into a stop (no silent stubs). The arithmetic
// below the policy (64 bpp pitch doubling, 2x MSAA rows, wrapping) is general and tested.
//
// Every address or size here is in the guest's units (tiles, pixels, physical bytes); guest constants
// at or above 2^31 never appear.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime::video::edram {

constexpr uint32_t kTileWidthSamples = 80;   // X xenos.h:455
constexpr uint32_t kTileHeightSamples = 16;  // X xenos.h:456
constexpr uint32_t kTileCount = 2048;        // X xenos.h:457
constexpr uint32_t kTileBytes = kTileWidthSamples * kTileHeightSamples * 4;  // 5120 at 32 bpp
constexpr uint32_t kMaxSurfacePitch = 8192;  // X xenos.h:1161-1163 kTexture2DCubeMaxWidthHeight; also the height cap
constexpr uint32_t kResolveAlignment = 8;    // X xenos.h:1000-1001
constexpr uint32_t kMaxResolveSize = (1u << 14) - kResolveAlignment;  // X xenos.h:1005-1007

enum class Msaa : uint32_t { X1 = 0, X2 = 1, X4 = 2 };  // X xenos.h:288-292; 3 is undefined (refused)

// X xenos.h:299-319 ColorRenderTargetFormat.
enum class ColorFormat : uint32_t
{
    k8_8_8_8 = 0,
    k8_8_8_8_Gamma = 1,
    k2_10_10_10 = 2,
    k2_10_10_10_Float = 3,
    k16_16 = 4,
    k16_16_16_16 = 5,
    k16_16_Float = 6,
    k16_16_16_16_Float = 7,
    k2_10_10_10_As_10_10_10_10 = 10,
    k2_10_10_10_Float_As_16_16_16_16 = 12,
    k32_Float = 14,
    k32_32_Float = 15,
};
enum class DepthFormat : uint32_t { D24S8 = 0, D24FS8 = 1 };  // X xenos.h:406-410
enum class ModeControl : uint32_t { Ignore = 0, ColorDepth = 4, Depth = 5, Copy = 6 };  // X xenos.h:903-930

constexpr bool IsKnownColorFormat(uint32_t f) { return f <= 7 || f == 10 || f == 12 || f == 14 || f == 15; }
constexpr bool Is64bpp(ColorFormat f)  // X xenos.h:323-327
{
    return f == ColorFormat::k16_16_16_16 || f == ColorFormat::k16_16_16_16_Float || f == ColorFormat::k32_32_Float;
}
// The format whose host target holds this format's pixels: X xenos.h:394-404 GetStorageColorFormat, then
// X render_target_cache.h:703-711 GetColorResourceFormat (sRGB is a view property, not a different image).
constexpr ColorFormat StorageFormat(ColorFormat f)
{
    switch (f)
    {
    case ColorFormat::k8_8_8_8_Gamma: return ColorFormat::k8_8_8_8;
    case ColorFormat::k2_10_10_10_As_10_10_10_10: return ColorFormat::k2_10_10_10;
    case ColorFormat::k2_10_10_10_Float_As_16_16_16_16: return ColorFormat::k2_10_10_10_Float;
    default: return f;
    }
}

// ---- Register fields (X registers.h) -----------------------------------------------------------------

struct SurfaceInfo  // RB_SURFACE_INFO 0x2000 (X registers.h:618-628)
{
    uint32_t pitch;     // bits 0-13, pixels
    uint32_t msaa;      // bits 16-17 (Msaa; 3 undefined)
    uint32_t hizPitch;  // bits 18-31
};
constexpr SurfaceInfo DecodeSurfaceInfo(uint32_t v) { return {v & 0x3FFF, (v >> 16) & 3, v >> 18}; }

struct ColorInfo  // RB_COLOR_INFO 0x2001 (also 0x2003-0x2005; X registers.h:684-700)
{
    uint32_t base;     // bits 0-10, tiles (EDRAM addressing is periodic in 11 bits; bit 11 is kept apart by X)
    uint32_t format;   // bits 16-19 (ColorFormat)
    int32_t expBias;   // bits 20-25, signed
};
constexpr ColorInfo DecodeColorInfo(uint32_t v)
{
    return {v & 0x7FF, (v >> 16) & 0xF, int32_t(((v >> 20) & 0x3F) ^ 0x20) - 0x20};
}

struct DepthInfo  // RB_DEPTH_INFO 0x2002 (X registers.h:779-789)
{
    uint32_t base;    // bits 0-10, tiles
    uint32_t format;  // bit 16 (DepthFormat)
};
constexpr DepthInfo DecodeDepthInfo(uint32_t v) { return {v & 0x7FF, (v >> 16) & 1}; }

// ---- Tile arithmetic -------------------------------------------------------------------------------------

// A 4x pixel is 2x2 samples, a 2x pixel 1x2 (X render_target_cache.cc:591-592, 1036-1037; shaders/edram.xesli:27-33).
constexpr uint32_t SamplesXLog2(Msaa m) { return m == Msaa::X4 ? 1 : 0; }
constexpr uint32_t SamplesYLog2(Msaa m) { return m == Msaa::X1 ? 0 : 1; }
// X render_target_cache.cc:606-608 (= xenos.h:466-478 GetSurfacePitchTiles without the 64 bpp doubling).
constexpr uint32_t PitchTilesAt32bpp(uint32_t pitchPixels, Msaa m)
{
    return ((pitchPixels << SamplesXLog2(m)) + kTileWidthSamples - 1) / kTileWidthSamples;
}
// X render_target_cache.cc:1017-1043 GetRenderTargetHeight with resolution scale 1 and a host limit of at
// least 8192 (MoltenVK's maxFramebufferHeight is checked at device creation): the rows down to
// the next 11-bit addressing period, capped at 8192 pixel rows. The same height for every Key of a pitch
// and MSAA mode, so the host can rebind targets freely (X .h:528-537).
constexpr uint32_t RenderTargetHeight(uint32_t pitchTilesAt32bpp, Msaa m)
{
    if (pitchTilesAt32bpp == 0)
        return 0;
    uint32_t rows = (kTileCount + pitchTilesAt32bpp - 1) / pitchTilesAt32bpp;
    const uint32_t cap = (kMaxSurfacePitch << SamplesYLog2(m)) / kTileHeightSamples;
    if (rows > cap)
        rows = cap;
    return rows * (kTileHeightSamples >> SamplesYLog2(m));
}

// ---- Keys ---------------------------------------------------------------------------------------------

// X render_target_cache.h:269-339 RenderTargetKey, the same bit layout: base 0-10, pitch tiles at 32 bpp
// 11-18, MSAA 19-20, depth 21, format 22-25. Packed 0 is "no owner" (X .h:297-300: a real key has a pitch).
struct Key
{
    uint32_t packed = 0;

    static constexpr Key Color(uint32_t baseTiles, uint32_t pitchTilesAt32bpp, Msaa m, ColorFormat storage)
    {
        return {(baseTiles & 0x7FF) | (pitchTilesAt32bpp & 0xFF) << 11 | uint32_t(m) << 19 | uint32_t(storage) << 22};
    }
    static constexpr Key Depth(uint32_t baseTiles, uint32_t pitchTilesAt32bpp, Msaa m, DepthFormat f)
    {
        return {(baseTiles & 0x7FF) | (pitchTilesAt32bpp & 0xFF) << 11 | uint32_t(m) << 19 | 1u << 21 | uint32_t(f) << 22};
    }
    constexpr bool Empty() const { return packed == 0; }
    constexpr uint32_t Base() const { return packed & 0x7FF; }
    constexpr uint32_t PitchTilesAt32bpp() const { return (packed >> 11) & 0xFF; }
    constexpr Msaa MsaaMode() const { return Msaa((packed >> 19) & 3); }
    constexpr bool IsDepth() const { return (packed >> 21) & 1; }
    constexpr uint32_t Format() const { return (packed >> 22) & 0xF; }
    constexpr bool Is64bpp() const { return !IsDepth() && edram::Is64bpp(ColorFormat(Format())); }
    constexpr uint32_t PitchTiles() const { return PitchTilesAt32bpp() << (Is64bpp() ? 1 : 0); }  // X .h:321-323
    constexpr uint32_t Width() const { return PitchTilesAt32bpp() * (kTileWidthSamples >> SamplesXLog2(MsaaMode())); }  // X .h:324-332
    constexpr uint32_t Height() const { return RenderTargetHeight(PitchTilesAt32bpp(), MsaaMode()); }
    constexpr bool operator==(const Key&) const = default;
};

// The M4 whitelist: colour 8_8_8_8, 8_8_8_8_GAMMA, 2_10_10_10 and its _AS_10_10_10_10
// variant (storage 0 and 2), 2_10_10_10_FLOAT and its _AS_16_16_16_16 variant (storage 3, 7e3: X xenos.h:394-404
// GetStorageColorFormat, "the same packing and meaning of values ... without blending precision modifiers"); both
// 32_FLOAT (storage 14, 32 bpp); both depth formats; 1x, 2x and 4x MSAA. False with the reason otherwise (64 bpp formats,
// 32_32_FLOAT among them, by name).
bool SupportedKey(Key k, std::string& why);

// ---- Ownership ----------------------------------------------------------------------------------------

// A pixel rectangle of a Key's host target (X render_target_cache.h:367-372 Transfer::Rectangle).
struct Rect
{
    uint32_t x = 0, y = 0, width = 0, height = 0;
    constexpr bool operator==(const Rect&) const = default;
};

// Tiles [start, end) of `dest`'s layout as at most three rectangles of its pixels (X render_target_cache.cc:338-420
// GetRangeRectangles without the cutout; a wrapped range is passed as two calls as X requires, :342-348).
// Precondition: start < end <= 2048, and either start >= dest.Base() or end <= dest.Base() (the wrapped tail).
uint32_t RangeRectangles(uint32_t startTiles, uint32_t endTiles, Key dest, Rect out[3]);

// The host target `source` holds the newest contents of tiles [start, end) (never wrapping: a wrapped
// claim yields two transfers) that `dest` now needs. The GPU side copies them before the next use of dest:
// the rectangles in each image come from RangeRectangles(start, end, key, ...).
struct Transfer
{
    uint32_t startTiles = 0, endTiles = 0;
    Key source;
    constexpr bool operator==(const Transfer&) const = default;
};

class Ownership
{
public:
    Ownership() { owner_.fill(0); }
    Key At(uint32_t tile) const { return {owner_[tile % kTileCount]}; }
    uint32_t Count(Key k) const;  // tiles k owns

    // X render_target_cache.cc:1539-1699 ChangeOwnership: tiles [base + start, base + start + length) of
    // dest's addressing period (wrapping into the next, :1683-1697) become dest's; every tile that another
    // non-empty owner held is appended to `transfers` as a range from that owner (adjacent tiles with the
    // same owner coalesce, :1624-1632; an empty previous owner transfers nothing, :1599-1600).
    // Refuses, changing nothing, a transfer between the two depth formats (the header's reduction; unit 6b). Depth ->
    // colour and colour -> depth are accepted (the latter without X's host-depth owner: the header). `length` 0 does nothing. Precondition: start <= 2048 - (length != 0), length <= 2048.
    // `clearCutout` (a resolve clear's rectangle in dest's pixels, X .cc:1347-1359 passing &clear_rectangle):
    // a tile whose pixels lie entirely inside it is claimed but not transferred, since the clear overwrites it
    // (X :1601-1607 skips a range whose rectangles minus the cutout are empty, :338-420 + :422-498
    // GetRangeRectangles/AddRectangle, and copies only the remaining rectangles of the ranges it keeps,
    // vulkan_render_target_cache.cc:4505-4532). FM4 Recomp difference: the decision is per tile, so a kept
    // Transfer needs no cutout downstream (its RangeRectangles are what the GPU copies); the bytes moved are
    // the whole partially-covered tiles, a superset of X's border strips and a subset of X's kept ranges.
    bool Change(Key dest, uint32_t startTilesBaseRelative, uint32_t lengthTiles, std::vector<Transfer>& transfers,
                std::string& refusal, const Rect* clearCutout = nullptr);

private:
    std::array<uint32_t, kTileCount> owner_;
};

// ---- Bind: the targets of a draw ---------------------------------------------------------------------------

struct BindRegisters
{
    uint32_t surfaceInfo = 0;   // RB_SURFACE_INFO 0x2000
    uint32_t modeControl = 0;   // RB_MODECONTROL 0x2208 (X registers.h:609-616: edram_mode bits 0-2)
    uint32_t depthControl = 0;  // RB_DEPTHCONTROL 0x2200 (X registers.h:744-749: stencil_enable bit 0, z_enable bit 1)
    uint32_t colorMask = 0;     // RB_COLOR_MASK 0x2104 (X registers.h:702-720: 4 bits per target, RGBA from bit 0)
    uint32_t depthInfo = 0;     // RB_DEPTH_INFO 0x2002
    uint32_t colorInfo[4] = {}; // RB_COLOR_INFO, RB_COLOR1_INFO .. RB_COLOR3_INFO (0x2001, 0x2003-0x2005)
};

constexpr uint32_t kHeightUnknown = ~0u;

struct Binding
{
    bool ok = false;
    std::string refusal;
    // False: the draw rasterizes nothing (pitch 0, or RB_MODECONTROL not kColorDepth / kDepth: X draw_util.cc:35-47
    // IsRasterizationPotentiallyDone; X render_target_cache.cc:594-598 treats pitch 0 as "nothing to bind").
    // Nothing is claimed and no target is bound; ok stays true.
    bool rasterizes = false;
    uint32_t pitchTilesAt32bpp = 0;
    Msaa msaa = Msaa::X1;
    uint32_t widthPixels = 0, heightPixels = 0;  // the host size of every Key of this binding (Key::Width/Height)
    Key depth;                     // Empty: no depth/stencil target for this draw
    Key color[4];                  // Empty: not written by this draw
    uint32_t lengthTiles[5] = {};  // the tiles each claims from its base (0 depth, 1-4 colour)
    std::vector<Transfer> transfers[5];  // what the GPU side copies into each before the draw
};

// X render_target_cache.cc:574-1015 Update, reduced (header). `heightUsed`: the pixel rows the draw can
// touch (the caller passes the window scissor's bottom after the window offset, or kHeightUnknown), clamped
// to RenderTargetHeight. Refusals (nothing changed): MSAA 3; pitch > 8192; an unknown colour format; a Key
// outside the M4 whitelist; a cross-aspect transfer (Ownership::Change).
Binding Bind(Ownership& ownership, const BindRegisters& regs, uint32_t heightUsed);

// ---- Resolve: a draw in copy mode -----------------------------------------------------------------------

enum class CopyCommand : uint32_t { Raw = 0, Convert = 1, ConstantOne = 2, Null = 3 };  // X xenos.h:1009-1014
enum class SampleSelect : uint32_t { k0 = 0, k1, k2, k3, k01, k23, k0123 };          // X xenos.h:1017-1025

struct ResolveRegisters
{
    uint32_t surfaceInfo = 0;     // 0x2000
    uint32_t colorInfo[4] = {};   // 0x2001, 0x2003-0x2005
    uint32_t depthInfo = 0;       // 0x2002
    uint32_t modeControl = 0;     // 0x2208 (must be kCopy)
    uint32_t copyControl = 0;     // RB_COPY_CONTROL 0x2318 (X registers.h:796-810)
    uint32_t copyDestBase = 0;    // RB_COPY_DEST_BASE 0x2319: a physical address
    uint32_t copyDestPitch = 0;   // RB_COPY_DEST_PITCH 0x231A (X registers.h:828-837: pitch 0-13, height 16-29)
    uint32_t copyDestInfo = 0;    // RB_COPY_DEST_INFO 0x231B (X registers.h:812-826)
    uint32_t depthClear = 0;      // RB_DEPTH_CLEAR 0x231D
    uint32_t colorClear = 0;      // RB_COLOR_CLEAR 0x231E
    uint32_t colorClearLo = 0;    // RB_COLOR_CLEAR_LO 0x231F
    uint32_t vtxCntl = 0;         // PA_SU_VTX_CNTL 0x2302 (X registers.h:452-460: pix_center bit 0)
    uint32_t suScModeCntl = 0;    // PA_SU_SC_MODE_CNTL 0x2205 (X registers.h:435: vtx_window_offset_enable bit 16)
    uint32_t windowOffset = 0;    // PA_SC_WINDOW_OFFSET 0x2080 (X registers.h:561-569: signed 15-bit x bits 0-14, y 16-30)
    uint32_t windowScissorTl = 0; // PA_SC_WINDOW_SCISSOR_TL 0x2081 (X registers.h:572-582: x 0-13, y 16-29, offset disable bit 31)
    uint32_t windowScissorBr = 0; // PA_SC_WINDOW_SCISSOR_BR 0x2082 (X registers.h:585-593)
    uint32_t screenScissorTl = 0; // PA_SC_SCREEN_SCISSOR_TL 0x200E (X registers.h:539-547: signed 15-bit)
    uint32_t screenScissorBr = 0; // PA_SC_SCREEN_SCISSOR_BR 0x200F (X registers.h:550-558)
    uint32_t fetch0 = 0, fetch1 = 0;  // SHADER_CONSTANT_FETCH_00_0/1 0x4800-0x4801 (X xenos.h:1135-1148)
    // The six words at the fetch constant's address, each loaded as a little-endian 32-bit word from guest
    // memory (as X reads them, draw_util.cc:789-800) before the fetch constant's endian swap is applied here.
    uint32_t vertexWords[6] = {};
};

struct ResolveInfo
{
    bool ok = false;
    std::string refusal;
    // ok and empty: the clamped rectangle has no area; Xenia copies and clears nothing (X vulkan_render_target_cache.cc:1051-1054
    // "Nothing to copy/clear"; FM4's second HDR band). Nothing else of this ResolveInfo is filled in.
    bool empty = false;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // pixels of the source surface, multiples of 8 (x1, y1 exclusive)
    Msaa msaa = Msaa::X1;
    uint32_t pitchTilesAt32bpp = 0;
    bool isDepth = false;        // copy_src_select >= 4
    uint32_t sourceIndex = 0;    // the colour target (0-3) or 4 for depth
    Key source;                  // the source surface's Key at its original base (storage format)
    // X draw_util.cc:875-904 GetResolveEdramTileSpan: the tiles the rectangle covers: `spanRows` rows of
    // `spanRowLength` tiles from `spanBase` (not reduced modulo 2048), at `spanPitch` tiles a row.
    uint32_t spanBase = 0, spanRowLength = 0, spanRows = 0, spanPitch = 0;
    uint32_t edramOffsetXDiv8 = 0, edramOffsetYDiv8 = 0;  // of the rectangle within its first tile
    bool copy = false;  // false: a clear-only resolve
    CopyCommand command = CopyCommand::Raw;
    SampleSelect sampleSelect = SampleSelect::k0;  // sanitized (X draw_util.cc:495-545)
    uint32_t destFormat = 0;    // xenos ColorFormat (texture format) of the destination; a depth resolve: 22 k_24_8 / 23 k_24_8_FLOAT
    uint32_t destEndian = 0;    // Endian128 0-3 (X xenos.h:200-207)
    int32_t destExpBias = 0;
    bool destSwap = false;      // red/blue exchange (copy_dest_swap)
    uint32_t destPitch = 0, destHeight = 0;  // texels (RB_COPY_DEST_PITCH)
    // Bytes per destination texel, log2 (X draw_util.cc:1133 bpp_log2 from texture_info_formats.inl): 2 (32 bpp), or 1 for
    // texture format 10 (k_8_8, 16 bpp, texture_info_formats.inl:11). A depth resolve: 2.
    uint32_t destBytesLog2 = 2;
    // Physical address of the base-granularity tile holding (x0, y0): X draw_util.cc:1134-1173 (32x32 texels at 32 bpp, 64x64
    // at 16 bpp, xenos.h:1188-1195).
    uint32_t destBase = 0;
    uint32_t destOffsetX = 0, destOffsetY = 0;  // (x0, y0) within that tile, pixels
    uint32_t destExtentStart = 0, destExtentLength = 0;  // the guest bytes the copy may write (for the write watch, unit 7)
    bool clearColor = false, clearDepth = false;
    uint32_t colorClear = 0, colorClearLo = 0, depthClear = 0;
    uint32_t depthBase = 0;  // RB_DEPTH_INFO's base and format, for a depth clear (X draw_util.cc:1224-1236)
    DepthFormat depthFormat = DepthFormat::D24S8;
};

// X draw_util.cc:759-1290 GetResolveInfo (the parts that do not depend on the host renderer), fail closed:
// Pure: no ownership change.
ResolveInfo DecodeResolve(const ResolveRegisters& regs);

struct ResolveClear
{
    Key target;   // the host target to clear (at the original base: X render_target_cache.cc:1318-1345)
    Rect rect;    // its pixels
    uint32_t value = 0, valueLo = 0;  // the raw clear words (X draw_util.h:720-741)
    std::vector<Transfer> transfers;  // what the target needs first: only tiles not wholly inside `rect` (X .cc:1347-1359, the cutout)
};

struct ResolvePlan
{
    bool ok = false;
    std::string refusal;
    // The copy reads `copyRect` of the one host target `copySource` (info.source). The span's tiles become its
    // first: `copyTransfers` is what other owners held there (FM4 resolves the 1x surface in frames where the
    // 4x clear was the last writer of the colour tiles). X instead
    // dumps every owner's rectangles into its EDRAM buffer and resolves from there (render_target_cache.cc:1074-
    // 1159); claiming the span for the source target is the host-target form of the same data movement, and
    // leaves the tiles where the next 1x draw wants them. A tile no target holds is refused (nothing drew it).
    bool copy = false;
    Key copySource;
    Rect copyRect;
    std::vector<Transfer> copyTransfers;
    bool hasDepthClear = false, hasColorClear = false;
    ResolveClear depthClear, colorClear;
};

// Applies the resolve's ownership effects (X vulkan/vulkan_render_target_cache.cc:1033-1200 Resolve: the copy
// first, then the clears, X render_target_cache.cc:1160-1360). Refuses, changing nothing: an unowned span
// tile; a cross-aspect or depth-format transfer into the source; a D24FS8 depth clear above 1.0 (Vulkan clears
// are [0, 1]; X xenos.cc:150-166 Float20e4To32 is monotonic in the 24-bit value, so that is depth bits >
// 0xF00000); a clear Key outside the whitelist.
ResolvePlan PrepareResolve(Ownership& ownership, const ResolveInfo& info);

// ---- Draw extent: the rows a draw can touch (X draw_extent_estimator.cc) ---------------------------------
//
// X render_target_cache.cc:757-767 binds with height_used = min(GetRenderTargetHeight, EstimateMaxY). The
// result is Bind's `heightUsed`. Without it a clear draw (clip disabled, 8192x8192 scissor) claims every tile
// up to the next surface's base: FM4's 4x fast clear of the 7e3 pass (depth
// moved from tile 720 to 1024) then reached the old depth target's tiles 720-1023 and was refused.

struct ExtentRegisters
{
    uint32_t surfaceInfo = 0;        // RB_SURFACE_INFO 0x2000 (msaa_samples: the top-left rule's rounding)
    uint32_t windowOffset = 0;       // PA_SC_WINDOW_OFFSET 0x2080 (X registers.h:561-569: signed 15-bit y at bit 16)
    uint32_t windowScissorTl = 0;    // PA_SC_WINDOW_SCISSOR_TL 0x2081 (bit 31 window_offset_disable)
    uint32_t windowScissorBr = 0;    // PA_SC_WINDOW_SCISSOR_BR 0x2082 (x bits 0-13, y 16-29)
    uint32_t screenScissorBr = 8192 | 8192u << 16;  // PA_SC_SCREEN_SCISSOR_BR 0x200F (signed 15-bit; D3D9 keeps 8192, X :318-320 comment)
    uint32_t clipCntl = 0;           // PA_CL_CLIP_CNTL 0x2204 (clip_disable bit 16, X registers.h:502)
    uint32_t suScModeCntl = 0;       // PA_SU_SC_MODE_CNTL 0x2205 (vtx_window_offset_enable bit 16)
    uint32_t vteCntl = 0;            // PA_CL_VTE_CNTL 0x2206 (X registers.h:520-536: y scale ena bit 2, y offset ena bit 3, vtx_xy_fmt bit 8)
    uint32_t vtxCntl = 0;            // PA_SU_VTX_CNTL 0x2302 (pix_center bit 0)
    uint32_t vportYScale = 0, vportYOffset = 0;  // PA_CL_VPORT_YSCALE 0x2111, _YOFFSET 0x2112: float bits
};

// X draw_extent_estimator.cc:289-316 with its defaults (execute_unclipped_draw_vs_on_cpu true, :24-38;
// _with_scissor false, :39-49): only a draw with clipping disabled AND a scissor whose right and bottom
// (after the window offset, within the screen scissor) both reach 8192 has its vertices evaluated.
bool NeedsVertexMaxY(const ExtentRegisters& r);

// X draw_extent_estimator.cc:267-366 EstimateMaxY: the scissor's bottom (window offset unless disabled, the
// screen scissor, >= 0); with clipping on, also the viewport's bottom (window offset if enabled, + 0.5 unless
// pix_center, + y offset if enabled, + |y scale| if enabled else 1; truncated); with clipping off, also
// `vertexMaxY` when NeedsVertexMaxY and the caller has it (nullptr: the scissor alone, X's result with the
// estimation off).
uint32_t EstimateMaxY(const ExtentRegisters& r, const uint32_t* vertexMaxY);

// The exported position of one vertex (X draw_extent_estimator.cc:54-73 PositionYExportSink: y and w of
// oPos). X runs the vertex shader on the CPU (ShaderInterpreter, :169-243); we have no interpreter, so the
// caller supplies the positions (PassThroughPositions below for a shader that exports a fetched position).
struct VertexPosition
{
    float y = 0, w = 1;
};

// X draw_extent_estimator.cc:199-264 after the shader: y / w unless vtx_xy_fmt (:212-217), the viewport's y
// scale and offset when enabled (:146-152, :219), the maximum, to 16.8 fixed point (:246, ui/graphics_util.cc
// FloatToD3D11Fixed16p8), + 0.5 unless pix_center (:253-255), + the window y offset when vtx_window_offset_enable
// (:256-258), then the top-left rule: .5 exclusive at 1x, 1. exclusive with MSAA (:259-264). No vertex: 0
// (X :81-83 with no indices; all killed: max_y stays -FLT_MAX, which saturates to 0). Point lists are not
// modelled (X adds the point radius, :221-238): the caller keeps the scissor bound for them.
uint32_t VertexMaxY(const ExtentRegisters& r, const VertexPosition* positions, size_t count);

// For a vertex shader whose oPos is a vertex fetch's xyz with w = 1, unchanged (FM4's fast-clear VS
// b6cb6460159a82b1): the y of each of
// `vertices` vertices, read from `words` (the vertex window as 32-bit loads of guest memory in host order, as
// the renderer copies it; the fetch constant's endian applied as X xenos.h:1070-1086 GpuSwap) at dword
// `offsetDwords + 1` of each `strideDwords`-dword vertex, w = 1. False with a reason when the window is too short
// or the position does not fit in the stride. Recognising such a shader is the caller's (shader translator's) job.
bool PassThroughPositions(const uint32_t* words, size_t wordCount, uint32_t endian, uint32_t strideDwords, uint32_t offsetDwords, uint32_t vertices,
                          std::vector<VertexPosition>& out, std::string& why);

// ---- Helpers shared with the tests ----------------------------------------------------------------------

// X ui/graphics_util.cc:17-60: D3D11 float -> 16.8 fixed point, round to nearest even, saturating.
int32_t FloatToFixed16p8(float f);
// X xenos.h:1070-1092 GpuSwap for a 32-bit word: 0 none, 1 bytes in halves, 2 all bytes, 3 halves.
uint32_t GpuSwap32(uint32_t v, uint32_t endian);

}  // namespace runtime::video::edram
