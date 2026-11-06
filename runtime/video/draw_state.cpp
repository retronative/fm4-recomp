// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The draw state decode (draw_state.h). "X" is Xenia xenia-canary 67f16c4e315f src/xenia/gpu/;
// "FR" is ForzaRecomp runtime/gpu/render.cpp (facts and simplifications, re-derived here).
#include "draw_state.h"
#include <algorithm>
#include <bit>
#include <cstdio>

namespace runtime::video {
namespace {

std::string Hex(uint32_t v)
{
    char text[16];
    std::snprintf(text, sizeof text, "%08X", v);
    return text;
}
std::string Num(uint64_t v) { return std::to_string(v); }

// A signed field of `bits` bits at `shift` (two's complement).
int32_t SignedField(uint32_t word, uint32_t shift, uint32_t bits)
{
    const uint32_t v = (word >> shift) & ((1u << bits) - 1), sign = 1u << (bits - 1);
    return int32_t((v ^ sign)) - int32_t(sign);
}

struct Decoder
{
    const DrawInputs& in;
    DrawState& d;
    bool refused = false;

    uint32_t Reg(uint32_t index) const { return in.read(in.context, index); }
    float RegFloat(uint32_t index) const { return std::bit_cast<float>(Reg(index)); }
    bool Refuse(const std::string& why)
    {
        if (!refused)
            d.kind = DrawKind::Refused, d.error = why, refused = true;
        return false;
    }

    // ---- The packet (X pm4_command_processor_implement.h:985-1099; registers.h:272-296) ----
    bool Packet()
    {
        if (in.opcode != 0x22 && in.opcode != 0x36)
            return Refuse("draw opcode " + Hex(in.opcode) + " is not DRAW_INDX (22) or DRAW_INDX_2 (36)");
        d.opcode = in.opcode;
        d.vizQueryToken = in.vizQueryToken;
        if (d.vizQueryToken != 0)  // a conditional draw on a visibility query (X :988-992)
            return Refuse("DRAW_INDX with a viz-query token " + Hex(d.vizQueryToken) + ": conditional draws are not modelled");
        const uint32_t vizQuery = Reg(xr::kPaScVizQuery);
        if (vizQuery & 1)  // viz_query_ena (X registers.h:479; Xenia drops the draw when kill_pix_post_hi_z is also set, pm4:1074-1075)
            return Refuse("PA_SC_VIZ_QUERY " + Hex(vizQuery) + ": visibility queries are not modelled");
        const uint32_t path = Reg(xr::kVgtOutputPathCntl) & 3;
        if (path == 1)  // kTessellationEnable (X xenos.h:876-880; registers.h:362-369)
            return Refuse("VGT_OUTPUT_PATH_CNTL path_select 1: tessellation is not modelled");

        d.initiator = Reg(xr::kVgtDrawInitiator);
        d.primitiveType = d.initiator & 0x3F;
        d.sourceSelect = (d.initiator >> 6) & 3;
        d.majorMode = (d.initiator >> 8) & 3;
        d.indices.is32Bit = (d.initiator >> 11) & 1;
        d.indices.count = d.initiator >> 16;
        if (d.majorMode != 0)  // kExplicit (X xenos.h:791-800): explicit major mode, 2D and patch primitives
            return Refuse("VGT_DRAW_INITIATOR " + Hex(d.initiator) + ": major mode " + Num(d.majorMode) + " (explicit) is not modelled");
        if (d.primitiveType >= 0x10)
            return Refuse("VGT_DRAW_INITIATOR " + Hex(d.initiator) + ": primitive type " + Num(d.primitiveType) + " (" +
                          PrimitiveTypeName(d.primitiveType) + ") is not modelled");
        if (d.indices.count == 0)
            return Refuse("VGT_DRAW_INITIATOR " + Hex(d.initiator) + ": a draw of 0 indices");
        switch (d.sourceSelect)  // X xenos.h:784-788 SourceSelect; pm4:1010-1067
        {
        case 0:  // kDMA: an index buffer
        {
            d.indices.indexed = true;
            const uint32_t base = Reg(xr::kVgtDmaBase), size = Reg(xr::kVgtDmaSize);
            const uint32_t bytesEach = d.indices.is32Bit ? 4 : 2;
            d.indices.physical = base & ~(bytesEach - 1);  // X pm4:1042
            d.indices.endian = size >> 30;                  // X registers.h:277
            const uint64_t bytes = uint64_t(size & 0xFFFFFF) * bytesEach;  // X pm4:1045
            if (d.indices.physical == 0)
                return Refuse("VGT_DMA_BASE " + Hex(base) + ": an index buffer at physical address 0");
            if (bytes < uint64_t(d.indices.count) * bytesEach)
                return Refuse("VGT_DMA_SIZE " + Hex(size) + ": " + Num(size & 0xFFFFFF) + " indices for a draw of " + Num(d.indices.count));
            if (uint64_t(d.indices.physical) + bytes > in.physicalBytes)
                return Refuse("VGT_DMA_BASE " + Hex(base) + " + " + Num(bytes) + " bytes is past physical memory (" + Num(in.physicalBytes) + " bytes)");
            if (!d.indices.is32Bit && d.indices.endian >= 2)  // X xenos.h:1055-1068: 16-bit GpuSwap knows none and 8in16 only
                return Refuse("VGT_DMA_SIZE " + Hex(size) + ": 16-bit indices with endian " + Num(d.indices.endian) + " (8in32 or 16in32)");
            d.indices.bytes = uint32_t(bytes);
            break;
        }
        case 2:  // kAutoIndex (X pm4:1057-1061)
            d.indices.indexed = false;
            break;
        case 1:  // kImmediate: Xenia does not support it either (X pm4:1048-1056)
            return Refuse("VGT_DRAW_INITIATOR " + Hex(d.initiator) + ": immediate indices (source select 1) are not modelled");
        default:
            return Refuse("VGT_DRAW_INITIATOR " + Hex(d.initiator) + ": source select 3 is invalid");
        }
        d.indexOffset = Reg(xr::kVgtIndxOffset) & 0xFFFFFF;  // X registers.h:327-341
        d.minIndex = Reg(xr::kVgtMinVtxIndx) & 0xFFFFFF;     // :343-350
        d.maxIndex = Reg(xr::kVgtMaxVtxIndx) & 0xFFFFFF;     // :352-360
        return true;
    }

