// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// CPU-only tests of the draw state decode (runtime/video/draw_state.h): hand-built register files,
// no Vulkan, no guest memory, no translator run (the ShaderTranslation inputs are plain structs
// filled by hand). Expected values are computed by hand from Xenia 67f16c4e315f's definitions
// (registers.h, xenos.h, draw_util.cc; "X file:line") in the comment next to each check, never from
// the decoder. Register values marked OBSERVED are FM4's own, from the local capture trace13 (not distributed) (a
// headless run to swap 400; the CP trace logs, per draw, a `draw` line with RB_SURFACE_INFO,
// RB_COLOR_INFO, RB_DEPTH_INFO, RB_COLOR1_INFO, RB_MODECONTROL, PA_SC_WINDOW_SCISSOR_BR,
// VGT_DRAW_INITIATOR, RB_COLOR_MASK, RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL,
// PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL, SQ_PROGRAM_CNTL and a `state` line with the
// viz-query token, PA_SC_VIZ_QUERY, VGT_OUTPUT_PATH_CNTL, SQ_INTERPOLATOR_CNTL, SQ_VS_CONST,
// SQ_PS_CONST, PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL, PA_SU_VTX_CNTL, RB_STENCILREFMASK and
// PA_CL_VPORT_XSCALE..ZOFFSET), cited by the `draw` line: 3 (draw 0, the first of the 48
// zero-surface point draws), 670 (draw 304, a point draw on the 4X target; 9456 such), 662 (draw
// 302, a clear; 1576), 601 (draw 276, a UI quad list; 355), 45817 (draw 19501, a UI rect list;
// 39), 45744 (draw 19472, a triangle strip into a gamma target; 39), 607 (draw 277, a resolve;
// 394). Every targeted draw has viz token 0, PA_SC_VIZ_QUERY 0, VGT_OUTPUT_PATH_CNTL 0, window
// offset and scissor TL 0. Not traced (RB_ALPHA_REF, RB_BLEND_*, the fetch constants, VGT_DMA_*):
// CONSTRUCTED from the Direct3D 9 conventions or ForzaRecomp's observations (milestones/
// M3-runtime.md), as each fixture says.
// "CONTROL" names a negative control: a wrong state that must be refused, with its register named.
//
//   tests/run_draw_state_tests.sh
#include "draw_state.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace runtime::video;
using runtime::shader::ShaderTranslation;

namespace {
int failures = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}
bool Near(float a, float b, float tolerance = 1e-6f) { return std::fabs(a - b) <= tolerance; }

constexpr uint64_t kPhysical = uint64_t(512) << 20;  // the console's 512 MB

uint32_t ReadVector(const void* context, uint32_t index)
{
    const auto& regs = *static_cast<const std::vector<uint32_t>*>(context);
    if (index >= regs.size())
    {
        std::printf("FAIL register %X read past the file\n", index);
        std::exit(2);
    }
    return regs[index];
}

constexpr uint32_t F(float f) { return __builtin_bit_cast(uint32_t, f); }

// A register file with the Direct3D 9 constants every fixture shares.
struct File
{
    std::vector<uint32_t> regs = std::vector<uint32_t>(xr::kRegisterCount, 0);
    uint32_t& operator[](uint32_t i) { return regs[i]; }
    File()
    {
        regs[xr::kPaScScreenScissorBr] = 0x20002000;  // 8192 x 8192: Direct3D 9 never uses the screen scissor (X draw_util.cc:681-682)
        regs[xr::kVgtMaxVtxIndx] = 0xFFFFFF;
        regs[xr::kSqPsConst] = 0x000FF100;  // OBSERVED (trace13 every targeted draw) psconst 000FF100: base 256, size 255 (bits 0-8, 12-20; X registers.h:242-252)
        regs[xr::kSqVsConst] = 0x000FF000;  // OBSERVED vsconst 000FF000: base 0, size 255
        regs[xr::kSqInterpolatorCntl] = 0x00010001;  // OBSERVED interp 00010001 on 21,372 of 21,648 draws: param_shade bit 0, sampling_pattern bit 0 (recorded, not applied)
        regs[xr::kPaSuVtxCntl] = 4;  // OBSERVED vtxcntl 00000004 (points, clears, strips, resolves): pix_center 0 (half pixel), round_mode 2
    }
};

// FM4's point draws (OBSERVED trace10 line 3, draw 0: the first of 48 before any surface is set; 9648
// more at line 61 on, on the 4X target, differ only in the surface registers and blend0 00010001):
// a one-index auto-indexed point list with RB_COLOR_MASK 0 and depth/stencil off, writing nothing.
File PointDraw()
{
    File f;
    f[xr::kRbModeControl] = 4;             // OBSERVED mode 00000004
    f[xr::kRbSurfaceInfo] = 0;             // OBSERVED surface 00000000
    f[xr::kRbColorInfo] = 0;               // OBSERVED color 00000000
    f[xr::kRbDepthInfo] = 0;               // OBSERVED depth 00000000
    f[xr::kPaScWindowScissorBr] = 0x00100010;  // OBSERVED scissor_br 00100010: 16 x 16
    f[xr::kVgtDrawInitiator] = 0x00010081;  // OBSERVED initiator: point list (1), auto index (2 << 6), 1 index (X registers.h:282-296)
    f[xr::kRbColorMask] = 0;               // OBSERVED colormask 00000000
    f[xr::kRbDepthControl] = 0;            // OBSERVED depthctl 00000000
    f[xr::kRbBlendControl0] = 0;           // OBSERVED blend0 00000000 (00010001 from draw 48 on)
    f[xr::kRbColorControl] = 0;            // OBSERVED colorctl 00000000 (87000007 on the 1X surface: alpha-to-mask offsets 3 1 0 2, test off)
    f[xr::kPaClClipCntl] = 0x00010000;     // OBSERVED clip 00010000: clip_disable
    f[xr::kPaSuScModeCntl] = 0x00010000;   // OBSERVED sumode 00010000: vtx_window_offset_enable
    f[xr::kPaClVteCntl] = 0x300;           // OBSERVED vte 00000300: vtx_xy_fmt, vtx_z_fmt (no viewport scale or offset)
    f[xr::kSqProgramCntl] = 0x1000000E;    // OBSERVED program 1000000E: vs_num_reg 14, ps_export_mode 2
    return f;
}

// FM4's clears (OBSERVED trace10 line 55, draw 48; 1590 such: the 640 x 360 4X MSAA 2_10_10_10 target at tile 0,
// D24FS8 at tile 720, scissor 8192 x 8192, a 3-vertex auto-indexed rect list writing every colour component,
// depth ALWAYS with write and stencil ALWAYS / REPLACE, no blending, clipping disabled, the position pre-divided;
// FR: vertex colour and screen-space position, clearing colour and depth to 0; the state is Direct3D's fast clear,
// KB xbox360/gpu-edram-and-ring.md). RB_STENCILREFMASK is not traced: CONSTRUCTED.
File RectClear()
{
    File f;
    f[xr::kRbModeControl] = 4;                 // OBSERVED mode 00000004
    f[xr::kRbSurfaceInfo] = 0x0A020280;        // OBSERVED: pitch 0x280 = 640 (bits 0-13), msaa 2 = 4X (bits 16-17), hiz pitch 0x280 (bits 18-31)
    f[xr::kRbColorInfo] = 0x00020000;          // OBSERVED: base 0, format 2 (k_2_10_10_10), exp bias 0
    f[xr::kRbDepthInfo] = 0x000102D0;          // OBSERVED: base 0x2D0 = 720 tiles, format 1 (D24FS8)
    f[xr::kPaScWindowScissorBr] = 0x20002000;  // OBSERVED: 8192 x 8192
    f[xr::kVgtDrawInitiator] = 0x00030088;     // OBSERVED initiator: rect list (8), auto index (2 << 6 = 0x80), 3 indices
    f[xr::kRbColorMask] = 0xFFFF;              // OBSERVED colormask 0000FFFF: every component of colour 0-3 (only colour 0 is decoded)
    f[xr::kRbDepthControl] = 0x8777;           // OBSERVED depthctl 00008777: stencil on (bit 0), z on (1), z write (2), zfunc 7 (bits 4-6), stencilfunc 7 (8-10), zpass replace 2 (14-16)
    f[xr::kRbStencilRefMask] = 0x00FF0000;     // OBSERVED (trace13 line 663) stencilref 00FF0000: ref 0, compare mask 0, write mask FF (the clear writes stencil 0)
    f[xr::kRbBlendControl0] = 0x00010001;      // OBSERVED blend0 00010001: one / zero / add, colour and alpha: no blending
    f[xr::kRbColorControl] = 0;                // OBSERVED colorctl 00000000
    f[xr::kPaClClipCntl] = 0x00010000;         // OBSERVED clip 00010000: clip_disable (bit 16), dx_clip_space_def clear: screen-space draw (X draw_util.cc:248-271)
    f[xr::kPaSuScModeCntl] = 0x00010000;       // OBSERVED sumode 00010000: vtx_window_offset_enable only
    f[xr::kPaClVteCntl] = 0x300;               // OBSERVED vte 00000300: no viewport scale or offset; vtx_xy_fmt and vtx_z_fmt (xy, z divided by W; w the reciprocal)
    f[xr::kSqProgramCntl] = 0x10010001;        // OBSERVED program 10010001: vs_num_reg 1, vs_resource, ps_export_mode 2
    f[xr::kFetchConstants + 0] = 0x1B400003;   // CONSTRUCTED slot 0: type 3 (vertex), address 0x1B400000
    f[xr::kFetchConstants + 1] = (12u << 2) | 2;  // 12 words, endian 2 (8in32)
    f[xr::kAluConstants + 0] = F(1.0f);        // VS c0.x
    f[xr::kAluConstants + 1024] = F(2.0f);     // PS c0.x (ALU 256)
    f[xr::kBoolConstants + 0] = 0x1;
    f[xr::kLoopConstants + 0] = 0x40;          // count 64
    return f;
}

