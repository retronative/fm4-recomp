// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The draw state decode (draw_state.h). "X" is Xenia xenia-canary 67f16c4e315f src/xenia/gpu/;
// "FR" is ForzaRecomp runtime/gpu/render.cpp (facts and simplifications, re-derived here).
#include "draw_state.h"
#include <cstring>
#include <algorithm>
#include <atomic>
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
        // FM4 Recomp (alpha): a draw conditional on a visibility query (token: id bits 0-5, use bit 8; X :988-992) is drawn,
        // as Xenia, which ignores the condition: every query reads visible (command_processor.cpp VIZ_QUERY), so the condition holds.
        // FM4 Recomp (alpha, Car Select: PA_SC_VIZ_QUERY 000000F9): as Xenia (pm4_command_processor_implement.h:1074-1075),
        // a draw with viz_query_ena (bit 0) and kill_pix_post_hi_z (bit 7; X registers.h:474-484) only feeds the visibility test and is
        // dropped (classified NoWrite below; VIZ_QUERY reports every query visible, command_processor.cpp); with bit 7 clear it is drawn.
        const uint32_t vizQuery = Reg(xr::kPaScVizQuery);
        vizQueryKill_ = (vizQuery & 1) && (vizQuery & 0x80);
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
    bool vizQueryKill_ = false;  // FM4 Recomp (alpha): Packet
    bool WritesNothing() const
    {
        if (vizQueryKill_)
            return true;
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
        case 5: d.hostPrimitive = HostPrimitive::TriangleFan; break;  // FM4 Recomp (alpha)
        case 2: d.hostPrimitive = HostPrimitive::LineList; break;  // FM4 Recomp: X primitive_processor.cc passes a line list through
        case 1:
            if (!noWrite)  // point sprites: PA_SU_POINT_SIZE expansion (X primitive_processor.cc:379-385 kPointListAsTriangleStrip)
                return Refuse("primitive type 1 (point list) that writes colour, depth or stencil: point sprites are not modelled (PA_SU_POINT_SIZE " +
                              Hex(Reg(xr::kPaSuPointSize)) + ", PA_SU_POINT_MINMAX " + Hex(Reg(xr::kPaSuPointMinMax)) + ")");
            d.hostPrimitive = HostPrimitive::PointNoWrite;
            break;
        default:
            return Refuse("primitive type " + Num(d.primitiveType) + " (" + PrimitiveTypeName(d.primitiveType) + ") is not modelled");
        }
        // Primitive restart (IndexBuffer::restart, draw_state.h): only a DMA-indexed strip restarts; Xenia ignores
        // multi_prim_ib_ena for lists (X primitive_processor.cc:640-646) and for auto-indexed draws (:516-573), as here.
        const uint32_t sc = Reg(xr::kPaSuScModeCntl);
        if (((sc >> 21) & 1) && d.primitiveType == 6 && d.indices.indexed)
        {
            d.indices.resetIndex = Reg(xr::kVgtMultiPrimIbResetIndx) & 0xFFFFFF;  // reset_indx: 24 bits (X registers.h:320)
            d.indices.restart = d.indices.is32Bit || d.indices.resetIndex <= 0xFFFF;  // (:635-638)
        }
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
        // poly_offset_front/back/para_enable (:430-432): X draw_util.cc:86-114; polygonal = X draw_util.h:56-74 (triangle list, fan,
        // strip, with W flags, quad list, quad strip, polygon: 4-7, 13-15; not the rect list).
        {
            const bool polygonal = PrimitivePolygonal(d.primitiveType);
            float scale = 0, offset = 0;
            if (polygonal)
            {
                if (((sc >> 11) & 1) && !d.cullFront)
                    scale = RegFloat(xr::kPaSuPolyOffsetFrontScale), offset = RegFloat(xr::kPaSuPolyOffsetFrontScale + 1);
                if (((sc >> 12) & 1) && !d.cullBack && scale == 0 && offset == 0)
                    scale = RegFloat(xr::kPaSuPolyOffsetFrontScale + 2), offset = RegFloat(xr::kPaSuPolyOffsetFrontScale + 3);
            }
            else if ((sc >> 13) & 1)
                scale = RegFloat(xr::kPaSuPolyOffsetFrontScale), offset = RegFloat(xr::kPaSuPolyOffsetFrontScale + 1);
            d.polyOffsetScale = scale, d.polyOffsetOffset = offset;
        }
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
        d.alphaTest.func = cc & 7;
        // FM4 Recomp: no alpha test in kDepth (DepthOnly): Xenia runs no pixel shader there (xenos.h:903-925 "the pixel shader
        // is never used with kDepth"; vulkan_command_processor.cc:2196-2208 sets it only for kColorDepth; draw_util.cc:121-126),
        // so RB_COLORCONTROL's test cannot discard a depth-only draw. FM4's welcome screen clears depth with a kDepth rect while
        // the UI's alpha test (8700000C: GREATER, ref 0) is still set and the clear's colour alpha is 0; testing it discarded the
        // clear. The guest PS still runs for DepthOnly here (a kill or oDepth in it
        // would still apply: APPROXIMATE against Xenia's no-fragment-stage pipeline).
        d.alphaTest.enabled = d.kind == DrawKind::Draw && ((cc >> 3) & 1) && d.alphaTest.func != 7;
        d.alphaTest.ref = RegFloat(xr::kRbAlphaRef);
        d.alphaToMask = d.kind == DrawKind::Draw && ((cc >> 4) & 1) ? (cc >> 24) | 1u << 8 : 0;
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
        const uint32_t misc = Reg(xr::kSqContextMisc);  // X registers.h:126-215
        p.scOutputScreenXy = (misc >> 1) & 1;
        p.sampleControl = (misc >> 2) & 3;
        p.paramGenPos = (misc >> 8) & 0xFF;
        const uint32_t interp = Reg(xr::kSqInterpolatorCntl);  // X registers.h:217-227
        p.paramShade = interp & 0xFFFF;
        p.samplingPattern = interp >> 16;
        // param_shade (flat shading per interpolator) is recorded, not refused: Xenia 67f16c4 never reads it (no use in
        // draw_util.cc, spirv_shader_translator.cc, vulkan_command_processor.cc, vulkan_pipeline_cache.cc,
        // texture_cache.cc, primitive_processor.cc; registers.h:220 only defines it), and FM4 sets 00010001 on every UI
        // draw (CP trace13). APPROXIMATE: a flat interpolator is
        // interpolated smoothly here, as in Xenia. Logged once (the decoder's only side effect).
        // sampling_pattern IS read by Xenia: centroid interpolation per interpolator when the target is multisampled and
        // SQ_CONTEXT_MISC sc_sample_cntl is 2 (vulkan_pipeline_cache.cc:190-195, xenos.h:862-874
        // GetInterpolatorSamplingPattern). Recorded here and not applied (APPROXIMATE; on a 1X target it has no effect).
        if (p.paramShade != 0)
        {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                std::fprintf(stderr, "[draw] SQ_INTERPOLATOR_CNTL %08X: param_shade %04X recorded, not applied (Xenia ignores it; APPROXIMATE)\n",
                             interp, p.paramShade);
        }
        const uint32_t vsc = Reg(xr::kSqVsConst), psc = Reg(xr::kSqPsConst);  // X registers.h:229-252
        p.vsConstBase = vsc & 0x1FF, p.vsConstSize = (vsc >> 12) & 0x1FF;
        p.psConstBase = psc & 0x1FF, p.psConstSize = (psc >> 12) & 0x1FF;
        // The translator binds ALU constants 0-255 to the vertex and 256-511 to the pixel shader (shader_glsl.h
        // bindings 2/3), as Xenia's backends do; a base saying otherwise is refused.
        if (p.vsConstBase != 0 || (p.psConstBase != 0 && p.psConstBase != 256))
            return Refuse("SQ_VS_CONST " + Hex(vsc) + " / SQ_PS_CONST " + Hex(psc) + ": a float constant base other than 0 / 256 is not modelled");
        // FM4 Recomp: a pixel shader reading more interpolators than the vertex shader exports reads 0 in the others (Xenia
        // zeroes them); the vertex shader's variant declares them (draw_record.cpp XE_PS_INTERPOLATORS). 16 at most.
        if (in.ps->interpolatorCount > 16)
            return Refuse("the pixel shader reads " + Num(in.ps->interpolatorCount) + " interpolators (16 at most)");
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
            // 0 8_8_8_8, 1 8_8_8_8_GAMMA, 2 2_10_10_10, 3 2_10_10_10_FLOAT, 14 32_FLOAT (X xenos.h:299-318; 14 is 32 bpp, xenos.h:323-327,
            // so the tile arithmetic below holds; FM4's HDR luminance reduction), 4 16_16 (32 bpp; FM4
            // Recomp: FM4's intro); the rest are not modelled
            // FM4 Recomp (alpha, RT format 5): 5 16_16_16_16 (X xenos.h:309-310 "Fixed point -32...32", 64 bpp: xenos.h:323-327; FM4 after a
            // race, RB_COLOR_INFO 00050000). 6, 7 and 15 stay refused.
            if (t.colorFormat > 5 && t.colorFormat != 14)
                return Refuse("RB_COLOR_INFO " + Hex(ci) + ": colour format " + Num(t.colorFormatRaw) + " is not modelled (0, 1, 2, 3, 4, 5, 10, 12, 14 are)");
            // The exponent bias (pipeline_key.h ColorExpBiasAccepted): 16_16 with 5 (the host scale 1), 16_16_16_16 with any (the pixel
            // shader scales by 2^(bias - 5): draw_record.cpp XE_COLOR0_SCALE), the others 0.
            if (t.colorFormat == 4 ? t.colorExpBias != 5 : t.colorFormat != 5 && t.colorExpBias != 0)
                return Refuse("RB_COLOR_INFO " + Hex(ci) + ": colour exponent bias " + Num(uint64_t(int64_t(t.colorExpBias))) + " is not modelled" +
                              (t.colorFormat == 4 ? " (16_16 takes 5)" : ""));
            if (t.depthWritten && t.colorBase == t.depthBase)
                return Refuse("RB_COLOR_INFO " + Hex(ci) + " / RB_DEPTH_INFO " + Hex(di) + ": colour and depth at the same EDRAM base, both written");
        }
        // Tiles (X xenos.h:467-480 GetSurfacePitchTiles): 80 samples a tile row at 32 bpp, the 4X pitch doubled; rows a tile holds:
        // 16 samples over 1 (1X) or 2 (2X, 4X) vertical samples. `pitchTiles` is the 32 bpp pitch (the depth target's, Bind's).
        // FM4 Recomp (alpha, RT format 5): a written 64 bpp colour target (format 5) has twice the tiles a row (xenos.h:474-476
        // is_64bpp, render_target_cache.h:321-323 GetPitchTiles), so its rows to the next surface are counted at that pitch. Each
        // target's rows are taken separately; for 32 bpp targets that is the former min(tiles) / pitchTiles (floor division is
        // monotonic, so the minimum commutes with it).
        const uint32_t pitchSamples = t.pitch << (t.msaa == 2 ? 1 : 0);
        t.pitchTiles = (pitchSamples + 79) / 80;
        const uint32_t rowsPerTile = 16 / (t.msaa == 0 ? 1 : 2);
        const auto limit = [](uint32_t base, uint32_t other) { return other > base ? other : 2048u; };
        const auto rows = [&](uint32_t tiles, uint32_t pitchTiles) { return tiles / pitchTiles * rowsPerTile; };
        uint32_t edramRows = rows(2048, t.pitchTiles);
        // FM4 Recomp (Photo Mode): the depth target bounds the colour target only when it is bound (X draw_util.cc:60-79
        // GetNormalizedDepthControl, render_target_cache.cc:627-636; edram.cpp Bind): stencil, or a depth test other than
        // "always, no write". Photo Mode's 64 bpp accumulation draws (depth off, depth base 720) otherwise stopped at row 352.
        const bool depthBound =
            d.depthStencil.stencilEnable || (d.depthStencil.zEnable && (d.depthStencil.zWriteEnable || d.depthStencil.zFunc != 7));
        if (t.colorWritten)
            edramRows = std::min(edramRows, rows((depthBound ? limit(t.colorBase, t.depthBase) : 2048u) - t.colorBase,
                                                 t.pitchTiles << (t.colorFormat == 5 ? 1 : 0)));
        if (t.depthWritten)
            edramRows = std::min(edramRows, rows(limit(t.depthBase, t.colorBase) - t.depthBase, t.pitchTiles));
        t.edramRows = edramRows;
        if (t.edramRows == 0)
            return Refuse("RB_SURFACE_INFO " + Hex(si) + " RB_COLOR_INFO " + Hex(ci) + " RB_DEPTH_INFO " + Hex(di) + ": the render target has no rows");
        // An empty scissor gives a target height of 0: a defined draw that covers nothing (X draw_util.h:282-283), not a refusal.
        t.targetHeight = std::min(t.edramRows, d.scissor.y + d.scissor.height);
        // FM4 Recomp: Xenia binds with min(height, EstimateMaxY) (X render_target_cache.cc:757-767,
        // draw_extent_estimator.cc:267-366). The registers it reads are recorded here; a draw whose extent needs its
        // vertices keeps the scissor bound above and the renderer lowers it (draw_record.h BoundTargetHeight), since the
        // vertex data is guest memory this pure decode does not read. The viewport bound of clipped draws is not applied
        // (unchanged behaviour; in the traced draws it never lowers a draw below its scissor).
        edram::ExtentRegisters& e = d.extent;
        e.surfaceInfo = si;
        e.windowOffset = Reg(xr::kPaScWindowOffset);
        e.windowScissorTl = Reg(xr::kPaScWindowScissorTl);
        e.windowScissorBr = Reg(xr::kPaScWindowScissorBr);
        e.screenScissorBr = Reg(xr::kPaScScreenScissorBr);
        e.clipCntl = Reg(xr::kPaClClipCntl);
        e.suScModeCntl = Reg(xr::kPaSuScModeCntl);
        e.vteCntl = Reg(xr::kPaClVteCntl);
        e.vtxCntl = Reg(xr::kPaSuVtxCntl);
        e.vportYScale = Reg(xr::kPaClVportXScale + 2);   // PA_CL_VPORT_YSCALE 0x2111
        e.vportYOffset = Reg(xr::kPaClVportXScale + 3);  // PA_CL_VPORT_YOFFSET 0x2112
        d.extentNeedsVertices = edram::NeedsVertexMaxY(e);
        return true;
    }

    // ---- Fetch constants ----
    bool VertexFetches()
    {
        const shader::ShaderTranslation& vs = *in.vs;
        d.vertexSlots.reserve(vs.vertexFetchSlots.size());  // FM4 Recomp (M7 CPU cost): one allocation at most, not one per growth step
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
            s.computedIndex = k < vs.vertexFetchComputedIndex.size() && vs.vertexFetchComputedIndex[k] != 0;
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
        case 20: case 53:          // k_DXT4_5, k_DXT4_5_AS_16_16_16_16 (FM4 Recomp)
        case 19: case 52:          // k_DXT2_3, k_DXT2_3_AS_16_16_16_16 (FM4 Recomp)
        case 7: case 54:           // k_2_10_10_10, k_2_10_10_10_AS_16_16_16_16
        case 36: case 22: case 23: // k_32_FLOAT, k_24_8, k_24_8_FLOAT
        case 10:                   // k_8_8 (FM4 Recomp: the k_8_8 resolve's destination)
        case 25:                   // k_16_16 (signed integer only)
        case 49:                   // k_DXN (FM4 Recomp)
        case 59:                   // k_DXT5A (FM4 Recomp)
        case 29: case 32:          // k_16_16_16_16_EXPAND, k_16_16_16_16_FLOAT
            return true;
        default:
            return false;
        }
    }

    bool TextureFetches()
    {
        struct Use { uint32_t slot, dimension; shader::ShaderTranslation::Filters filters; };
        // FM4 Recomp (M7 CPU cost): the thread's scratch, cleared each decode (its storage stays: an allocation and a free a draw before;
        // Decoder::TextureFetches was 3.8 % of the command thread's frees, scenario336). Nothing keeps a reference past this call.
        thread_local std::vector<Use> uses;
        uses.clear();
        uses.reserve(in.vs->textureSlots.size() + in.ps->textureSlots.size());  // the upper bound
        for (const shader::ShaderTranslation* stage : {in.vs, in.ps})
            for (size_t i = 0; i < stage->textureSlots.size(); i++)
            {
                const Use u{stage->textureSlots[i], stage->textureDimensions[i], stage->textureFilters[i]};
                auto same = std::find_if(uses.begin(), uses.end(), [&](const Use& v) { return v.slot == u.slot; });
                if (same == uses.end())
                    uses.push_back(u);
                // FM4 Recomp: 1D (0) and 2D (1) fetches of one slot share Xenia's 2D binding (spirv_shader_translator_fetch.cc:2328-2332).
                else if ((same->dimension == 0 ? 1 : same->dimension) != (u.dimension == 0 ? 1 : u.dimension) || same->filters.mag != u.filters.mag || same->filters.min != u.filters.min ||
                         same->filters.volMag != u.filters.volMag || same->filters.volMin != u.filters.volMin || same->filters.mip != u.filters.mip)
                    return Refuse("texture slot " + Num(u.slot) + " is fetched with different filters or dimensions by the vertex and pixel shaders");
            }
        std::sort(uses.begin(), uses.end(), [](const Use& a, const Use& b) { return a.slot < b.slot; });
        d.textureSlots.reserve(uses.size());  // FM4 Recomp (M7 CPU cost): one allocation at most
        for (const Use& u : uses)
        {
            TextureSlot t;
            t.slot = u.slot;
            if (t.slot >= 32)
                return Refuse("texture fetch slot " + Num(t.slot) + " is past the 32 texture fetch constants");
            for (uint32_t i = 0; i < 6; i++)
                t.words[i] = Reg(xr::kFetchConstants + 6 * t.slot + i);
            const auto where = [&] { return "texture slot " + Num(t.slot) + " (" + Hex(t.words[0]) + " " + Hex(t.words[1]) + " " + Hex(t.words[2]) + " " +
                                      Hex(t.words[3]) + " " + Hex(t.words[4]) + " " + Hex(t.words[5]) + "): "; };
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
            const uint32_t cAniso = (t.words[3] >> 25) & 7;
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
            t.anisoFilter = u.filters.aniso != 7 ? u.filters.aniso : cAniso;  // X vulkan_texture_cache.cc:671-674
            t.fetchDimension = u.dimension;
            t.gamma = t.sign[0] == 3 && t.sign[1] == 3 && t.sign[2] == 3 && t.sign[3] == 0;

            if (t.type != 2)  // X xenos.h:1124-1129; texture_cache.cc:857-889: an invalid constant binds Xenia's zero null image
            {
                t.null = true, t.gamma = false;  // the shader sees (0, 0, 0, 0) unconverted (X resets the signs to unsigned)
                for (uint32_t& s : t.sign)
                    s = 0;
                t.expAdjust = 0, t.lodBias = 0, t.anisoFilter = 0;
                d.textureSlots.push_back(t);
                continue;
            }
            if (t.physical == 0)  // X texture_cache.cc:896-899 "no texture data at all"
                return Refuse(where() + "base address 0");
            if (t.physical >= in.physicalBytes)
                return Refuse(where() + "base address past physical memory");
            if (!FormatAccepted(t.format))
                return Refuse(where() + "texture format " + Num(t.format) + " is not modelled");
            // The shader's dimension against the constant's (shader_glsl.h textureDimensions): 1 needs a 2D that is not stacked, 2 a 3D, 3 a cube.
            // FM4 Recomp: a 1D fetch (0) samples a 1D or a 2D constant at (s, 0): Xenia binds 1D and 2D fetches alike as 2D arrays
            // (spirv_shader_translator_fetch.cc:2328-2332) and builds the texture from the constant's own dimension (FM4
            // fetches a 256x1 2D constant as 1D).
            const uint32_t needed = u.dimension <= 3 ? u.dimension : 0xFF;
            const bool linearOf2D = u.dimension == 0 && t.dimension == 1;
            if (needed == 0xFF)
                return Refuse(where() + "the shader fetches dimension " + Num(u.dimension) + " (1 2D, 2 3D, 3 cube) but the constant's dimension is " +
                              Num(t.dimension) + " (0 1D, 1 2D or stacked, 2 3D, 3 cube)");
            // FM4 Recomp (alpha: a cube fetch of a 2D constant after Back): as Xenia, a constant whose
            // dimension does not fit the fetch binds the zero null image of the fetch's dimension (vulkan_texture_cache.cc:587-609
            // GetActiveBindingOrNullImageView, AreDimensionsCompatible), the same as an invalid constant above.
            if (t.dimension != needed && !linearOf2D)
            {
                t.null = true, t.gamma = false;
                for (uint32_t& s : t.sign)
                    s = 0;
                t.expAdjust = 0, t.lodBias = 0, t.anisoFilter = 0;
                d.textureSlots.push_back(t);
                continue;
            }
            if (t.stacked)
                return Refuse(where() + "a stacked (2D array) texture is not modelled");
            // A tiled 3D texture: the texture unit untiles it (texture_untile.h TiledOffset3D, Xenia texture_util.cc:461-487).
            if (t.dimension == 3 && t.width != t.height)
                return Refuse(where() + "a cube map with faces " + Num(t.width) + "x" + Num(t.height) + " is not square");
            if (t.dimension != 2 && t.pitchTexels < t.width)
                return Refuse(where() + "pitch " + Num(t.pitchTexels) + " texels is smaller than the width " + Num(t.width));
            for (uint32_t i = 0; i < (t.dimension == 2 ? 3u : 2u); i++)
            {
                const uint32_t m = t.clamp[i];  // X xenos.h:104-113: 0 repeat, 1 mirrored repeat, 2 clamp to edge, 6 clamp to border
                if (!(m <= 2 || m == 6))
                    return Refuse(where() + "clamp mode " + Num(m) + " on axis " + Num(i) + " is not modelled (0, 1, 2 and 6 are)");
                if (m == 6 && t.borderColor > 1)  // X xenos.h:150-164: 2 and 3 are YCbCr blacks
                    return Refuse(where() + "border colour " + Num(t.borderColor) + " is not modelled (0 black, 1 white are)");
            }
            if (t.borderSize != 0)
                return Refuse(where() + "border_size 1 is not modelled");
            if (t.anisoFilter > 5)  // X xenos.h:140-148: 0 off, 1-5 1:1 to 16:1; 6 undefined, 7 (use the fetch constant) in the constant
                return Refuse(where() + "anisotropic filter " + Num(t.anisoFilter) + " is undefined");
            if (t.magFilter > 1 || t.minFilter > 1)  // X xenos.h:131-138: 2 (base map) and 3 (fetch constant) mean nothing here
                return Refuse(where() + "mag/min filter " + Num(t.magFilter) + "/" + Num(t.minFilter) + " is not point (0) or linear (1)");
            if (t.dimension == 2 && (t.volMagFilter != t.magFilter || t.volMinFilter != t.minFilter))  // shader_glsl.h Filters: one host filter for all axes
                return Refuse(where() + "3D volume filters " + Num(t.volMagFilter) + "/" + Num(t.volMinFilter) + " differ from mag/min " +
                              Num(t.magFilter) + "/" + Num(t.minFilter));
            if (t.mipMinLevel > t.mipMaxLevel)
                return Refuse(where() + "mip_min_level " + Num(t.mipMinLevel) + " above mip_max_level " + Num(t.mipMaxLevel));
            // Mipmapping (mip filter 0 point / 1 linear with levels mip_min-mip_max, X xenos.h:131-138): the texture unit uploads
            // every level (texture_upload.cpp) and the sampler takes the mip mode (texture_info.cpp DescribeBinding).
            for (uint32_t i = 0; i < 4; i++)  // X xenos.h XE_GPU_TEXTURE_SWIZZLE: 0-3 components, 4 zero, 5 one
                if (((t.swizzle >> (3 * i)) & 7) > 5)
                    return Refuse(where() + "swizzle component " + Num((t.swizzle >> (3 * i)) & 7) + " is not modelled (X Y Z W 0 1 are)");
            const bool signedAll = t.sign[0] == 1 && t.sign[1] == 1 && t.sign[2] == 1 && t.sign[3] == 1;
            const bool floatFormat = t.format == 36 || t.format == 22 || t.format == 23 || t.format == 29 || t.format == 32;
            const bool int16 = t.format == 25;
            if (int16 && !(signedAll && t.numFormat == 1))  // FR render.cpp:611-618: FM4's signed-integer 16_16 only
                return Refuse(where() + "k_16_16 is modelled only as a signed integer texture (signs 1, number format 1)");
            if (!int16 && t.numFormat != 0 && !floatFormat)
                return Refuse(where() + "integer number format with a normalized format is not modelled");
            // FM4 Recomp: unsigned and unsigned-biased (2) components mixed, a normalized format: the biased ones as v * 2 - 1 on the
            // result components their swizzle selects (X spirv_shader_translator_fetch.cc:2167-2236).
            // Every component unsigned stays accepted for every format (float ones too: FM4's k_24_8_FLOAT).
            const bool unsignedAll = t.sign[0] == 0 && t.sign[1] == 0 && t.sign[2] == 0 && t.sign[3] == 0;
            bool unsignedOrBiased = !floatFormat && !int16;
            for (uint32_t i = 0; i < 4; i++)
                unsignedOrBiased = unsignedOrBiased && (t.sign[i] == 0 || t.sign[i] == 2);
            if (!(unsignedAll || unsignedOrBiased || (t.gamma && !floatFormat) || (signedAll && (floatFormat || int16))))
                return Refuse(where() + "component signs " + Num(t.sign[0]) + Num(t.sign[1]) + Num(t.sign[2]) + Num(t.sign[3]) +
                              " are not modelled (unsigned or unsigned biased, x y z gamma, or signed float/int16)");
            for (uint32_t i = 0; i < 4; i++)
                if (const uint32_t g = (t.swizzle >> (3 * i)) & 7; g < 4 && t.sign[g] == 2)
                    t.biasedMask |= 1u << i;
            d.textureSlots.push_back(t);
        }
        return true;
    }

    void Constants()
    {
        if (in.constantsScratch != nullptr)
            d.aluConstants.swap(*in.constantsScratch);  // FM4 Recomp: the caller's scratch storage (DrawInputs::constantsScratch)
        d.aluConstants.resize(2048);
        d.vsConstantWords = std::min<uint32_t>(in.vs->constantCount, 256) * 4, d.psConstantWords = std::min<uint32_t>(in.ps->constantCount, 256) * 4;
        if (in.registers != nullptr)  // DrawInputs::registers: the same words, byte-swapped in bulk
        {
            const auto bulk = [&](uint32_t first, uint32_t count, uint32_t* out) {
                for (uint32_t i = 0; i < count; i++)
                {
                    uint32_t raw;
                    std::memcpy(&raw, in.registers + 4 * size_t(first + i), 4);
                    out[i] = __builtin_bswap32(raw);
                }
            };
            // FM4 Recomp (M7 CPU cost): only the words the shaders can read (the rest of the vector is stale and never uploaded).
            bulk(xr::kAluConstants, d.vsConstantWords, d.aluConstants.data());
            bulk(xr::kAluConstants + 1024, d.psConstantWords, d.aluConstants.data() + 1024);
            bulk(xr::kBoolConstants, 8, d.boolConstants);
            bulk(xr::kLoopConstants, 32, d.loopConstants);
        }
        else
        {
            for (uint32_t i = 0; i < d.vsConstantWords; i++)
                d.aluConstants[i] = Reg(xr::kAluConstants + i);
            for (uint32_t i = 0; i < d.psConstantWords; i++)
                d.aluConstants[1024 + i] = Reg(xr::kAluConstants + 1024 + i);
            for (uint32_t i = 0; i < 8; i++)
                d.boolConstants[i] = Reg(xr::kBoolConstants + i);
            for (uint32_t i = 0; i < 32; i++)
                d.loopConstants[i] = Reg(xr::kLoopConstants + i);
        }
        d.vsLoopRegisters.reserve(in.vs->loopConstants.size()), d.psLoopRegisters.reserve(in.ps->loopConstants.size());  // FM4 Recomp (M7 CPU cost)
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
    // Xenia loads the guest words at the fetched index without bounding them by the constant's size (X
    // spirv_shader_translator_fetch.cc:126-131, "Bound checking is not done here"); FM4 draws reach past it
    // (indices up to 23 at stride 4 on a 12-dword constant). The window is what the indices reach.
    uint64_t needed = uint64_t(highIndex) * s.stride + s.end;
    // FM4 Recomp: a computed-index fetch reads where the shader says: the window holds at least the constant's whole declared data
    // (Xenia reads guest memory directly; FM4's vs_2419997a1464c2fd slot 94 indexed by r0.w).
    if (s.computedIndex)
        needed = std::max<uint64_t>(needed, s.sizeWords);
    return uint32_t(std::min<uint64_t>(needed, UINT32_MAX));
}