    // ---- Writes: RB_MODECONTROL, RB_COLOR_MASK, RB_DEPTHCONTROL ----
    void DepthStencilDecode()
    {
        const uint32_t c = Reg(xr::kRbDepthControl);  // X registers.h:744-765
        DepthStencil& s = d.depthStencil;
        s.stencilEnable = c & 1;
        s.zEnable = (c >> 1) & 1;
        s.zWriteEnable = (c >> 2) & 1;
        s.zFunc = (c >> 4) & 7;
        s.backfaceEnable = (c >> 7) & 1;
        const uint32_t front = Reg(xr::kRbStencilRefMask), back = s.backfaceEnable ? Reg(xr::kRbStencilRefMaskBf) : front;
        s.front = {(c >> 8) & 7, (c >> 11) & 7, (c >> 14) & 7, (c >> 17) & 7, front & 0xFF, (front >> 8) & 0xFF, (front >> 16) & 0xFF};
        s.back = s.backfaceEnable ? DepthStencil::Face{(c >> 20) & 7, (c >> 23) & 7, (c >> 26) & 7, (c >> 29) & 7, back & 0xFF, (back >> 8) & 0xFF,
                                                       (back >> 16) & 0xFF}
                                  : s.front;
    }
    bool WritesNothing() const
    {
        const DepthStencil& s = d.depthStencil;
        return d.colorWriteMask == 0 && !(s.zEnable && s.zWriteEnable) && !s.stencilEnable;
    }

    // ---- Primitive (X xenos.h:41-57; primitive_processor.cc:378-420) ----
    bool Primitive()
    {
        const bool noWrite = d.kind == DrawKind::NoWrite;
        switch (d.primitiveType)
        {
        case 4: d.hostPrimitive = HostPrimitive::TriangleList; break;
        case 6: d.hostPrimitive = HostPrimitive::TriangleStrip; break;
        case 8: d.hostPrimitive = HostPrimitive::RectList; break;
        case 13: d.hostPrimitive = HostPrimitive::QuadList; break;
        case 1:
            if (!noWrite)  // point sprites: PA_SU_POINT_SIZE expansion (X primitive_processor.cc:379-385 kPointListAsTriangleStrip)
                return Refuse("primitive type 1 (point list) that writes colour, depth or stencil: point sprites are not modelled (PA_SU_POINT_SIZE " +
                              Hex(Reg(xr::kPaSuPointSize)) + ", PA_SU_POINT_MINMAX " + Hex(Reg(xr::kPaSuPointMinMax)) + ")");
            d.hostPrimitive = HostPrimitive::PointNoWrite;
            break;
        default:
            return Refuse("primitive type " + Num(d.primitiveType) + " (" + PrimitiveTypeName(d.primitiveType) + ") is not modelled");
        }
        // Primitive restart applies to strips, fans, loops and polygons only (X primitive_processor.cc:619-628).
        const uint32_t sc = Reg(xr::kPaSuScModeCntl);
        if (((sc >> 21) & 1) && d.primitiveType == 6 && !noWrite)
            return Refuse("PA_SU_SC_MODE_CNTL " + Hex(sc) + " multi_prim_ib_ena with a triangle strip (reset index " +
                          Hex(Reg(xr::kVgtMultiPrimIbResetIndx) & 0xFFFFFF) + "): primitive restart is not modelled");
        return true;
    }