// FM4's UI quad lists (OBSERVED trace10 line 267, draw 226; 357 such, plus 38 rect lists at line 24971 that
// differ only in the initiator 00180088): the 1280-pitch 1X target, colour format 10 (k_2_10_10_10_AS_10_10_10_10)
// at tile 0 with D24FS8 at 720, scissor 1280 x 720, a 32-vertex auto-indexed quad list, depth test off, alpha
// blending src alpha / inv src alpha, the alpha test enabled with function ALWAYS, back faces culled with
// clockwise fronts, MSAA and window offset enabled, every viewport scale and offset enabled with vtx_w0_fmt.
// trace13 (line 602, the `state` line of the same draw): the viewport PA_CL_VPORT x scale 640, x offset 640, y scale
// -360, y offset 360, **z scale -1, z offset 1** (a reversed depth range: Direct3D 9 MinZ 1, MaxZ 0), PA_SU_VTX_CNTL 5
// (pix_center 1: no half-pixel offset; round_mode 2), RB_STENCILREFMASK 00FFFF00, SQ_INTERPOLATOR_CNTL 00010001.
// Not traced, CONSTRUCTED: the blend constants, the fetch constants (a linear k_8_8_8_8 and a tiled gamma DXT1,
// formats FR saw on the welcome screen).
File UiQuad()
{
    File f;
    f[xr::kRbModeControl] = 4;                 // OBSERVED
    f[xr::kRbSurfaceInfo] = 0x14000500;        // OBSERVED: pitch 1280 (0x500), msaa 0, hiz pitch 1280 << 18
    f[xr::kRbColorInfo] = 0x000A0000;          // OBSERVED: base 0, format 10 (k_2_10_10_10_AS_10_10_10_10, storage format 2)
    f[xr::kRbDepthInfo] = 0x000102D0;          // OBSERVED: base 720, format 1 (D24FS8)
    f[xr::kPaScWindowScissorBr] = 0x02D00500;  // OBSERVED: 1280 x 720
    f[xr::kVgtDrawInitiator] = 0x0020008D;     // OBSERVED: quad list (13), auto index (2 << 6), 32 indices
    f[xr::kRbColorMask] = 0xF;                 // OBSERVED colormask 0000000F
    f[xr::kRbDepthControl] = 0x00700730;       // OBSERVED depthctl: stencil off, z off, zfunc 3, stencilfunc 7, stencilfunc_bf 7 (bits 20-22)
    f[xr::kRbBlendControl0] = 0x07060706;      // OBSERVED blend0: colour src alpha (6), add, inv src alpha (7); alpha the same
    f[xr::kRbBlendRed] = F(0.25f);             // CONSTRUCTED
    f[xr::kRbBlendRed + 3] = F(0.75f);         // CONSTRUCTED
    f[xr::kRbColorControl] = (0x87u << 24) | 0xF;  // OBSERVED colorctl 8700000F: alpha func 7 (always) with alpha_test_enable (bit 3); alpha-to-mask offsets 3 1 0 2 (bits 24-31), its enable (bit 4) clear (spelt as an expression for tests/test_no_title_addresses.py)
    f[xr::kRbAlphaRef] = F(0.5f);              // CONSTRUCTED
    f[xr::kPaSuScModeCntl] = 0x00018006;       // OBSERVED sumode: cull back (bit 1), front is CW (bit 2), msaa_enable (15), vtx_window_offset_enable (16)
    f[xr::kPaClClipCntl] = 0x00080000;         // OBSERVED clip: dx_clip_space_def, clipping on
    f[xr::kPaClVteCntl] = 0x43F;               // OBSERVED vte: every scale and offset enabled (bits 0-5), vtx_w0_fmt (bit 10)
    f[xr::kPaClVportXScale + 0] = F(640.0f);   // OBSERVED vport 44200000 44200000 C3B40000 43B40000 BF800000 3F800000 (trace13 line 602)
    f[xr::kPaClVportXScale + 1] = F(640.0f);
    f[xr::kPaClVportXScale + 2] = F(-360.0f);
    f[xr::kPaClVportXScale + 3] = F(360.0f);
    f[xr::kPaClVportXScale + 4] = F(-1.0f);
    f[xr::kPaClVportXScale + 5] = F(1.0f);
    f[xr::kPaSuVtxCntl] = 5;                   // OBSERVED vtxcntl 00000005: pix_center 1 (no half pixel), round_mode 2
    f[xr::kRbStencilRefMask] = 0x00FFFF00;     // OBSERVED stencilref 00FFFF00: ref 0, compare mask FF, write mask FF (stencil is off)
    f[xr::kSqProgramCntl] = 0x10010001;        // OBSERVED program: vs_num_reg 1, ps_num_reg 0, vs_resource (bit 16), ps_export_mode 2 (bits 27-30)
    f[xr::kFetchConstants + 0] = 0x1B480003;   // CONSTRUCTED vertex slot 0 (words 0-1): type 3, address 0x1B480000
    f[xr::kFetchConstants + 1] = (40u << 2);   // 40 words, endian 0
    // Texture slots 1 and 2 (words 6t..6t+5, clear of vertex slot 0's words 2s, 2s+1; X xenos.h:1206-1307 layout).
    // Slot 1: linear k_8_8_8_8, 256 x 128, unsigned, clamp to edge on x and y, swizzle XYZW, linear mag/min, mip filter
    // base map, endian 8in32, base 0x1B600000.
    f[xr::kFetchConstants + 6 * 1 + 0] = 0x02004802;  // type 2 | clamp_x 2 << 10 | clamp_y 2 << 13 | pitch (256 >> 5 = 8) << 22
    f[xr::kFetchConstants + 6 * 1 + 1] = 0x1B600086;  // format 6 | endian 2 << 6 | base 0x1B600000
    f[xr::kFetchConstants + 6 * 1 + 2] = 0x000FE0FF;  // width - 1 = 255 | (height - 1 = 127) << 13
    f[xr::kFetchConstants + 6 * 1 + 3] = 0x01280D10;  // swizzle XYZW 0x688 << 1 | mag 1 << 19 | min 1 << 21 | mip 2 << 23
    f[xr::kFetchConstants + 6 * 1 + 4] = 0;
    f[xr::kFetchConstants + 6 * 1 + 5] = 0x200;       // dimension 1 (2D) << 9
    // Texture slot 2: tiled DXT1 512 x 512, x y z gamma (sign 3), repeat, swizzle XYZ1, exp adjust -1, LOD bias -16/32,
    // base 0x1B700000.
    f[xr::kFetchConstants + 6 * 2 + 0] = (1u << 31) | 0x040000FE;  // tiled bit 31 | pitch (512 >> 5 = 16) << 22 | sign x,y,z 3 (bits 2-7) | type 2
    f[xr::kFetchConstants + 6 * 2 + 1] = 0x1B700012;  // format 18 | base 0x1B700000
    f[xr::kFetchConstants + 6 * 2 + 2] = 0x003FE1FF;  // 511 | 511 << 13
    f[xr::kFetchConstants + 6 * 2 + 3] = 0x012FF510;  // swizzle XYZ1 0xA88 << 1 | exp adjust -1 (0x3F << 13) | mag 1 | min 1 | mip 2
    f[xr::kFetchConstants + 6 * 2 + 4] = 0x003F0000;  // lod bias -16 (10-bit 0x3F0) << 12
    f[xr::kFetchConstants + 6 * 2 + 5] = 0x200;
    return f;
}

// The same state with an index buffer (CONSTRUCTED: no draw in trace10's 400 swaps uses one; every initiator
// has source select 2): a 6-index 16-bit triangle list, swap 8in16.
File Textured()
{
    File f = UiQuad();
    f[xr::kVgtDrawInitiator] = 0x00060004;  // triangle list (4), DMA (0 << 6), 16-bit (bit 11 clear), 6 indices
    f[xr::kVgtDmaBase] = 0x1B500000;
    f[xr::kVgtDmaSize] = (1u << 30) | 6;    // 6 words, swap 8in16 (bits 30-31)
    return f;
}