FetchProof VertexFetchProof(const DrawState& d, uint32_t highIndex)
{
    if (d.vertexSlots.empty())
        return FetchProof::NoSlots;
    for (const VertexSlot& s : d.vertexSlots)
    {
        if (s.computedIndex)
            return FetchProof::Computed;
        if (!s.valid || s.sizeWords == 0 || (s.stride == 0 && s.end == 0))
            return FetchProof::Invalid;
        if (VertexWindowDwords(s, highIndex) > s.sizeWords)
            return FetchProof::Past;
    }
    return FetchProof::Inside;
}
bool VertexFetchProvablyInside(const DrawState& d, uint32_t highIndex) { return VertexFetchProof(d, highIndex) == FetchProof::Inside; }

void DecodeDrawState(const DrawInputs& in, DrawState& out)
{
    // FM4 Recomp (M7 CPU cost): `out` is reset to a fresh DrawState but keeps its vectors' storage (moved out, cleared, moved back);
    // aluConstants is kept whole (Constants() overwrites every word; the other kinds never read it).
    std::vector<VertexSlot> vertexSlots = std::move(out.vertexSlots);
    std::vector<TextureSlot> textureSlots = std::move(out.textureSlots);
    std::vector<uint32_t> aluConstants = std::move(out.aluConstants), vsLoops = std::move(out.vsLoopRegisters), psLoops = std::move(out.psLoopRegisters);
    out = DrawState{};
    vertexSlots.clear(), textureSlots.clear(), vsLoops.clear(), psLoops.clear();
    out.vertexSlots = std::move(vertexSlots), out.textureSlots = std::move(textureSlots), out.aluConstants = std::move(aluConstants);
    out.vsLoopRegisters = std::move(vsLoops), out.psLoopRegisters = std::move(psLoops);
    DrawState& d = out;
    if (!in.read || !in.vs || !in.ps)
    {
        d.error = "DecodeDrawState: no register reader or shader translation";
        return;
    }
    Decoder x{in, d};
    d.edramMode = x.Reg(xr::kRbModeControl) & 7;  // X registers.h:609-616, xenos.h:903-926
    if (d.edramMode == 6)
    {
        d.kind = DrawKind::Resolve;
        return;
    }
    if (d.edramMode != 4 && d.edramMode != 5)
    {
        x.Refuse("RB_MODECONTROL mode " + Num(d.edramMode) + " is not modelled (4 colour and depth, 5 depth, 6 copy)");
        return;
    }
    if (!x.Packet())
        return;
    x.DepthStencilDecode();
    d.colorWriteMask = d.edramMode == 5 ? 0 : x.Reg(xr::kRbColorMask) & 0xF;  // X registers.h:702-724; draw_util.cc:726-734
    d.kind = x.WritesNothing() ? DrawKind::NoWrite : d.edramMode == 5 ? DrawKind::DepthOnly : DrawKind::Draw;
    if (!x.Primitive())
        return;
    if (d.kind == DrawKind::NoWrite)  // nothing reaches the GPU: the rest of the state is irrelevant and not validated
        return;
    d.aaMask = x.Reg(xr::kPaScAaMask);
    if (!x.Raster() || !x.ScissorDecode() || !x.BlendDecode() || !x.ProgramDecode() || !x.Target() || !x.VertexFetches() || !x.TextureFetches())
        return;
    x.Constants();
}

DrawState DecodeDrawState(const DrawInputs& in)
{
    DrawState d;
    DecodeDrawState(in, d);
    return d;
}

}  // namespace runtime::video