    // ---- Fixed function ----
    bool Raster()
    {
        const uint32_t sc = Reg(xr::kPaSuScModeCntl);  // X registers.h:416-449
        d.cullFront = sc & 1;
        d.cullBack = (sc >> 1) & 1;
        d.frontClockwise = (sc >> 2) & 1;  // "0 - front is CCW, 1 - front is CW" (:421-422)
        d.msaaEnable = (sc >> 15) & 1;
        d.provokingVertexLast = (sc >> 19) & 1;
        if ((sc >> 3) & 3)  // poly_mode (:427): 1 dual mode with polymode_front/back_ptype; only filled triangles are modelled
            return Refuse("PA_SU_SC_MODE_CNTL " + Hex(sc) + ": polygon mode " + Num((sc >> 3) & 3) + " (front type " + Num((sc >> 5) & 7) +
                          ", back type " + Num((sc >> 8) & 7) + ") is not modelled");
        if ((sc >> 11) & 7)  // poly_offset_front/back/para_enable (:430-432): PA_SU_POLY_OFFSET_*
            return Refuse("PA_SU_SC_MODE_CNTL " + Hex(sc) + ": polygon offset (enables " + Num((sc >> 11) & 7) + ", PA_SU_POLY_OFFSET_FRONT_SCALE " +
                          Hex(Reg(xr::kPaSuPolyOffsetFrontScale)) + ") is not modelled");
        if ((sc >> 20) & 1)  // persp_corr_dis (:439)
            return Refuse("PA_SU_SC_MODE_CNTL " + Hex(sc) + ": perspective correction disabled is not modelled");
        const uint32_t clip = Reg(xr::kPaClClipCntl);  // X registers.h:491-517
        if (clip & 0x3F)  // ucp_ena_0-5
            return Refuse("PA_CL_CLIP_CNTL " + Hex(clip) + ": user clip planes (" + Num(clip & 0x3F) + ") are not modelled");
        d.viewport.clipDisabled = (clip >> 16) & 1;
        d.viewport.dxClipSpace = (clip >> 19) & 1;
        const uint32_t vte = Reg(xr::kPaClVteCntl);  // X registers.h:520-537
        // The position format (bits 8-10) is undone by the vertex shader's xe_fixup from xe_flags (shader_glsl.h; X
        // vulkan_command_processor.cc:3547-3555 sets the same three flags from the same three bits).
        d.viewport.xyDividedByW = (vte >> 8) & 1;
        d.viewport.zDividedByW = (vte >> 9) & 1;
        d.viewport.wNotReciprocal = (vte >> 10) & 1;
        d.systemFlags = (d.viewport.xyDividedByW ? shader::kFlagXyDividedByW : 0) | (d.viewport.zDividedByW ? shader::kFlagZDividedByW : 0) |
                        (d.viewport.wNotReciprocal ? shader::kFlagWNotReciprocal : 0);
        for (uint32_t axis = 0; axis < 3; axis++)  // X draw_util.cc:306-319
        {
            d.viewport.scale[axis] = (vte >> (2 * axis)) & 1 ? RegFloat(xr::kPaClVportXScale + 2 * axis) : 1.0f;
            d.viewport.offset[axis] = (vte >> (2 * axis + 1)) & 1 ? RegFloat(xr::kPaClVportXScale + 2 * axis + 1) : 0.0f;
        }
        const uint32_t window = Reg(xr::kPaScWindowOffset);  // X registers.h:561-570, draw_util.cc:324-328
        if ((sc >> 16) & 1)
        {
            d.viewport.offsetAddXy[0] += float(SignedField(window, 0, 15));
            d.viewport.offsetAddXy[1] += float(SignedField(window, 16, 15));
        }
        if (!(Reg(xr::kPaSuVtxCntl) & 1))  // pix_center (X registers.h:455); draw_util.cc:329-332 (half_pixel_offset defaults to true)
        {
            d.viewport.offsetAddXy[0] += 0.5f;
            d.viewport.offsetAddXy[1] += 0.5f;
        }
        return true;
    }

    bool ScissorDecode()
    {
        // X draw_util.cc:639-713 (the portable branch).
        const uint32_t wtl = Reg(xr::kPaScWindowScissorTl), wbr = Reg(xr::kPaScWindowScissorBr);
        const uint32_t stl = Reg(xr::kPaScScreenScissorTl), sbr = Reg(xr::kPaScScreenScissorBr), window = Reg(xr::kPaScWindowOffset);
        int32_t tlX = int32_t(wtl & 0x3FFF), tlY = int32_t((wtl >> 16) & 0x3FFF);  // 14-bit (X registers.h:575-578)
        int32_t brX = int32_t(wbr & 0x3FFF), brY = int32_t((wbr >> 16) & 0x3FFF);  // (:588-590)
        if (!(wtl >> 31))  // window_offset_disable (:579)
        {
            const int32_t ox = SignedField(window, 0, 15), oy = SignedField(window, 16, 15);
            tlX += ox, tlY += oy, brX += ox, brY += oy;
        }
        tlX = std::max(tlX, SignedField(stl, 0, 15));  // (:684-688; screen scissor signed 15-bit, registers.h:542-556)
        tlY = std::max(tlY, SignedField(stl, 16, 15));
        brX = std::min(brX, SignedField(sbr, 0, 15));
        brY = std::min(brY, SignedField(sbr, 16, 15));
        const int32_t pitch = int32_t(Reg(xr::kRbSurfaceInfo) & 0x3FFF);  // (:698-699)
        tlX = std::min(tlX, pitch);
        brX = std::min(brX, pitch);
        tlX = std::max(tlX, 0);  // (:706-709)
        tlY = std::max(tlY, 0);
        brX = std::max(brX, tlX);
        brY = std::max(brY, tlY);
        d.scissor = {uint32_t(tlX), uint32_t(tlY), uint32_t(brX - tlX), uint32_t(brY - tlY)};
        d.pixelInvocations = uint64_t(d.scissor.width) * d.scissor.height;
        return true;
    }