ShaderTranslation Vs(std::vector<uint32_t> slots = {}, std::vector<uint32_t> strides = {}, std::vector<uint32_t> ends = {}, uint32_t interpolators = 1)
{
    ShaderTranslation t;
    t.interpolatorCount = interpolators;
    t.vertexFetchSlots = slots;
    t.vertexFetchStrides = strides;
    t.vertexFetchEnds = ends;
    return t;
}
ShaderTranslation Ps(uint32_t interpolators = 1)
{
    ShaderTranslation t;
    t.interpolatorCount = interpolators;
    return t;
}
void Sample(ShaderTranslation& t, uint32_t slot, uint32_t dimension, ShaderTranslation::Filters filters = {})
{
    t.textureSlots.push_back(slot);
    t.textureDimensions.push_back(dimension);
    t.textureFilters.push_back(filters);
}

DrawState Decode(File& f, const ShaderTranslation& vs, const ShaderTranslation& ps, uint32_t opcode = 0x36, uint32_t vizToken = 0)
{
    DrawInputs in;
    in.read = ReadVector;
    in.context = &f.regs;
    in.opcode = opcode;
    in.vizQueryToken = vizToken;
    in.physicalBytes = kPhysical;
    in.vs = &vs;
    in.ps = &ps;
    return DecodeDrawState(in);
}

bool Refused(const DrawState& d, const char* fragment, const char* name)
{
    const bool ok = d.kind == DrawKind::Refused && d.error.find(fragment) != std::string::npos;
    if (!ok)
        std::printf("  (%s: kind %u, error \"%s\", wanted \"%s\")\n", name, unsigned(d.kind), d.error.c_str(), fragment);
    return ok;
}
#define CONTROL(state, fragment) Expect(Refused(state, fragment, "CONTROL " fragment), "CONTROL " fragment)

void TestPointDraws()
{
    File f = PointDraw();
    const ShaderTranslation vs = Vs(), ps = Ps(0);
    DrawState d = Decode(f, vs, ps);
    Expect(d.kind == DrawKind::NoWrite && d.error.empty(), "FM4 point draw with surface 0, mask 0, depth off: NoWrite, not refused");
    Expect(d.hostPrimitive == HostPrimitive::PointNoWrite && d.primitiveType == 1 && d.indices.count == 1 && !d.indices.indexed,
           "point draw: primitive 1, auto index, 1 index");
    Expect(d.colorWriteMask == 0 && !d.depthStencil.zEnable && !d.depthStencil.stencilEnable, "point draw: no writes decoded");
    // CONTROL: the same point draw writing colour is a point-sprite draw, refused by name.
    f[xr::kRbColorMask] = 0xF;
    f[xr::kRbSurfaceInfo] = 0x0A020280;
    CONTROL(Decode(f, vs, ps), "point list");
    // CONTROL: a writing draw on the pitch-0 surface.
    f[xr::kVgtDrawInitiator] = 0x00030084;  // triangle list, auto, 3
    f[xr::kRbSurfaceInfo] = 0;
    CONTROL(Decode(f, vs, ps), "no pitch");
    // Mode 5 (depth only) with colour mask F but depth off is still NoWrite; with depth write it is DepthOnly and the mask is 0.
    File g = PointDraw();
    g[xr::kRbModeControl] = 5;
    g[xr::kRbColorMask] = 0xF;
    g[xr::kVgtDrawInitiator] = 0x00030084;
    Expect(Decode(g, vs, ps).kind == DrawKind::NoWrite, "mode 5 with depth off: NoWrite even with a colour mask");
    g[xr::kRbDepthControl] = 0x76;  // z on, write, always
    g[xr::kRbSurfaceInfo] = 0x0A020280;
    g[xr::kRbDepthInfo] = 0x000102D0;
    g[xr::kPaScWindowScissorBr] = 0x20002000;
    d = Decode(g, vs, ps);
    Expect(d.kind == DrawKind::DepthOnly && d.colorWriteMask == 0 && d.target.depthWritten && !d.target.colorWritten, "mode 5 with a depth write: DepthOnly, colour mask forced 0");
    // Mode 6 is a resolve, handed off; modes 0-3 and 7 are refused.
    g[xr::kRbModeControl] = 6;
    Expect(Decode(g, vs, ps).kind == DrawKind::Resolve, "mode 6: Resolve");
    g[xr::kRbModeControl] = 0;
    CONTROL(Decode(g, vs, ps), "RB_MODECONTROL mode 0");
}