    bool BlendDecode()
    {
        const uint32_t c = Reg(xr::kRbBlendControl0);  // X registers.h:726-742
        Blend& b = d.blend;
        b.srcColor = c & 31, b.colorOp = (c >> 5) & 7, b.dstColor = (c >> 8) & 31;
        b.srcAlpha = (c >> 16) & 31, b.alphaOp = (c >> 21) & 7, b.dstAlpha = (c >> 24) & 31;
        b.enabled = c != 0x00010001;
        for (uint32_t f : {b.srcColor, b.dstColor, b.srcAlpha, b.dstAlpha})  // X xenos.h:749-765 BlendFactor: 0, 1, 4-16
            if (f == 2 || f == 3 || f > 16)
                return Refuse("RB_BLENDCONTROL0 " + Hex(c) + ": blend factor " + Num(f) + " is undefined");
        for (uint32_t op : {b.colorOp, b.alphaOp})  // X xenos.h:768-774 BlendOp: 0-4
            if (op > 4)
                return Refuse("RB_BLENDCONTROL0 " + Hex(c) + ": blend operation " + Num(op) + " is undefined");
        for (uint32_t i = 0; i < 4; i++)
            b.constant[i] = RegFloat(xr::kRbBlendRed + i);
        const uint32_t cc = Reg(xr::kRbColorControl);  // X registers.h:630-682
        if ((cc >> 4) & 1)
            return Refuse("RB_COLORCONTROL " + Hex(cc) + ": alpha to mask (coverage) is not modelled");
        d.alphaTest.func = cc & 7;
        d.alphaTest.enabled = ((cc >> 3) & 1) && d.alphaTest.func != 7;
        d.alphaTest.ref = RegFloat(xr::kRbAlphaRef);
        return true;
    }

    bool ProgramDecode()
    {
        const uint32_t pc = Reg(xr::kSqProgramCntl);  // X registers.h:104-124
        Program& p = d.program;
        p.vsNumReg = pc & 0xFF, p.psNumReg = (pc >> 8) & 0xFF;
        p.vsResource = (pc >> 16) & 1, p.psResource = (pc >> 17) & 1;
        p.paramGen = (pc >> 18) & 1;
        p.vsExportCount = (pc >> 20) & 0xF;
        p.vsExportMode = (pc >> 24) & 7;
        p.psExportMode = (pc >> 27) & 0xF;
        if ((pc >> 19) & 1 || (pc >> 31) & 1)  // gen_index_pix, gen_index_vtx
            return Refuse("SQ_PROGRAM_CNTL " + Hex(pc) + ": index generation (gen_index_vtx/pix) is not modelled");
        if (p.vsExportMode != 0)  // X xenos.h:832-842: sprite / edge / kill vectors, multipass
            return Refuse("SQ_PROGRAM_CNTL " + Hex(pc) + ": vertex export mode " + Num(p.vsExportMode) + " is not modelled (0, position only)");
        if (p.paramGen)  // PsParamGen (X registers.h:133-207): the translator has no path for it
            return Refuse("SQ_PROGRAM_CNTL " + Hex(pc) + ": pixel parameter generation (param_gen) is not modelled");
        const uint32_t misc = Reg(xr::kSqContextMisc);  // X registers.h:126-215
        p.scOutputScreenXy = (misc >> 1) & 1;
        p.sampleControl = (misc >> 2) & 3;
        p.paramGenPos = (misc >> 8) & 0xFF;
        const uint32_t interp = Reg(xr::kSqInterpolatorCntl);  // X registers.h:217-227
        p.paramShade = interp & 0xFFFF;
        p.samplingPattern = interp >> 16;
        const uint32_t used = in.ps->interpolatorCount >= 16 ? 0xFFFF : (1u << in.ps->interpolatorCount) - 1;
        if (p.paramShade & used)
            return Refuse("SQ_INTERPOLATOR_CNTL " + Hex(interp) + ": flat shading (param_shade) of an interpolator the pixel shader reads is not modelled");
        const uint32_t vsc = Reg(xr::kSqVsConst), psc = Reg(xr::kSqPsConst);  // X registers.h:229-252
        p.vsConstBase = vsc & 0x1FF, p.vsConstSize = (vsc >> 12) & 0x1FF;
        p.psConstBase = psc & 0x1FF, p.psConstSize = (psc >> 12) & 0x1FF;
        // The translator binds ALU constants 0-255 to the vertex and 256-511 to the pixel shader (shader_glsl.h
        // bindings 2/3), as Xenia's backends do; a base saying otherwise is refused.
        if (p.vsConstBase != 0 || (p.psConstBase != 0 && p.psConstBase != 256))
            return Refuse("SQ_VS_CONST " + Hex(vsc) + " / SQ_PS_CONST " + Hex(psc) + ": a float constant base other than 0 / 256 is not modelled");
        if (in.ps->interpolatorCount > in.vs->interpolatorCount)
            return Refuse("the pixel shader reads " + Num(in.ps->interpolatorCount) + " interpolators, the vertex shader exports " +
                          Num(in.vs->interpolatorCount));
        return true;
    }

    // ---- Render target ----
    bool Target()
    {
        RenderTarget& t = d.target;
        const uint32_t si = Reg(xr::kRbSurfaceInfo), ci = Reg(xr::kRbColorInfo), di = Reg(xr::kRbDepthInfo);
        t.pitch = si & 0x3FFF;        // X registers.h:621
        t.msaa = (si >> 16) & 3;      // :623
        t.hizPitch = si >> 18;        // :624
        t.colorBase = ci & 0x7FF;     // :690
        t.colorFormatRaw = (ci >> 16) & 0xF;  // :693
        t.colorExpBias = SignedField(ci, 20, 6);  // :694
        t.depthBase = di & 0x7FF;     // :785
        t.depthFormat = (di >> 16) & 1;  // :788
        t.colorWritten = d.kind == DrawKind::Draw && d.colorWriteMask != 0;
        t.depthWritten = (d.depthStencil.zEnable && d.depthStencil.zWriteEnable) || d.depthStencil.stencilEnable;
        if (t.msaa == 3)
            return Refuse("RB_SURFACE_INFO " + Hex(si) + ": msaa_samples 3 is undefined");
        t.samples = 1u << t.msaa;
        if (t.pitch == 0)
            return Refuse("RB_SURFACE_INFO " + Hex(si) + ": a render target with no pitch");
        if ((ci >> 11) & 1)  // color_base_bit_11 (:691): past the 2048-tile EDRAM
            return Refuse("RB_COLOR_INFO " + Hex(ci) + ": colour base bit 11 is not modelled");
        if ((di >> 11) & 1)
            return Refuse("RB_DEPTH_INFO " + Hex(di) + ": depth base bit 11 is not modelled");
        // Storage formats (X xenos.h:394-404): 10 is 2 and 12 is 3 with another blending precision (KB gpu-edram-and-ring.md).
        t.colorFormat = t.colorFormatRaw == 10 ? 2 : t.colorFormatRaw == 12 ? 3 : t.colorFormatRaw;
        if (t.colorWritten)
        {
            if (t.colorFormat > 3)  // 0 8_8_8_8, 1 8_8_8_8_GAMMA, 2 2_10_10_10, 3 2_10_10_10_FLOAT (X xenos.h:299-318); the rest are not modelled
                return Refuse("RB_COLOR_INFO " + Hex(ci) + ": colour format " + Num(t.colorFormatRaw) + " is not modelled (0, 1, 2, 3, 10, 12 are)");
            if (t.colorExpBias != 0)
                return Refuse("RB_COLOR_INFO " + Hex(ci) + ": colour exponent bias " + Num(uint64_t(int64_t(t.colorExpBias))) + " is not modelled");
            if (t.depthWritten && t.colorBase == t.depthBase)
                return Refuse("RB_COLOR_INFO " + Hex(ci) + " / RB_DEPTH_INFO " + Hex(di) + ": colour and depth at the same EDRAM base, both written");
        }
        // Tiles (X xenos.h:467-480 GetSurfacePitchTiles; every accepted colour format is 32 bpp): 80 samples a tile row, the
        // 4X pitch doubled; rows a tile holds: 16 samples over 1 (1X) or 2 (2X, 4X) vertical samples.
        const uint32_t pitchSamples = t.pitch << (t.msaa == 2 ? 1 : 0);
        t.pitchTiles = (pitchSamples + 79) / 80;
        const uint32_t rowsPerTile = 16 / (t.msaa == 0 ? 1 : 2);
        const auto limit = [](uint32_t base, uint32_t other) { return other > base ? other : 2048u; };
        uint32_t tiles = 2048;
        if (t.colorWritten)
            tiles = std::min(tiles, limit(t.colorBase, t.depthBase) - t.colorBase);
        if (t.depthWritten)
            tiles = std::min(tiles, limit(t.depthBase, t.colorBase) - t.depthBase);
        t.edramRows = tiles / t.pitchTiles * rowsPerTile;
        if (t.edramRows == 0)
            return Refuse("RB_SURFACE_INFO " + Hex(si) + " RB_COLOR_INFO " + Hex(ci) + " RB_DEPTH_INFO " + Hex(di) + ": the render target has no rows");
        // An empty scissor gives a target height of 0: a defined draw that covers nothing (X draw_util.h:282-283), not a refusal.
        t.targetHeight = std::min(t.edramRows, d.scissor.y + d.scissor.height);
        return true;
    }

    // ---- Fetch constants ----
    bool VertexFetches()
    {
        const shader::ShaderTranslation& vs = *in.vs;
        for (size_t k = 0; k < vs.vertexFetchSlots.size(); k++)
        {
            VertexSlot s;
            s.slot = vs.vertexFetchSlots[k];
            if (s.slot >= 96)
                return Refuse("vertex fetch slot " + Num(s.slot) + " is past the 96 vertex fetch constants");
            const uint32_t w0 = Reg(xr::kFetchConstants + 2 * s.slot), w1 = Reg(xr::kFetchConstants + 2 * s.slot + 1);
            s.physical = w0 & ~3u;
            s.sizeWords = (w1 >> 2) & 0xFFFFFF;
            s.endian = w1 & 3;
            s.stride = k < vs.vertexFetchStrides.size() ? vs.vertexFetchStrides[k] : 0;
            s.end = k < vs.vertexFetchEnds.size() ? vs.vertexFetchEnds[k] : 0;
            s.valid = (w0 & 3) == 3 && s.sizeWords != 0;
            if (s.valid)
            {
                if (s.physical == 0)
                    return Refuse("vertex fetch constant " + Num(s.slot) + " (" + Hex(w0) + " " + Hex(w1) + ") at physical address 0");
                if (uint64_t(s.physical) + uint64_t(s.sizeWords) * 4 > in.physicalBytes)
                    return Refuse("vertex fetch constant " + Num(s.slot) + " (" + Hex(w0) + " " + Hex(w1) + ") reaches past physical memory");
            }
            d.vertexSlots.push_back(s);
        }
        return true;
    }

    static bool FormatAccepted(uint32_t f)
    {
        switch (f)  // FR render.cpp:586-636 (what FM4's first screens sample); X xenos.h:491-581 names
        {
        case 2: case 6: case 50:   // k_8, k_8_8_8_8, k_8_8_8_8_AS_16_16_16_16
        case 18: case 51:          // k_DXT1, k_DXT1_AS_16_16_16_16
        case 7: case 54:           // k_2_10_10_10, k_2_10_10_10_AS_16_16_16_16
        case 36: case 22: case 23: // k_32_FLOAT, k_24_8, k_24_8_FLOAT
        case 25:                   // k_16_16 (signed integer only)
        case 29: case 32:          // k_16_16_16_16_EXPAND, k_16_16_16_16_FLOAT
            return true;
        default:
            return false;
        }
    }