void TestRectClear()
{
    File f = RectClear();
    const ShaderTranslation vs = Vs({0}, {4}, {4}), ps = Ps(1);
    DrawState d = Decode(f, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.error.empty(), "FM4 rect clear: Draw");
    if (d.kind != DrawKind::Draw)
        std::printf("  (%s)\n", d.error.c_str());
    Expect(d.hostPrimitive == HostPrimitive::RectList && d.indices.count == 3 && !d.indices.indexed, "rect clear: rect list, 3 auto indices");
    // RB_SURFACE_INFO 0A020280: pitch 640, msaa 2 (4X), hiz 640; RB_COLOR_INFO 00020000: base 0, format 2; RB_DEPTH_INFO 000102D0: base 720, D24FS8.
    Expect(d.target.pitch == 640 && d.target.msaa == 2 && d.target.samples == 4 && d.target.hizPitch == 640, "surface: pitch 640, 4X MSAA, hiz 640");
    Expect(d.target.colorBase == 0 && d.target.colorFormat == 2 && d.target.colorExpBias == 0, "colour: base 0, k_2_10_10_10, bias 0");
    Expect(d.target.depthBase == 720 && d.target.depthFormat == 1, "depth: base 720, D24FS8");
    // Tiles (X xenos.h:467-480): 4X doubles the pitch to 1280 samples, 1280 / 80 = 16 tiles a row; a tile holds 16 / 2 = 8
    // rows at 4X; the colour surface has the 720 tiles up to the depth base: 720 / 16 = 45 tile rows = 360 pixel rows.
    Expect(d.target.pitchTiles == 16 && d.target.edramRows == 360, "640 x 360 at 4X: 16 tiles a row, 360 rows to the depth base");
    Expect(d.target.targetHeight == 360 && d.target.colorWritten && d.target.depthWritten, "target height 360 (the scissor is taller)");
    // Scissor (X draw_util.cc:684-713): 0..8192 clamped to the pitch in x: 0, 0, 640 x 8192.
    Expect(d.scissor.x == 0 && d.scissor.y == 0 && d.scissor.width == 640 && d.scissor.height == 8192, "scissor 0,0 640x8192 (x clamped to the pitch)");
    Expect(d.pixelInvocations == uint64_t(640) * 8192, "pixel invocations 640 * 8192");
    // Depth/stencil 0x8777 with RB_STENCILREFMASK 00FFFF01.
    const DepthStencil& s = d.depthStencil;
    Expect(s.stencilEnable && s.zEnable && s.zWriteEnable && s.zFunc == 7 && !s.backfaceEnable, "depth: on, write, always; stencil on");
    // RB_STENCILREFMASK 00FF0000 (X registers.h:767-777): ref bits 0-7 = 0, compare mask bits 8-15 = 0, write mask bits 16-23 = FF.
    Expect(s.front.func == 7 && s.front.fail == 0 && s.front.zPass == 2 && s.front.zFail == 0 && s.front.ref == 0 && s.front.compareMask == 0 &&
               s.front.writeMask == 0xFF,
           "stencil front: always, keep, replace, keep, ref 0, compare mask 0, write mask FF");
    Expect(s.back.func == 7 && s.back.zPass == 2 && s.back.ref == 0 && s.back.writeMask == 0xFF, "stencil back = front when backface_enable is 0");
    Expect(d.program.paramShade == 1 && d.program.samplingPattern == 1, "interp 00010001 recorded: param_shade 1, sampling_pattern 1");
    Expect(!d.blend.enabled && d.blend.srcColor == 1 && d.blend.dstColor == 0 && d.blend.colorOp == 0, "blend 00010001: disabled");
    Expect(!d.alphaTest.enabled, "alpha test off");
    Expect(d.colorWriteMask == 0xF, "colour mask F");
    // Viewport: VTE 0 -> scale 1, offset 0 (X draw_util.cc:306-319); pix_center 0 -> + 0.5 (:329-332); clip_disable and dx clip space.
    Expect(Near(d.viewport.scale[0], 1) && Near(d.viewport.scale[1], 1) && Near(d.viewport.scale[2], 1) && Near(d.viewport.offset[0], 0) &&
               Near(d.viewport.offsetAddXy[0], 0.5f) && Near(d.viewport.offsetAddXy[1], 0.5f) && d.viewport.clipDisabled && !d.viewport.dxClipSpace,
           "viewport: identity, half pixel, clipping disabled, no dx clip space (clip 00010000)");
    // vte 00000300: xe_flags 1 | 2 (xy and z divided by W; w the reciprocal), the shader's xe_fixup undoes it (shader_glsl.h).
    Expect(d.systemFlags == 3 && d.viewport.xyDividedByW && d.viewport.zDividedByW && !d.viewport.wNotReciprocal, "vte 00000300: xe_flags 3");
    Expect(d.program.vsNumReg == 1 && d.program.vsResource && d.program.psExportMode == 2 && d.program.vsExportCount == 0, "program 10010001 decoded");
    // Vertex slot 0: 1B400003 / 00000032 -> address 0x1B400000, 12 words, endian 2, stride 4, end 4; a draw reaching index 2 needs 2 * 4 + 4 = 12.
    Expect(d.vertexSlots.size() == 1 && d.vertexSlots[0].valid && d.vertexSlots[0].physical == 0x1B400000 && d.vertexSlots[0].sizeWords == 12 &&
               d.vertexSlots[0].endian == 2 && d.vertexSlots[0].stride == 4 && d.vertexSlots[0].end == 4,
           "vertex slot 0 decoded");
    Expect(VertexWindowDwords(d.vertexSlots[0], 2) == 12 && VertexWindowDwords(d.vertexSlots[0], 1) == 8 && VertexWindowDwords(d.vertexSlots[0], 5) == 12,
           "vertex window: high 2 -> 12, high 1 -> 8, high 5 capped at the size");
    Expect(d.aluConstants.size() == 2048 && d.aluConstants[0] == F(1.0f) && d.aluConstants[1024] == F(2.0f) && d.boolConstants[0] == 1 &&
               d.loopConstants[0] == 0x40,
           "ALU, bool and loop constants copied in register order");
    Expect(d.program.vsConstBase == 0 && d.program.psConstBase == 256 && d.program.psConstSize == 255, "SQ_VS_CONST / SQ_PS_CONST decoded");
    Expect(d.textureSlots.empty() && d.vsLoopRegisters.empty(), "no textures, no loops");

    // The host fold of this viewport for a 1024 x 512 target (exact in binary): x scale 2 / 1024 = 2^-9,
    // x offset 0.5 * 2 / 1024 - 1 = 2^-10 - 1; y scale 2^-8, y offset 2^-9 - 1; z through (clip disabled): scale 1, offset 0, range 0..1.
    const HostViewport h = FoldViewport(d.viewport, 1024, 512);
    Expect(h.scale[0] == 0.001953125f && h.offset[0] == -0.9990234375f && h.scale[1] == 0.00390625f && h.offset[1] == -0.998046875f,
           "fold 1024x512: exact power-of-two scales and offsets");
    Expect(h.scale[2] == 1 && h.offset[2] == 0 && h.zMin == 0 && h.zMax == 1 && h.scale[3] == 1 && h.offset[3] == 0, "fold: z untouched, 0..1");

    // CONTROLS on the clear fixture.
    File g = RectClear();
    g[xr::kRbSurfaceInfo] = 0x0A030280;  // msaa 3
    CONTROL(Decode(g, vs, ps), "msaa_samples 3");
    g = RectClear(), g[xr::kRbColorInfo] = 0x00050000;  // format 5 (16_16_16_16)
    CONTROL(Decode(g, vs, ps), "colour format 5");
    g = RectClear(), g[xr::kRbColorInfo] = 0x000A0000;  // format 10 is storage format 2
    d = Decode(g, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.target.colorFormat == 2 && d.target.colorFormatRaw == 10, "colour format 10 decodes as storage format 2");
    g = RectClear(), g[xr::kRbColorInfo] = 0x00120000;  // exp bias 1 (bits 20-25)
    CONTROL(Decode(g, vs, ps), "exponent bias");
    g = RectClear(), g[xr::kRbColorInfo] = 0x00020800;  // base bit 11
    CONTROL(Decode(g, vs, ps), "colour base bit 11");
    g = RectClear(), g[xr::kRbDepthInfo] = 0x00010000;  // depth at tile 0 = the colour base, both written
    CONTROL(Decode(g, vs, ps), "same EDRAM base");
    g = RectClear(), g[xr::kRbBlendControl0] = 0x00010002;  // factor 2 is undefined
    CONTROL(Decode(g, vs, ps), "blend factor 2");
    g = RectClear(), g[xr::kRbBlendControl0] = 0x000100A1;  // op 5 (bits 5-7)
    CONTROL(Decode(g, vs, ps), "blend operation 5");
    g = RectClear(), g[xr::kRbColorControl] = 0x10;
    CONTROL(Decode(g, vs, ps), "alpha to mask");
    g = RectClear(), g[xr::kPaSuScModeCntl] = 0x8;  // poly_mode 1
    CONTROL(Decode(g, vs, ps), "polygon mode");
    g = RectClear(), g[xr::kPaSuScModeCntl] = 0x800;  // poly_offset_front_enable
    CONTROL(Decode(g, vs, ps), "polygon offset");
    g = RectClear(), g[xr::kPaSuScModeCntl] = 0x100000;  // persp_corr_dis
    CONTROL(Decode(g, vs, ps), "perspective correction");
    g = RectClear(), g[xr::kPaClClipCntl] = 0x00090001;  // ucp_ena_0
    CONTROL(Decode(g, vs, ps), "user clip planes");
    // The position format bits are decoded to xe_flags, never refused (observed 0x300, 0x43F, 0x400; the bit order
    // follows PA_CL_VTE_CNTL bits 8, 9, 10 as kFlagXyDividedByW 1, kFlagZDividedByW 2, kFlagWNotReciprocal 4).
    g = RectClear(), g[xr::kPaClVteCntl] = 0x43F;
    d = Decode(g, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.systemFlags == 4 && d.viewport.scale[0] == 0 && d.viewport.scale[2] == 0 && !d.viewport.xyDividedByW,
           "vte 0000043F: xe_flags 4; the enabled scales read their (here zero) registers");
    g = RectClear(), g[xr::kPaClVteCntl] = 0x400;
    Expect(Decode(g, vs, ps).systemFlags == 4, "vte 00000400: xe_flags 4");
    g = RectClear(), g[xr::kPaClVteCntl] = 0x3F;
    Expect(Decode(g, vs, ps).systemFlags == 0, "vte 0000003F: xe_flags 0");
    g = RectClear(), g[xr::kPaClVteCntl] = 0x200;
    Expect(Decode(g, vs, ps).systemFlags == 2, "vte 00000200: xe_flags 2 (z only)");
    g = RectClear(), g[xr::kSqProgramCntl] = 1u << 18;  // param_gen
    CONTROL(Decode(g, vs, ps), "param_gen");
    g = RectClear(), g[xr::kSqProgramCntl] = 4u << 24;  // vs_export_mode 4 (position 2 vectors, kill)
    CONTROL(Decode(g, vs, ps), "vertex export mode 4");
    g = RectClear(), g[xr::kSqProgramCntl] = 1u << 31;  // gen_index_vtx
    CONTROL(Decode(g, vs, ps), "index generation");
    // SQ_INTERPOLATOR_CNTL is recorded, never refused (Xenia does not read it; FM4 sets 00010001 on every UI draw).
    g = RectClear(), g[xr::kSqInterpolatorCntl] = 0x1;  // flat interpolator 0, which the PS reads
    d = Decode(g, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.program.paramShade == 1 && d.program.samplingPattern == 0, "flat shading of a read interpolator is accepted and recorded");
    g = RectClear(), g[xr::kSqInterpolatorCntl] = 0xFFFF0002;  // flat interpolator 1 (not read), every sampling bit
    d = Decode(g, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.program.paramShade == 2 && d.program.samplingPattern == 0xFFFF, "SQ_INTERPOLATOR_CNTL FFFF0002 recorded");
    g = RectClear(), g[xr::kSqVsConst] = 1;
    CONTROL(Decode(g, vs, ps), "SQ_VS_CONST");
    g = RectClear();
    CONTROL(Decode(g, Vs({0}, {4}, {4}, 1), Ps(2)), "the pixel shader reads 2 interpolators");
    g = RectClear(), g[xr::kPaScVizQuery] = 1;
    CONTROL(Decode(g, vs, ps), "PA_SC_VIZ_QUERY");
    g = RectClear(), g[xr::kVgtOutputPathCntl] = 1;
    CONTROL(Decode(g, vs, ps), "tessellation");
    g = RectClear();
    CONTROL(Decode(g, vs, ps, 0x22, 0x101), "viz-query token");
    g = RectClear(), g[xr::kVgtDrawInitiator] = 0x00000088;  // 0 indices
    CONTROL(Decode(g, vs, ps), "0 indices");
    g = RectClear(), g[xr::kVgtDrawInitiator] = 0x00030085;  // triangle fan
    CONTROL(Decode(g, vs, ps), "triangle fan");
    g = RectClear(), g[xr::kVgtDrawInitiator] = 0x00030090;  // type 0x10
    CONTROL(Decode(g, vs, ps), "primitive type 16");
    g = RectClear(), g[xr::kVgtDrawInitiator] = 0x00030188;  // major mode 1
    CONTROL(Decode(g, vs, ps), "major mode 1");
    g = RectClear(), g[xr::kVgtDrawInitiator] = 0x00030048;  // immediate indices (source 1)
    CONTROL(Decode(g, vs, ps), "immediate indices");
    g = RectClear(), g[xr::kVgtDrawInitiator] = 0x00030086, g[xr::kPaSuScModeCntl] = 1u << 21;  // strip with restart
    CONTROL(Decode(g, vs, ps), "primitive restart");
    g = RectClear(), g[xr::kVgtDrawInitiator] = 0x00030084, g[xr::kPaSuScModeCntl] = 1u << 21;  // list with the bit: ignored (X primitive_processor.cc:619-628)
    Expect(Decode(g, vs, ps).kind == DrawKind::Draw, "multi_prim_ib_ena with a triangle list is ignored");
    // Vertex fetch controls.
    g = RectClear(), g[xr::kFetchConstants + 0] = 0x00000003;  // type 3 at address 0
    CONTROL(Decode(g, vs, ps), "physical address 0");
    g = RectClear(), g[xr::kFetchConstants + 0] = 0x1FFFFFC3;  // 0x1FFFFFC0 + 48 bytes = 0x1FFFFFF0 fits; 0x1FFFFFF3 + 48 does not
    Expect(Decode(g, vs, ps).kind == DrawKind::Draw, "vertex data ending at the last 16 bytes of physical memory fits");
    g[xr::kFetchConstants + 0] = 0x1FFFFFF3;
    CONTROL(Decode(g, vs, ps), "past physical memory");
    g = RectClear(), g[xr::kFetchConstants + 0] = 0x00000001, g[xr::kFetchConstants + 1] = 0;  // FM4's kInvalidVertex slot
    d = Decode(g, vs, ps);
    Expect(d.kind == DrawKind::Draw && !d.vertexSlots[0].valid && VertexWindowDwords(d.vertexSlots[0], 100) == 0,
           "an invalid-vertex constant (00000001 00000000) is an empty window, not a refusal");
}