    bool TextureFetches()
    {
        struct Use { uint32_t slot, dimension; shader::ShaderTranslation::Filters filters; };
        std::vector<Use> uses;
        for (const shader::ShaderTranslation* stage : {in.vs, in.ps})
            for (size_t i = 0; i < stage->textureSlots.size(); i++)
            {
                const Use u{stage->textureSlots[i], stage->textureDimensions[i], stage->textureFilters[i]};
                auto same = std::find_if(uses.begin(), uses.end(), [&](const Use& v) { return v.slot == u.slot; });
                if (same == uses.end())
                    uses.push_back(u);
                else if (same->dimension != u.dimension || same->filters.mag != u.filters.mag || same->filters.min != u.filters.min ||
                         same->filters.volMag != u.filters.volMag || same->filters.volMin != u.filters.volMin || same->filters.mip != u.filters.mip)
                    return Refuse("texture slot " + Num(u.slot) + " is fetched with different filters or dimensions by the vertex and pixel shaders");
            }
        std::sort(uses.begin(), uses.end(), [](const Use& a, const Use& b) { return a.slot < b.slot; });
        for (const Use& u : uses)
        {
            TextureSlot t;
            t.slot = u.slot;
            if (t.slot >= 32)
                return Refuse("texture fetch slot " + Num(t.slot) + " is past the 32 texture fetch constants");
            for (uint32_t i = 0; i < 6; i++)
                t.words[i] = Reg(xr::kFetchConstants + 6 * t.slot + i);
            const std::string where = "texture slot " + Num(t.slot) + " (" + Hex(t.words[0]) + " " + Hex(t.words[1]) + " " + Hex(t.words[2]) + " " +
                                      Hex(t.words[3]) + " " + Hex(t.words[4]) + " " + Hex(t.words[5]) + "): ";
            // X xenos.h:1216-1245 dword 0.
            t.type = t.words[0] & 3;
            for (uint32_t i = 0; i < 4; i++)
                t.sign[i] = (t.words[0] >> (2 + 2 * i)) & 3;
            for (uint32_t i = 0; i < 3; i++)
                t.clamp[i] = (t.words[0] >> (10 + 3 * i)) & 7;
            t.signedRfMode = (t.words[0] >> 19) & 1;
            t.pitchTexels = ((t.words[0] >> 22) & 0x1FF) << 5;
            t.tiled = t.words[0] >> 31;
            // :1247-1252 dword 1.
            t.format = t.words[1] & 0x3F;
            t.endian = (t.words[1] >> 6) & 3;
            t.requestSize = (t.words[1] >> 8) & 3;
            t.stacked = (t.words[1] >> 10) & 1;
            t.nearestClampPolicy = (t.words[1] >> 11) & 1;
            t.physical = t.words[1] & 0xFFFFF000u;
            // :1297-1304 dword 5 (the dimension selects dword 2's layout).
            t.borderColor = t.words[5] & 3;
            t.dimension = (t.words[5] >> 9) & 3;
            t.packedMips = (t.words[5] >> 11) & 1;
            t.mipPhysical = t.words[5] & 0xFFFFF000u;
            // :1254-1272 dword 2 (each size stored minus one).
            switch (t.dimension)
            {
            case 0: t.width = (t.words[2] & 0xFFFFFF) + 1, t.height = 1, t.depth = 1; break;
            case 2: t.width = (t.words[2] & 0x7FF) + 1, t.height = ((t.words[2] >> 11) & 0x7FF) + 1, t.depth = (t.words[2] >> 22) + 1; break;
            default: t.width = (t.words[2] & 0x1FFF) + 1, t.height = ((t.words[2] >> 13) & 0x1FFF) + 1, t.depth = 1, t.stackDepth = (t.words[2] >> 26) + 1;
            }
            // :1274-1283 dword 3.
            t.numFormat = t.words[3] & 1;
            t.swizzle = (t.words[3] >> 1) & 0xFFF;
            t.expAdjust = SignedField(t.words[3], 13, 6);
            const uint32_t cMag = (t.words[3] >> 19) & 3, cMin = (t.words[3] >> 21) & 3, cMip = (t.words[3] >> 23) & 3;
            t.anisoFilter = (t.words[3] >> 25) & 7;
            t.arbitraryFilter = (t.words[3] >> 28) & 7;
            t.borderSize = t.words[3] >> 31;
            // :1285-1294 dword 4.
            const uint32_t cVolMag = t.words[4] & 1, cVolMin = (t.words[4] >> 1) & 1;
            t.mipMinLevel = (t.words[4] >> 2) & 0xF;
            t.mipMaxLevel = (t.words[4] >> 6) & 0xF;
            t.lodBias = SignedField(t.words[4], 12, 10);
            // The instruction's filters win unless they say 3 (shader_glsl.h Filters).
            t.magFilter = u.filters.mag != 3 ? u.filters.mag : cMag;
            t.minFilter = u.filters.min != 3 ? u.filters.min : cMin;
            t.mipFilter = u.filters.mip != 3 ? u.filters.mip : cMip;
            t.volMagFilter = u.filters.volMag != 3 ? u.filters.volMag : cVolMag;
            t.volMinFilter = u.filters.volMin != 3 ? u.filters.volMin : cVolMin;
            t.fetchDimension = u.dimension;
            t.gamma = t.sign[0] == 3 && t.sign[1] == 3 && t.sign[2] == 3 && t.sign[3] == 0;

            if (t.type != 2)  // X xenos.h:1124-1129; texture_cache.cc:867-889 (an invalid constant binds nothing in Xenia)
                return Refuse(where + "fetch constant type " + Num(t.type) + " is not a texture (2)");
            if (t.physical == 0)  // X texture_cache.cc:896-899 "no texture data at all"
                return Refuse(where + "base address 0");
            if (t.physical >= in.physicalBytes)
                return Refuse(where + "base address past physical memory");
            if (!FormatAccepted(t.format))
                return Refuse(where + "texture format " + Num(t.format) + " is not modelled");
            // The shader's dimension against the constant's (shader_glsl.h textureDimensions): 1 needs a 2D that is not stacked, 2 a 3D, 3 a cube.
            const uint32_t needed = u.dimension == 1 ? 1 : u.dimension == 2 ? 2 : u.dimension == 3 ? 3 : 0xFF;
            if (needed == 0xFF || t.dimension != needed)
                return Refuse(where + "the shader fetches dimension " + Num(u.dimension) + " (1 2D, 2 3D, 3 cube) but the constant's dimension is " +
                              Num(t.dimension) + " (0 1D, 1 2D or stacked, 2 3D, 3 cube)");
            if (t.stacked)
                return Refuse(where + "a stacked (2D array) texture is not modelled");
            if (t.dimension == 2 && t.tiled)
                return Refuse(where + "a tiled 3D texture is not modelled");
            if (t.dimension == 3 && t.width != t.height)
                return Refuse(where + "a cube map with faces " + Num(t.width) + "x" + Num(t.height) + " is not square");
            if (t.dimension != 2 && t.pitchTexels < t.width)
                return Refuse(where + "pitch " + Num(t.pitchTexels) + " texels is smaller than the width " + Num(t.width));
            for (uint32_t i = 0; i < (t.dimension == 2 ? 3u : 2u); i++)
            {
                const uint32_t m = t.clamp[i];  // X xenos.h:104-113: 0 repeat, 1 mirrored repeat, 2 clamp to edge, 6 clamp to border
                if (!(m <= 2 || m == 6))
                    return Refuse(where + "clamp mode " + Num(m) + " on axis " + Num(i) + " is not modelled (0, 1, 2 and 6 are)");
                if (m == 6 && t.borderColor > 1)  // X xenos.h:150-164: 2 and 3 are YCbCr blacks
                    return Refuse(where + "border colour " + Num(t.borderColor) + " is not modelled (0 black, 1 white are)");
            }
            if (t.borderSize != 0)
                return Refuse(where + "border_size 1 is not modelled");
            if (t.anisoFilter != 0)  // X xenos.h:140-148
                return Refuse(where + "anisotropic filtering " + Num(t.anisoFilter) + " is not modelled");
            if (t.magFilter > 1 || t.minFilter > 1)  // X xenos.h:131-138: 2 (base map) and 3 (fetch constant) mean nothing here
                return Refuse(where + "mag/min filter " + Num(t.magFilter) + "/" + Num(t.minFilter) + " is not point (0) or linear (1)");
            if (t.dimension == 2 && (t.volMagFilter != t.magFilter || t.volMinFilter != t.minFilter))  // shader_glsl.h Filters: one host filter for all axes
                return Refuse(where + "3D volume filters " + Num(t.volMagFilter) + "/" + Num(t.volMinFilter) + " differ from mag/min " +
                              Num(t.magFilter) + "/" + Num(t.minFilter));
            if (t.mipMinLevel > t.mipMaxLevel)
                return Refuse(where + "mip_min_level " + Num(t.mipMinLevel) + " above mip_max_level " + Num(t.mipMaxLevel));
            if (t.mipMaxLevel > t.mipMinLevel && t.mipFilter != 2)  // more than one level is sampled unless the mip filter is base map (X xenos.h:136)
                return Refuse(where + "mip levels " + Num(t.mipMinLevel) + "-" + Num(t.mipMaxLevel) + " with mip filter " + Num(t.mipFilter) +
                              ": mipmapping is not modelled (only the base level, mip filter 2)");
            for (uint32_t i = 0; i < 4; i++)  // X xenos.h XE_GPU_TEXTURE_SWIZZLE: 0-3 components, 4 zero, 5 one
                if (((t.swizzle >> (3 * i)) & 7) > 5)
                    return Refuse(where + "swizzle component " + Num((t.swizzle >> (3 * i)) & 7) + " is not modelled (X Y Z W 0 1 are)");
            const bool unsignedAll = t.sign[0] == 0 && t.sign[1] == 0 && t.sign[2] == 0 && t.sign[3] == 0;
            const bool signedAll = t.sign[0] == 1 && t.sign[1] == 1 && t.sign[2] == 1 && t.sign[3] == 1;
            const bool floatFormat = t.format == 36 || t.format == 22 || t.format == 23 || t.format == 29 || t.format == 32;
            const bool int16 = t.format == 25;
            if (int16 && !(signedAll && t.numFormat == 1))  // FR render.cpp:611-618: FM4's signed-integer 16_16 only
                return Refuse(where + "k_16_16 is modelled only as a signed integer texture (signs 1, number format 1)");
            if (!int16 && t.numFormat != 0 && !floatFormat)
                return Refuse(where + "integer number format with a normalized format is not modelled");
            if (!(unsignedAll || (t.gamma && !floatFormat) || (signedAll && (floatFormat || int16))))
                return Refuse(where + "component signs " + Num(t.sign[0]) + Num(t.sign[1]) + Num(t.sign[2]) + Num(t.sign[3]) +
                              " are not modelled (unsigned, x y z gamma, or signed float/int16)");
            d.textureSlots.push_back(t);
        }
        return true;
    }

    void Constants()
    {
        d.aluConstants.resize(2048);
        for (uint32_t i = 0; i < 2048; i++)
            d.aluConstants[i] = Reg(xr::kAluConstants + i);
        for (uint32_t i = 0; i < 8; i++)
            d.boolConstants[i] = Reg(xr::kBoolConstants + i);
        for (uint32_t i = 0; i < 32; i++)
            d.loopConstants[i] = Reg(xr::kLoopConstants + i);
        for (uint32_t l : in.vs->loopConstants)
            d.vsLoopRegisters.push_back(d.loopConstants[l & 31]);
        for (uint32_t l : in.ps->loopConstants)
            d.psLoopRegisters.push_back(d.loopConstants[l & 31]);
    }
};

}  // namespace

const char* PrimitiveTypeName(uint32_t type)
{
    switch (type)
    {
    case 0: return "none";
    case 1: return "point list";
    case 2: return "line list";
    case 3: return "line strip";
    case 4: return "triangle list";
    case 5: return "triangle fan";
    case 6: return "triangle strip";
    case 7: return "triangle with w flags";
    case 8: return "rectangle list";
    case 12: return "line loop";
    case 13: return "quad list";
    case 14: return "quad strip";
    case 15: return "polygon";
    case 16: return "2D copy rect list v0 / line patch";
    case 17: return "2D copy rect list v1 / triangle patch";
    case 18: return "2D copy rect list v2 / quad patch";
    case 19: return "2D copy rect list v3";
    case 20: return "2D fill rect list";
    case 21: return "2D line strip";
    case 22: return "2D tri strip";
    default: return "unused";
    }
}

HostViewport FoldViewport(const GuestViewport& v, uint32_t width, uint32_t height)
{
    HostViewport h;
    const float size[2] = {float(width), float(height)};
    for (uint32_t axis = 0; axis < 2; axis++)
    {
        h.scale[axis] = v.scale[axis] * 2.0f / size[axis];
        h.offset[axis] = (v.offset[axis] + v.offsetAddXy[axis]) * 2.0f / size[axis] - 1.0f;
    }
    if (v.clipDisabled)  // X draw_util.cc:371-375
    {
        h.scale[2] = v.scale[2];
        h.offset[2] = v.offset[2];
        h.zMin = 0.0f, h.zMax = 1.0f;
    }
    else
    {
        float near, far;
        if (v.dxClipSpace)  // :437-441
        {
            near = v.offset[2], far = v.offset[2] + v.scale[2];
            h.scale[2] = 1.0f, h.offset[2] = 0.0f;
        }
        else  // :442-494
        {
            near = v.offset[2] - v.scale[2], far = v.offset[2] + v.scale[2];
            h.scale[2] = 0.5f, h.offset[2] = 0.5f;
        }
        h.zMin = std::clamp(near, 0.0f, 1.0f);  // :508-509 saturate
        h.zMax = std::clamp(far, 0.0f, 1.0f);
    }
    h.scale[3] = 1.0f, h.offset[3] = 0.0f;
    return h;
}

uint32_t VertexWindowDwords(const VertexSlot& s, uint32_t highIndex)
{
    if (!s.valid)
        return 0;
    const uint64_t needed = uint64_t(highIndex) * s.stride + s.end;
    return uint32_t(std::min<uint64_t>(needed, s.sizeWords));
}

DrawState DecodeDrawState(const DrawInputs& in)
{
    DrawState d;
    if (!in.read || !in.vs || !in.ps)
    {
        d.error = "DecodeDrawState: no register reader or shader translation";
        return d;
    }
    Decoder x{in, d};
    d.edramMode = x.Reg(xr::kRbModeControl) & 7;  // X registers.h:609-616, xenos.h:903-926
    if (d.edramMode == 6)
    {
        d.kind = DrawKind::Resolve;
        return d;
    }
    if (d.edramMode != 4 && d.edramMode != 5)
    {
        x.Refuse("RB_MODECONTROL mode " + Num(d.edramMode) + " is not modelled (4 colour and depth, 5 depth, 6 copy)");
        return d;
    }
    if (!x.Packet())
        return d;
    x.DepthStencilDecode();
    d.colorWriteMask = d.edramMode == 5 ? 0 : x.Reg(xr::kRbColorMask) & 0xF;  // X registers.h:702-724; draw_util.cc:726-734
    d.kind = x.WritesNothing() ? DrawKind::NoWrite : d.edramMode == 5 ? DrawKind::DepthOnly : DrawKind::Draw;
    if (!x.Primitive())
        return d;
    if (d.kind == DrawKind::NoWrite)  // nothing reaches the GPU: the rest of the state is irrelevant and not validated
        return d;
    d.aaMask = x.Reg(xr::kPaScAaMask);
    if (!x.Raster() || !x.ScissorDecode() || !x.BlendDecode() || !x.ProgramDecode() || !x.Target() || !x.VertexFetches() || !x.TextureFetches())
        return d;
    x.Constants();
    return d;
}

}  // namespace runtime::video