void TestScissor()
{
    // Window scissor (16, 16)-(640, 360), window offset (-8, +4) (signed 15-bit: -8 = 0x7FF8), screen 0..8192, pitch 640:
    // tl (8, 20), br (632, 364) -> 8, 20, 624 x 344 (X draw_util.cc:676-713).
    File f = RectClear();
    const ShaderTranslation vs = Vs({0}, {4}, {4}), ps = Ps(1);
    f[xr::kPaScWindowScissorTl] = 0x00100010;
    f[xr::kPaScWindowScissorBr] = 0x01680280;
    f[xr::kPaScWindowOffset] = 0x00047FF8;
    DrawState d = Decode(f, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.scissor.x == 8 && d.scissor.y == 20 && d.scissor.width == 624 && d.scissor.height == 344,
           "scissor moved by the window offset (-8, +4)");
    Expect(d.target.targetHeight == 360, "target height min(360 rows, bottom 364) = 360");
    f[xr::kPaScWindowScissorTl] = 0x00100010 | (1u << 31);  // window_offset_disable
    d = Decode(f, vs, ps);
    Expect(d.scissor.x == 16 && d.scissor.y == 16 && d.scissor.width == 624 && d.scissor.height == 344, "window_offset_disable: the offset is not applied");
    // CONTROL: bit 14 of a 14-bit field is not part of the coordinate (X registers.h:575: tl_x is 14 bits).
    f[xr::kPaScWindowScissorTl] = 0x4000 | (1u << 31);
    d = Decode(f, vs, ps);
    Expect(d.scissor.x == 0 && d.scissor.width == 640, "CONTROL scissor TL bit 14 does not leak into x");
    // The screen scissor intersects: screen BR (100, 50) -> 16, 16, 84 x 34.
    f[xr::kPaScWindowScissorTl] = 0x00100010 | (1u << 31);
    f[xr::kPaScScreenScissorBr] = 0x00320064;
    d = Decode(f, vs, ps);
    Expect(d.scissor.x == 16 && d.scissor.y == 16 && d.scissor.width == 84 && d.scissor.height == 34, "screen scissor intersected");
    // BR above TL collapses to an empty rectangle at TL: TL (100, 100), BR (50, 50), screen 8192 -> 100, 100, 0 x 0.
    f[xr::kPaScScreenScissorBr] = 0x20002000;
    f[xr::kPaScWindowScissorTl] = 0x00640064 | (1u << 31);
    f[xr::kPaScWindowScissorBr] = 0x00320032;
    d = Decode(f, vs, ps);
    Expect(d.scissor.x == 100 && d.scissor.y == 100 && d.scissor.width == 0 && d.scissor.height == 0 && d.pixelInvocations == 0, "inverted scissor collapses to empty");
    Expect(d.kind == DrawKind::Draw && d.target.targetHeight == 100, "an empty scissor is a defined draw covering nothing: target height min(360, 100)");
    // CONTROL: a colour surface at tile 2047 with a 16-tile pitch has no whole tile row (1 / 16 = 0 rows).
    f[xr::kRbColorInfo] = 0x000207FF;
    CONTROL(Decode(f, vs, ps), "no rows");
}

void TestTextured()
{
    File f = Textured();
    ShaderTranslation vs = Vs({0}, {5}, {5}, 2), ps = Ps(2);
    Sample(vs, 1, 1);                              // the vertex shader samples slot 1 too (filters from the constant)
    Sample(ps, 1, 1);
    Sample(ps, 2, 1, {0, 3, 3, 3, 3});             // slot 2 with a point mag filter from the instruction
    DrawState d = Decode(f, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.error.empty(), "textured triangle list: Draw");
    if (d.kind != DrawKind::Draw)
        std::printf("  (%s)\n", d.error.c_str());
    Expect(d.hostPrimitive == HostPrimitive::TriangleList && d.indices.indexed && d.indices.count == 6 && !d.indices.is32Bit &&
               d.indices.physical == 0x1B500000 && d.indices.bytes == 12 && d.indices.endian == 1,
           "indices: 6 x 16-bit 8in16 at 1B500000, 12 bytes");
    Expect(d.indexOffset == 0 && d.minIndex == 0 && d.maxIndex == 0xFFFFFF, "index offset 0, clamp 0..FFFFFF");
    Expect(!d.cullFront && d.cullBack && d.frontClockwise && d.msaaEnable, "sumode 00018006: cull back faces, front is clockwise, msaa_enable");
    // depthctl 00700730: bits 1 and 0 clear; zfunc bits 4-6 = 3; stencilfunc bits 8-10 = 7; stencilfunc_bf bits 20-22 = 7.
    Expect(!d.depthStencil.zEnable && !d.depthStencil.zWriteEnable && d.depthStencil.zFunc == 3 && !d.depthStencil.stencilEnable &&
               d.depthStencil.front.func == 7,
           "depthctl 00700730: depth and stencil off (zfunc 3, stencil func 7 decoded)");
    // blend0 07060706: src colour 6, op 0, dst colour 7 (bits 8-12); src alpha 6 (bits 16-20), op 0, dst alpha 7 (bits 24-28).
    Expect(d.blend.enabled && d.blend.srcColor == 6 && d.blend.dstColor == 7 && d.blend.colorOp == 0 && d.blend.srcAlpha == 6 && d.blend.dstAlpha == 7 &&
               d.blend.alphaOp == 0 && d.blend.constant[0] == 0.25f && d.blend.constant[3] == 0.75f,
           "blend0 07060706: src alpha / inv src alpha on colour and alpha, constants read");
    // colorctl 8700000F: alpha_test_enable with function 7 (always) is no test.
    Expect(!d.alphaTest.enabled && d.alphaTest.func == 7, "colorctl 8700000F: alpha test enabled with ALWAYS is off");
    // color 000A0000: format 10 is k_2_10_10_10_AS_10_10_10_10, storage format 2 (X xenos.h:394-404).
    Expect(d.target.pitch == 1280 && d.target.msaa == 0 && d.target.colorFormat == 2 && d.target.colorFormatRaw == 10 && d.target.depthFormat == 1 &&
               d.target.depthBase == 720,
           "target: 1280 pitch 1X, 2_10_10_10 (as 10_10_10_10) at 0, D24FS8 at 720");
    Expect(d.systemFlags == 4 && !d.viewport.xyDividedByW && !d.viewport.zDividedByW && d.viewport.wNotReciprocal, "vte 0000043F: xe_flags 4 (W not reciprocal)");
    // CONSTRUCTED alpha test: function 4 (greater) with RB_ALPHA_REF 0.5.
    {
        File a = Textured();
        a[xr::kRbColorControl] = (0x87u << 24) | 0xC;
        const DrawState e = Decode(a, vs, ps);
        Expect(e.kind == DrawKind::Draw && e.alphaTest.enabled && e.alphaTest.func == 4 && e.alphaTest.ref == 0.5f, "alpha test greater 0.5");
    }
    // 1X: 1280 / 80 = 16 tiles a row, 16 rows a tile; 720 tiles -> 45 tile rows = 720 rows; depth not written -> the colour extent alone.
    Expect(d.target.pitchTiles == 16 && d.target.edramRows == 720 && d.target.targetHeight == 720 && d.target.colorWritten && !d.target.depthWritten,
           "1280 x 720 at 1X: 720 rows");
    Expect(d.scissor.x == 0 && d.scissor.y == 0 && d.scissor.width == 1280 && d.scissor.height == 720, "scissor 1280 x 720");
    // Viewport: all enables set, D3D9 values; + 0.5; clipping with dx clip space.
    // vport (observed): x 640 / 640, y -360 / 360, z scale -1 / offset 1; vtxcntl 5: pix_center 1, so no half pixel (X draw_util.cc:329-332).
    Expect(d.viewport.scale[0] == 640 && d.viewport.offset[0] == 640 && d.viewport.scale[1] == -360 && d.viewport.offset[1] == 360 &&
               d.viewport.scale[2] == -1 && d.viewport.offset[2] == 1 && d.viewport.offsetAddXy[0] == 0 && d.viewport.offsetAddXy[1] == 0 &&
               !d.viewport.clipDisabled && d.viewport.dxClipSpace,
           "viewport 1280 x 720 D3D9 values, reversed z, pixel centres");
    // Fold for the 1280 x 720 target: x scale 640 * 2 / 1280 = 1, offset 640 * 2 / 1280 - 1 = 0; y scale -1, offset 360 * 2 / 720 - 1 = 0;
    // z: dx clip space -> near = offset = 1, far = offset + scale = 0 (X draw_util.cc:437-441, :508-509): a reversed range kept as is
    // (Vulkan permits minDepth > maxDepth; Xenia swaps it for Direct3D 12 only, :513-517), ndc z untouched.
    const HostViewport h = FoldViewport(d.viewport, 1280, 720);
    Expect(h.scale[0] == 1 && h.offset[0] == 0 && h.scale[1] == -1 && h.offset[1] == 0, "fold 1280x720: unit scales, no half-pixel offset");
    Expect(h.scale[2] == 1 && h.offset[2] == 0 && h.zMin == 1 && h.zMax == 0, "fold: dx clip space, reversed depth range 1..0");
    {
        // The same with pix_center 0 (vtxcntl 4): + 0.5 pixel -> x offset 1 / 1280, y offset 1 / 720.
        File c = Textured();
        c[xr::kPaSuVtxCntl] = 4;
        const HostViewport hc = FoldViewport(Decode(c, vs, ps).viewport, 1280, 720);
        Expect(Near(hc.offset[0], 1.0f / 1280) && Near(hc.offset[1], 1.0f / 720), "fold with pix_center 0: half-pixel offsets");
    }
    // The OpenGL-style clip space (dx_clip_space_def 0) with z scale 0.5, offset 0.5: range 0..1 and ndc z * 0.5 + 0.5 (:442-494).
    GuestViewport gl = d.viewport;
    gl.dxClipSpace = false, gl.scale[2] = 0.5f, gl.offset[2] = 0.5f;
    const HostViewport hg = FoldViewport(gl, 1280, 720);
    Expect(hg.scale[2] == 0.5f && hg.offset[2] == 0.5f && hg.zMin == 0 && hg.zMax == 1, "fold: -W..W clip space remapped");
    // A depth range 0.2..0.7 (D3D9 MinZ 0.2, MaxZ 0.7: z scale 0.5, offset 0.2) saturates nothing: zMin 0.2, zMax 0.7.
    GuestViewport dz = d.viewport;
    dz.scale[2] = 0.5f, dz.offset[2] = 0.2f;
    const HostViewport hd = FoldViewport(dz, 1280, 720);
    Expect(Near(hd.zMin, 0.2f) && Near(hd.zMax, 0.7f), "fold: D3D9 depth range 0.2..0.7");
    // program 10010001: bits 0 (vs_num_reg 1), 16 (vs_resource), 28 (ps_export_mode bits 27-30 = 2); export count and mode 0.
    Expect(d.program.vsNumReg == 1 && d.program.psNumReg == 0 && d.program.vsResource && !d.program.psResource && d.program.vsExportCount == 0 &&
               d.program.vsExportMode == 0 && d.program.psExportMode == 2 && !d.program.paramGen,
           "SQ_PROGRAM_CNTL 10010001 fields");
    Expect(d.vertexSlots.size() == 1 && d.vertexSlots[0].physical == 0x1B480000 && d.vertexSlots[0].sizeWords == 40 && d.vertexSlots[0].endian == 0,
           "vertex slot 0: 1B480000, 40 words");
    Expect(VertexWindowDwords(d.vertexSlots[0], 3) == 20, "vertex window for high index 3: 3 * 5 + 5 = 20");
    // Texture slot 1 (both stages, the constant's filters).
    Expect(d.textureSlots.size() == 2 && d.textureSlots[0].slot == 1 && d.textureSlots[1].slot == 2, "two texture slots, ascending");
    const TextureSlot& t1 = d.textureSlots[0];
    Expect(t1.type == 2 && t1.format == 6 && t1.endian == 2 && t1.physical == 0x1B600000 && !t1.tiled && t1.pitchTexels == 256, "slot 1: linear k_8_8_8_8 8in32 at 1B600000, pitch 256");
    Expect(t1.width == 256 && t1.height == 128 && t1.depth == 1 && t1.dimension == 1 && t1.fetchDimension == 1 && !t1.stacked, "slot 1: 256 x 128 2D");
    Expect(t1.clamp[0] == 2 && t1.clamp[1] == 2 && t1.clamp[2] == 0 && t1.sign[0] == 0 && !t1.gamma, "slot 1: clamp to edge x y, unsigned");
    Expect(t1.swizzle == 0x688 && t1.magFilter == 1 && t1.minFilter == 1 && t1.mipFilter == 2 && t1.expAdjust == 0 && t1.lodBias == 0 && t1.anisoFilter == 0,
           "slot 1: XYZW, linear, base map, no bias");
    Expect(t1.mipMinLevel == 0 && t1.mipMaxLevel == 0 && t1.borderColor == 0 && t1.borderSize == 0 && t1.numFormat == 0, "slot 1: one level");
    const TextureSlot& t2 = d.textureSlots[1];
    Expect(t2.format == 18 && t2.tiled && t2.physical == 0x1B700000 && t2.pitchTexels == 512 && t2.width == 512 && t2.height == 512, "slot 2: tiled DXT1 512 x 512");
    Expect(t2.gamma && t2.sign[0] == 3 && t2.sign[3] == 0 && t2.swizzle == 0xA88 && t2.expAdjust == -1 && t2.lodBias == -16, "slot 2: gamma, XYZ1, exp -1, lod bias -16");
    Expect(t2.magFilter == 0 && t2.minFilter == 1 && t2.mipFilter == 2, "slot 2: the instruction's point mag filter overrides the constant's linear");
    Expect(d.vsLoopRegisters.empty() && d.psLoopRegisters.empty() && d.pixelInvocations == uint64_t(1280) * 720, "loop inputs");

    // Index buffer controls.
    File g = Textured();
    g[xr::kVgtDmaBase] = 0;
    CONTROL(Decode(g, vs, ps), "physical address 0");
    g = Textured(), g[xr::kVgtDmaSize] = (1u << 30) | 4;  // 4 indices for 6
    CONTROL(Decode(g, vs, ps), "4 indices for a draw of 6");
    g = Textured(), g[xr::kVgtDmaBase] = 0x1FFFFFF8;  // + 12 bytes passes 512 MB
    CONTROL(Decode(g, vs, ps), "past physical memory");
    g = Textured(), g[xr::kVgtDmaBase] = 0x1FFFFFF4;  // + 12 = 0x20000000 exactly: fits
    Expect(Decode(g, vs, ps).kind == DrawKind::Draw, "an index buffer ending exactly at 512 MB fits");
    g = Textured(), g[xr::kVgtDmaSize] = (2u << 30) | 6;  // 16-bit with 8in32
    CONTROL(Decode(g, vs, ps), "16-bit indices with endian 2");
    g = Textured(), g[xr::kVgtDrawInitiator] = 0x00060804, g[xr::kVgtDmaSize] = (2u << 30) | 6;  // 32-bit 8in32: fine, 24 bytes
    d = Decode(g, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.indices.is32Bit && d.indices.bytes == 24 && d.indices.endian == 2, "32-bit 8in32 indices, 24 bytes");
    g = Textured(), g[xr::kVgtDmaBase] = 0x1B500003;  // the low bit is masked for 16-bit indices (X pm4:1042)
    Expect(Decode(g, vs, ps).indices.physical == 0x1B500002, "16-bit index base masked to 2 bytes");
    g = Textured(), g[xr::kVgtIndxOffset] = 0x01000005, g[xr::kVgtMinVtxIndx] = 2;  // 24-bit fields
    d = Decode(g, vs, ps);
    Expect(d.indexOffset == 5 && d.minIndex == 2, "VGT_INDX_OFFSET and VGT_MIN_VTX_INDX are 24-bit");

    // Texture controls (each on slot 1 unless named).
    auto word = [](File& h, uint32_t slot, uint32_t i) -> uint32_t& { return h[xr::kFetchConstants + 6 * slot + i]; };
    g = Textured(), word(g, 1, 0) = 0x02004800;  // type 0
    CONTROL(Decode(g, vs, ps), "type 0 is not a texture");
    g = Textured(), word(g, 1, 1) = 0x00000086;  // base 0
    CONTROL(Decode(g, vs, ps), "base address 0");
    g = Textured(), word(g, 1, 1) = 0x1B600094;  // format 20 (DXT4_5)
    CONTROL(Decode(g, vs, ps), "texture format 20");
    g = Textured(), word(g, 1, 5) = 0x400;  // dimension 2 (3D) while the shader fetches 2D
    CONTROL(Decode(g, vs, ps), "the shader fetches dimension 1");
    g = Textured(), word(g, 1, 1) = 0x1B600486;  // stacked
    CONTROL(Decode(g, vs, ps), "stacked");
    g = Textured(), word(g, 1, 0) = 0x02004C02;  // clamp_x 3 (mirror clamp to edge)
    CONTROL(Decode(g, vs, ps), "clamp mode 3");
    g = Textured(), word(g, 1, 0) = 0x02005802, word(g, 1, 5) = 0x202;  // clamp_x 6 (border) with border colour 2
    CONTROL(Decode(g, vs, ps), "border colour 2");
    g = Textured(), word(g, 1, 0) = 0x02005802, word(g, 1, 5) = 0x201;  // border white: accepted
    Expect(Decode(g, vs, ps).kind == DrawKind::Draw, "clamp to a white border is accepted");
    g = Textured(), word(g, 1, 3) = 0x01280D10 | (1u << 25);  // aniso 1
    CONTROL(Decode(g, vs, ps), "anisotropic");
    g = Textured(), word(g, 1, 3) = 0x01280D10 | (1u << 31);  // border_size
    CONTROL(Decode(g, vs, ps), "border_size");
    g = Textured(), word(g, 1, 3) = 0x00A80D10;  // mag 1, min 1, mip 1 (linear) ...
    word(g, 1, 4) = 4u << 6;                     // ... with levels 0-4
    CONTROL(Decode(g, vs, ps), "mipmapping");
    g = Textured(), word(g, 1, 4) = 4u << 6;  // levels 0-4 with the base-map mip filter: accepted, base only
    d = Decode(g, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.textureSlots[0].mipMaxLevel == 4, "mip levels with the base-map filter are accepted");
    g = Textured(), word(g, 1, 4) = (2u << 2) | (1u << 6);  // min 2 above max 1
    CONTROL(Decode(g, vs, ps), "mip_min_level 2 above");
    g = Textured(), word(g, 1, 3) = 0x01280000 | (0x6 << 1);  // swizzle component 0 = 6
    CONTROL(Decode(g, vs, ps), "swizzle component 6");
    g = Textured(), word(g, 1, 0) = 0x02004802 | (2u << 2);  // sign x biased
    CONTROL(Decode(g, vs, ps), "component signs");
    g = Textured(), word(g, 1, 0) = 0x02004802 | (3u << 2) | (3u << 4);  // gamma on x y only
    CONTROL(Decode(g, vs, ps), "component signs");
    g = Textured(), word(g, 1, 3) = 0x01280D11;  // integer number format with k_8_8_8_8
    CONTROL(Decode(g, vs, ps), "integer number format");
    g = Textured(), word(g, 1, 0) = 0x00004802;  // pitch 0 < width 256
    CONTROL(Decode(g, vs, ps), "pitch 0 texels");
    g = Textured(), word(g, 1, 3) = 0x01300D10;  // mag filter 2 (base map; bits 19-20) is not a mag filter
    CONTROL(Decode(g, vs, ps), "mag/min filter 2/1");
    g = Textured();
    {
        ShaderTranslation ps2 = Ps(2);
        Sample(ps2, 1, 1, {1, 1, 3, 3, 3});  // the PS fetches slot 1 with explicit linear filters, the VS with the constant's (3)
        CONTROL(Decode(g, vs, ps2), "different filters");
    }
    // k_16_16 (25) is modelled only as signed integer (FR); k_32_FLOAT (36) accepts signed; k_2_10_10_10 unsigned.
    g = Textured(), word(g, 1, 1) = 0x1B600099;  // format 25, unsigned, fraction
    CONTROL(Decode(g, vs, ps), "k_16_16");
    g = Textured(), word(g, 1, 1) = 0x1B600099, word(g, 1, 0) = 0x02004802 | 0x154, word(g, 1, 3) = 0x01280D11;  // signs 1111, integer
    Expect(Decode(g, vs, ps).kind == DrawKind::Draw, "signed integer k_16_16 accepted");
    g = Textured(), word(g, 1, 1) = 0x1B6000A4, word(g, 1, 0) = 0x02004802 | 0x154;  // k_32_FLOAT signed
    Expect(Decode(g, vs, ps).kind == DrawKind::Draw, "signed k_32_FLOAT accepted");
    // A 3D fetch needs a linear 3D constant whose volume filters equal mag/min; a cube needs square faces
    // (the vertex shader samples nothing here: `vs` fetches slot 1 as 2D, which would disagree).
    const ShaderTranslation vsPlain = Vs({0}, {5}, {5}, 2);
    g = Textured();
    {
        ShaderTranslation ps3 = Ps(2);
        Sample(ps3, 1, 2);
        word(g, 1, 5) = 0x400;                            // dimension 2
        word(g, 1, 2) = 63 | (31u << 11) | (7u << 22);    // 64 x 32 x 8
        word(g, 1, 4) = 0x3;                              // vol mag/min linear = mag/min linear
        d = Decode(g, vsPlain, ps3);
        Expect(d.kind == DrawKind::Draw && d.textureSlots[0].width == 64 && d.textureSlots[0].height == 32 && d.textureSlots[0].depth == 8, "3D 64 x 32 x 8 decoded");
        if (d.kind != DrawKind::Draw)
            std::printf("  (%s)\n", d.error.c_str());
        word(g, 1, 4) = 0x0;  // volume filters point, mag/min linear
        CONTROL(Decode(g, vsPlain, ps3), "3D volume filters");
        word(g, 1, 4) = 0x3, word(g, 1, 0) |= 1u << 31;  // tiled 3D
        CONTROL(Decode(g, vsPlain, ps3), "tiled 3D");
    }
    g = Textured();
    {
        ShaderTranslation ps3 = Ps(2);
        Sample(ps3, 1, 3);
        word(g, 1, 5) = 0x600;  // cube with 256 x 128 faces
        CONTROL(Decode(g, vsPlain, ps3), "not square");
        word(g, 1, 2) = 255 | (255u << 13);
        d = Decode(g, vsPlain, ps3);
        Expect(d.kind == DrawKind::Draw && d.textureSlots[0].dimension == 3 && d.textureSlots[0].fetchDimension == 3, "a 256 x 256 cube is accepted");
    }
    // Back-face stencil: backface_enable with its own functions and RB_STENCILREFMASK_BF.
    g = Textured();
    g[xr::kRbDepthControl] = 0xD4B087F7;  // 0x8777 | backface (bit 7) | bf func 3 (20-22) | bf fail 1 (23-25) | bf zpass 5 (26-28) | bf zfail 6 (29-31)
    g[xr::kRbStencilRefMask] = 0x00FFFF01;
    g[xr::kRbStencilRefMaskBf] = 0x00104020;
    d = Decode(g, vs, ps);
    const DepthStencil& s = d.depthStencil;
    Expect(d.kind == DrawKind::Draw && s.backfaceEnable && s.front.func == 7 && s.front.zPass == 2 && s.front.ref == 1, "backface: front unchanged");
    Expect(s.back.func == 3 && s.back.fail == 1 && s.back.zPass == 5 && s.back.zFail == 6 && s.back.ref == 0x20 && s.back.compareMask == 0x40 && s.back.writeMask == 0x10,
           "backface: back functions and RB_STENCILREFMASK_BF");
    // Loop registers per stage: the VS reads loop 3, the PS loops 0 and 31.
    g = Textured();
    g[xr::kLoopConstants + 3] = 0x10, g[xr::kLoopConstants + 0] = 0x20, g[xr::kLoopConstants + 31] = 0x30;
    {
        ShaderTranslation vsl = vs, psl = ps;
        vsl.loopConstants = {3}, psl.loopConstants = {0, 31};
        d = Decode(g, vsl, psl);
        Expect(d.vsLoopRegisters == std::vector<uint32_t>{0x10} && d.psLoopRegisters == std::vector<uint32_t>{0x20, 0x30}, "loop registers per stage");
    }
    // Quad lists are accepted (host-expanded); DRAW_INDX with a zero token is the same draw.
    g = Textured(), g[xr::kVgtDrawInitiator] = 0x0004000D | (2u << 6);  // quad list, auto, 4
    Expect(Decode(g, vs, ps).hostPrimitive == HostPrimitive::QuadList, "quad list accepted for host expansion");
    g = Textured();
    Expect(Decode(g, vs, ps, 0x22, 0).kind == DrawKind::Draw, "DRAW_INDX with a zero viz token");
    CONTROL(Decode(g, vs, ps, 0x10, 0), "draw opcode 00000010");
}

// Every distinct `draw` + `state` combination of trace13 (13 of them, OBSERVED; the `state` line follows each `draw`
// line) decodes to Draw, NoWrite or Resolve. The fixtures above carry the three most common; the rest are here, each
// named by the line of its first `draw`.
void TestObservedStates()
{
    const ShaderTranslation vs = Vs({0}, {5}, {5}, 1), ps = Ps(1);
    // Line 612, draw 278 (9449 such): the point draw on the 1X surface, colorctl 87000007 (alpha-to-mask offsets, test off,
    // its enable bit 4 clear), stencilref 00FFFF00, the reversed viewport z (-1 / 1): NoWrite before any of that is looked at.
    // Lines 111 (216 such, interp 0) and 45690 (24, interp 00010001, z 1 / 0) differ only in those recorded registers.
    File p = PointDraw();
    p[xr::kRbSurfaceInfo] = 0x14000500, p[xr::kRbColorInfo] = 0x00020000, p[xr::kRbDepthInfo] = 0x000102D0;
    p[xr::kRbBlendControl0] = 0x00010001, p[xr::kRbColorControl] = ((0x87u << 24) | 7), p[xr::kRbStencilRefMask] = 0x00FFFF00;
    p[xr::kPaClVportXScale + 4] = F(-1.0f), p[xr::kPaClVportXScale + 5] = F(1.0f);
    Expect(Decode(p, Vs(), Ps(0)).kind == DrawKind::NoWrite, "line 612: point draw on the 1X surface is NoWrite");
    p[xr::kSqInterpolatorCntl] = 0, p[xr::kRbSurfaceInfo] = 0x0A020280, p[xr::kRbColorControl] = 0, p[xr::kRbStencilRefMask] = 0x00FF0000;
    p[xr::kPaClVportXScale + 4] = F(1.0f), p[xr::kPaClVportXScale + 5] = F(0.0f);
    Expect(Decode(p, Vs(), Ps(0)).kind == DrawKind::NoWrite, "line 111: 4X point draw with interp 0 is NoWrite");
    // Line 103, draw 48 (12 such): the clear with interp 0 (the first clears of a run, before SQ_INTERPOLATOR_CNTL is set).
    File c = RectClear();
    c[xr::kSqInterpolatorCntl] = 0;
    DrawState d = Decode(c, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.program.paramShade == 0, "line 103: clear with interp 0 is Draw");
    // Line 45817, draw 19501 (39 such): the UI rect list, initiator 00180088 = rect list (8), auto, 24 indices; otherwise the quad's state
    // (line 601: vtxcntl 5, stencilref 00FFFF00, z -1 / 1, interp 00010001).
    File r = UiQuad();
    r[xr::kVgtDrawInitiator] = 0x00180088;
    d = Decode(r, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.hostPrimitive == HostPrimitive::RectList && d.indices.count == 24 && !d.indices.indexed && d.systemFlags == 4,
           "line 45817: UI rect list, 24 auto indices, xe_flags 4");
    Expect(d.depthStencil.front.ref == 0 && d.depthStencil.front.compareMask == 0xFF && d.depthStencil.front.writeMask == 0xFF && !d.depthStencil.stencilEnable,
           "line 45817: stencilref 00FFFF00 decoded (ref 0, masks FF), stencil off");
    Expect(d.program.paramShade == 1 && d.program.samplingPattern == 1 && d.program.vsConstBase == 0 && d.program.psConstBase == 256 &&
               d.program.vsConstSize == 255 && d.program.psConstSize == 255,
           "line 45817: interp 00010001, vsconst 000FF000, psconst 000FF100 recorded");
    // Line 45744, draw 19472 (39 such): a 4-vertex triangle strip into the k_8_8_8_8_GAMMA target (color 00010000), no blending
    // (00010001), colorctl 87000007, clip 00090000 (clip_disable and dx_clip_space_def), sumode 00018000 (msaa, window offset),
    // vte 00000400 (no viewport scale or offset: identity; vtx_w0_fmt), program 10010401 (vs 1, ps_num_reg 4, vs_resource,
    // ps_export_mode 2), depthctl 00700730, vtxcntl 4 (half pixel), stencilref 00FFFF00, vport z 1 / 0 (unused: vte enables 0).
    File s = UiQuad();
    s[xr::kVgtDrawInitiator] = 0x00040086, s[xr::kRbColorInfo] = 0x00010000, s[xr::kRbBlendControl0] = 0x00010001;
    s[xr::kRbColorControl] = ((0x87u << 24) | 7), s[xr::kPaClClipCntl] = 0x00090000, s[xr::kPaSuScModeCntl] = 0x00018000;
    s[xr::kPaClVteCntl] = 0x400, s[xr::kSqProgramCntl] = 0x10010401, s[xr::kPaSuVtxCntl] = 4;
    s[xr::kPaClVportXScale + 4] = F(1.0f), s[xr::kPaClVportXScale + 5] = F(0.0f);
    d = Decode(s, vs, ps);
    Expect(d.kind == DrawKind::Draw && d.error.empty(), "line 45744: triangle strip into the gamma target: Draw");
    if (d.kind != DrawKind::Draw)
        std::printf("  (%s)\n", d.error.c_str());
    Expect(d.hostPrimitive == HostPrimitive::TriangleStrip && d.indices.count == 4 && d.target.colorFormat == 1 && !d.blend.enabled && !d.alphaTest.enabled,
           "line 45744: strip of 4, k_8_8_8_8_GAMMA, no blend, no alpha test");
    Expect(!d.cullBack && !d.cullFront && d.msaaEnable && d.viewport.clipDisabled && d.viewport.dxClipSpace && d.systemFlags == 4 &&
               d.viewport.scale[0] == 1 && d.viewport.scale[1] == 1 && d.viewport.offset[1] == 0 && d.viewport.offsetAddXy[0] == 0.5f,
           "line 45744: no culling, clipping disabled, identity viewport with the half pixel, xe_flags 4");
    Expect(d.program.vsNumReg == 1 && d.program.psNumReg == 4 && d.program.vsResource && d.program.psExportMode == 2, "program 10010401");
    // With clipping disabled the fold passes z through and the identity viewport maps pixels to the 1280 x 720 target.
    const HostViewport h = FoldViewport(d.viewport, 1280, 720);
    Expect(Near(h.scale[0], 2.0f / 1280) && Near(h.offset[0], 1.0f / 1280 - 1.0f) && h.scale[2] == 1 && h.offset[2] == 0 && h.zMin == 0 && h.zMax == 1,
           "line 45744: screen-space fold");
    // Line 607, draw 277 (394 such; 45750: 39 with colour 0 and z 1 / 0; 45685: 1): mode 6 with the clear's packet on the 1X
    // surface, program 00010002, vtxcntl 4, interp 00010001, z -1 / 1: a resolve, decoded elsewhere.
    File q = UiQuad();
    q[xr::kRbModeControl] = 6, q[xr::kVgtDrawInitiator] = 0x00030088, q[xr::kRbColorInfo] = 0x00020000, q[xr::kRbDepthControl] = 0;
    q[xr::kRbBlendControl0] = 0x00010001, q[xr::kRbColorControl] = ((0x87u << 24) | 7), q[xr::kPaClClipCntl] = 0x00010000, q[xr::kPaSuScModeCntl] = 0x00010000;
    q[xr::kPaClVteCntl] = 0x300, q[xr::kSqProgramCntl] = 0x00010002, q[xr::kPaSuVtxCntl] = 4;
    Expect(Decode(q, vs, ps).kind == DrawKind::Resolve, "line 607: mode 6 is a resolve");
    q[xr::kRbColorInfo] = 0, q[xr::kPaClVportXScale + 4] = F(1.0f), q[xr::kPaClVportXScale + 5] = F(0.0f);
    Expect(Decode(q, vs, ps).kind == DrawKind::Resolve, "line 45750: mode 6 with colour 0 is a resolve");
    // The reversed depth range of the UI draws through the fold with clipping: a viewport z scale -1, offset 1 folds to
    // zMin 1, zMax 0 whether or not the depth test is on (X draw_util.cc:437-441, :508-509; kept reversed).
    File u = UiQuad();
    GuestViewport gv = Decode(u, vs, ps).viewport;
    HostViewport hr = FoldViewport(gv, 1280, 720);
    Expect(hr.zMin == 1 && hr.zMax == 0 && hr.scale[2] == 1 && hr.offset[2] == 0, "UI viewport z -1 / 1: reversed range 1..0, ndc z untouched");
    gv.dxClipSpace = false;  // the -W..W form of the same registers: near = 1 - (-1) = 2 -> 1, far = 1 + (-1) = 0
    hr = FoldViewport(gv, 1280, 720);
    Expect(hr.zMin == 1 && hr.zMax == 0 && hr.scale[2] == 0.5f && hr.offset[2] == 0.5f, "the same in -W..W clip space: saturated 1..0");
}

}  // namespace

int main()
{
    TestPointDraws();
    TestRectClear();
    TestScissor();
    TestTextured();
    TestObservedStates();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
