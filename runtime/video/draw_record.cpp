// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See draw_record.h.
#include "draw_record.h"
#include "host_samples.h"  // FM4 Recomp (FM4_HOST_MSAA_1X): HostSampleCount, HostMsaa1x
#include "render_scale.h"  // FM4 Recomp (M9, FM4_INTERNAL_RES): InternalHalvesQuiet
#include "edram.h"         // FM4 Recomp (M9, FM4_INTERNAL_SCALE_PITCH): PitchTilesAt32bpp

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace runtime::video {

namespace {

std::string Num(uint64_t v) { return std::to_string(v); }

std::string FloatLiteral(float f)
{
    char s[32];
    std::snprintf(s, sizeof s, "%.9g", double(f));
    std::string t = s;
    if (t.find_first_of(".eEn") == std::string::npos)
        t += ".0";
    return t;
}

bool Lists(const CachedShader& shader, uint32_t slot)
{
    return std::find(shader.t.textureSlots.begin(), shader.t.textureSlots.end(), slot) != shader.t.textureSlots.end();
}

}  // namespace

PipelineKey SubsetMergeKey(const PipelineKey& colorOnly, uint8_t depthFormat)
{
    PipelineKey k = colorOnly;
    k.depthUsed = 1, k.depthFormat = depthFormat, k.depthWrite = 0, k.depthFunc = 7, k.depthBias = 0, k.stencilEnable = 0;
    std::memset(k.stencilFront, 0, sizeof k.stencilFront), std::memset(k.stencilBack, 0, sizeof k.stencilBack);
    return k;
}

PipelineKey SubsetMergeKeyDepthOnly(const PipelineKey& depthOnly, uint8_t colorFormat)
{
    PipelineKey k = depthOnly;
    k.colorUsed = 1;
    k.color[0] = PipelineKeyColor{};
    k.color[0].format = colorFormat, k.color[0].writeMask = 0;
    k.color[0].srcColor = 1, k.color[0].dstColor = 0, k.color[0].colorOp = 0, k.color[0].srcAlpha = 1, k.color[0].dstAlpha = 0, k.color[0].alphaOp = 0;
    return k;
}

KeyResult KeyFromDrawState(const DrawState& d, const CachedShader& vs, const CachedShader& ps, uint64_t vsVariant, uint64_t psVariant, PipelineKey& out)
{
    if (d.kind != DrawKind::Draw && d.kind != DrawKind::DepthOnly)
        return Refuse("a draw of kind " + Num(uint32_t(d.kind)) + " has no pipeline (Draw 2, DepthOnly 3 do)");
    PipelineKey k;
    std::memset(&k, 0, sizeof k);
    k.vsHash = vs.hash, k.psHash = ps.hash, k.vsVariant = vsVariant, k.psVariant = psVariant;
    switch (d.hostPrimitive)
    {
    case HostPrimitive::TriangleList: case HostPrimitive::RectList: case HostPrimitive::QuadList: case HostPrimitive::TriangleFan:
        k.topology = uint8_t(Topology::TriangleList);
        break;
    case HostPrimitive::TriangleStrip: k.topology = uint8_t(Topology::TriangleStrip); break;
    case HostPrimitive::LineList: k.topology = uint8_t(Topology::LineList); break;
    case HostPrimitive::PointNoWrite: return Refuse("a point list that writes nothing has no pipeline");
    }
    k.primitiveRestart = uint8_t(d.indices.restart);  // a DMA-indexed strip with multi_prim_ib_ena (draw_state.h IndexBuffer::restart)
    k.cullFront = d.cullFront, k.cullBack = d.cullBack, k.frontClockwise = d.frontClockwise;  // every host primitive is polygonal
    k.depthClamp = d.viewport.clipDisabled;
    if (d.target.samples != 1 && d.target.samples != 2 && d.target.samples != 4)
        return Refuse("RB_SURFACE_INFO: " + Num(d.target.samples) + " samples");
    // FM4 Recomp (FM4_HOST_MSAA_1X=1): the host image's sample count (edram_vk.cpp GetTarget derives it the same way; renderer.cpp
    // stops on a mismatch with HostTargets::samples), not the guest's: rasterizationSamples must equal the attachments'.
    k.samples = uint8_t(HostSampleCount(d.target.samples, HostMsaa1x()));
    const DepthStencil& s = d.depthStencil;
    if (s.zEnable || s.stencilEnable)
    {
        k.depthUsed = 1;
        k.depthFormat = uint8_t(d.target.depthFormat);
        k.depthWrite = uint8_t(s.zEnable && s.zWriteEnable);
        k.depthFunc = uint8_t(s.zEnable ? s.zFunc : 7);
        if (s.stencilEnable)
        {
            k.stencilEnable = 1;
            k.stencilFront[0] = uint8_t(s.front.fail), k.stencilFront[1] = uint8_t(s.front.zPass), k.stencilFront[2] = uint8_t(s.front.zFail);
            k.stencilFront[3] = uint8_t(s.front.func);
            k.stencilBack[0] = uint8_t(s.back.fail), k.stencilBack[1] = uint8_t(s.back.zPass), k.stencilBack[2] = uint8_t(s.back.zFail);
            k.stencilBack[3] = uint8_t(s.back.func);
        }
    }
    if (d.kind == DrawKind::Draw && d.colorWriteMask != 0)
    {
        k.colorUsed = 1;
        PipelineKeyColor& c = k.color[0];
        c.format = uint8_t(d.target.colorFormat);  // the storage format (10 -> 2, 12 -> 3: draw_state.cpp Target)
        if (!ColorFormatMapped(c.format))
            return Refuse("colour format " + Num(c.format) + " is not mapped on the host");
        c.writeMask = uint8_t(d.colorWriteMask);
        if (d.blend.enabled)
        {
            c.srcColor = uint8_t(d.blend.srcColor), c.dstColor = uint8_t(d.blend.dstColor), c.colorOp = uint8_t(d.blend.colorOp);
            c.srcAlpha = uint8_t(d.blend.srcAlpha), c.dstAlpha = uint8_t(d.blend.dstAlpha), c.alphaOp = uint8_t(d.blend.alphaOp);
        }
        else
            c.srcColor = 1, c.dstColor = 0, c.colorOp = 0, c.srcAlpha = 1, c.dstAlpha = 0, c.alphaOp = 0;
    }
    if (!k.colorUsed && !k.depthUsed)
        return Refuse("a draw with neither a colour write nor a depth or stencil test has no attachment");
    // FM4 Recomp: depth bias on with a depth attachment and a non-zero offset (X vulkan_pipeline_cache.cc:2083-2092 enables it with
    // any depth attachment; the values are dynamic, HostDepthBias).
    k.depthBias = uint8_t(k.depthUsed && (d.polyOffsetScale != 0 || d.polyOffsetOffset != 0));
    out = k;
    return Accept();
}

HostDepthBias DepthBiasFor(const DrawState& d)
{
    // X vulkan_command_processor.cc:3276-3297: the constant factor times 2^24 - 1 for kD24S8 (draw_util.h:180-181) or 2^24 for
    // kD24FS8 (:212-213); the slope times 1/16 (xenos.h:448 kPolygonOffsetScaleSubpixelUnit; resolution scale 1).
    HostDepthBias b;
    b.constant = d.polyOffsetOffset * (d.target.depthFormat == 0 ? float((1u << 24) - 1) : float(1u << 24));
    b.slope = d.polyOffsetScale * (1.0f / 16.0f);
    return b;
}

KeyResult VariantsFromDrawState(const DrawState& d, const CachedShader& vs, const CachedShader& ps, ShaderVariantDefines& vsDefines,
                                ShaderVariantDefines& psDefines)
{
    // FM4 Recomp (M7 CPU cost): cleared, not reassigned, so a caller's reused defines keep their vectors' storage (`= {}` freed and
    // reallocated both every draw).
    vsDefines.lines.clear(), psDefines.lines.clear();
    if (d.hostPrimitive == HostPrimitive::RectList)
        vsDefines.Add("XE_RECT_LIST");
    // FM4 Recomp: the pixel shader reads interpolators the vertex shader does not export: they are declared and written as 0
    // (shader_glsl.cpp XE_PS_INTERPOLATORS; Xenia zeroes unwritten interpolators).
    if (ps.t.interpolatorCount > vs.t.interpolatorCount)
        vsDefines.Add("XE_PS_INTERPOLATORS " + Num(ps.t.interpolatorCount));
    for (const TextureSlot& t : d.textureSlots)
    {
        const std::string name = "XE_TEXTURE_" + Num(t.slot);
        for (ShaderVariantDefines* defines : {Lists(vs, t.slot) ? &vsDefines : nullptr, Lists(ps, t.slot) ? &psDefines : nullptr})
        {
            if (defines == nullptr)
                continue;
            if (t.gamma)
                defines->Add(name + "_GAMMA");
            if (t.biasedMask != 0)
            {
                std::string mask = "bvec4(";
                for (uint32_t i = 0; i < 4; i++)
                    mask += std::string((t.biasedMask >> i) & 1 ? "true" : "false") + (i < 3 ? ", " : ")");
                defines->Add(name + "_BIASED " + mask);
            }
            // FM4 Recomp (the rectangular car shadow): a signed-integer 16_16 fetch (number format 1, every
            // sign 1: draw_state.cpp accepts no other 16_16) holds each channel as the int16 itself times 2^exp_adjust, not normalized
            // (the consumer side of the resolve's copy_dest_number 3 packing, edram_vk_plan.cpp PackFixed16; ForzaRecomp render.cpp:620-626,
            // INFERRED there too). The view is R16G16_SNORM (int16 / 32767), so 32767 restores the integer exactly for -32767..32767
            // (-32768 reads -32767), and filtering commutes with the scale. FM4's car-shadow mask (1C1E3000).
            const bool int16Scale = t.format == 25 && t.numFormat == 1;
            if (t.expAdjust != 0 || int16Scale)
                defines->Add(name + "_SCALE " + std::string(int16Scale ? "32767.0 * " : "") + "ldexp(1.0, " + std::to_string(t.expAdjust) + ")");
            if (t.lodBias != 0)
                defines->Add(name + "_LOD_BIAS " + FloatLiteral(float(t.lodBias) / 32.0f));
        }
    }
    if (d.alphaTest.enabled)
    {
        psDefines.Add("XE_ALPHA_TEST " + Num(d.alphaTest.func));
        psDefines.Add("XE_ALPHA_REF " + FloatLiteral(d.alphaTest.ref));
    }
    // FM4 Recomp: PsParamGen (X shader_translator.cc:329-347: param_gen with param_gen_pos below the interpolator count; the
    // register then holds the pixel parameters, spirv_shader_translator.cc:2016-2135). Positions 16 and up are never interpolators.
    if (d.program.paramGen && d.program.paramGenPos < 16)
    {
        psDefines.Add("XE_PARAM_GEN " + Num(d.program.paramGenPos));
        psDefines.Add(std::string("XE_PARAM_GEN_POLYGONAL ") + (PrimitivePolygonal(d.primitiveType) ? "1" : "0"));
        if (d.hostPrimitive == HostPrimitive::LineList)
            psDefines.Add("XE_PARAM_GEN_LINE 1");  // z's sign set: X spirv_shader_translator.cc:2107-2120
    }
    // FM4 Recomp (M9, FM4_INTERNAL_RES): the variants that read the pixel position take it in guest pixels (render_scale.h
    // ScaleFragCoordReads, applied by shader_cache.cpp VariantGlsl); the define also keeps their cached SPIR-V apart from 1x's.
    // (FM4_INTERNAL_SCALE_PITCH: the scale of this draw's targets, by their pitch.)
    if (const uint32_t halves = runtime::video::scale::HalvesForPitch(edram::PitchTilesAt32bpp(d.target.pitch, edram::Msaa(d.target.msaa)),
                                                                      runtime::video::scale::InternalHalvesQuiet(), runtime::video::scale::PitchScaled());
        halves > runtime::video::scale::kHalves1x && ((d.program.paramGen && d.program.paramGenPos < 16) || d.alphaToMask != 0))
        psDefines.Add("XE_HALVES " + Num(halves) + "u");
    if (d.alphaToMask != 0)  // the offsets and the target's sample count (shader_glsl.cpp XE_ALPHA_TO_MASK)
    {
        psDefines.Add("XE_ALPHA_TO_MASK " + Num(d.alphaToMask & 0xFF) + "u");
        // FM4 Recomp (FM4_HOST_MSAA_1X=1): the host count (KeyFromDrawState's k.samples): a 1-sample target takes the 1x coverage
        // rule and writes no gl_SampleMask (shader_glsl.cpp XE_ALPHA_TO_MASK).
        psDefines.Add("XE_ALPHA_TO_MASK_SAMPLES " + Num(HostSampleCount(d.target.samples, HostMsaa1x())));
    }
    if (d.kind == DrawKind::Draw && d.colorWriteMask != 0)
    {
        if (d.target.colorFormat == 1)
            psDefines.Add("XE_COLOR0_GAMMA");
        else if (d.target.colorFormat == 3)
            psDefines.Add("XE_COLOR0_7E3");
        // FM4 Recomp (alpha, RT format 5): a 16_16_16_16 target (R16G16B16A16_SNORM) holds the guest's -32..32 as -1..1: colour 0
        // times 2^(bias - 5) after the alpha test (X vulkan_command_processor.cc:3757-3774 color_exp_bias -= 5 on the host-render-
        // target path with snorm16_render_target_full_range, render_target_cache.cc:188; spirv_shader_translator_rb.cc:831). Bias 0
        // (FM4's) is 1/32; bias 5 is 1 and needs no define (as 16_16's, which accepts only 5).
        else if (d.target.colorFormat == 5 && d.target.colorExpBias != 5)
            psDefines.Add("XE_COLOR0_SCALE " + FloatLiteral(std::ldexp(1.0f, d.target.colorExpBias - 5)));
    }
    else
        psDefines.Add("XE_NO_COLOR_OUTPUT");  // no colour attachment (the key's colorUsed is 0): the output is not declared
    return Accept();
}

// FM4 Recomp (M7 CPU cost, FM4_VARIANT_MEMO; draw_record.h): every input VariantsFromDrawState reads above, raw (a value it ignores
// only makes the memo miss more, never hit wrongly). The shader objects stand for what it reads of the pair (interpolator counts, the
// texture slots each lists: a translation's, fixed for the object) and for the variants ShaderCache::Variant returns (per shader).
// Of the draw: hostPrimitive, the kind, the primitive type (PrimitivePolygonal), the renderer's XE_FETCH_UNCHECKED decision
// (renderer.cpp adds that define after this function), the alpha test (ref as its bits: equal bits print one FloatLiteral),
// param_gen and its position, alpha to mask, the sample count, the colour mask, the target's colour format and exp bias, and each
// texture slot's slot, gamma, biasedMask, format, numFormat, expAdjust and lodBias.
bool VariantMemoKeyFor(const DrawState& d, const CachedShader& vs, const CachedShader& ps, bool fetchUnchecked, VariantMemoKey& out)
{
    if (d.textureSlots.size() > VariantMemoKey::kMaxTextures)
        return false;
    uint32_t n = 0;
    const auto put = [&](uint32_t v) { out.words[n++] = v; };
    const auto put64 = [&](uint64_t v) { put(uint32_t(v)), put(uint32_t(v >> 32)); };
    put64(uint64_t(reinterpret_cast<uintptr_t>(&vs)));
    put64(uint64_t(reinterpret_cast<uintptr_t>(&ps)));
    put(uint32_t(d.kind)), put(uint32_t(d.hostPrimitive)), put(d.primitiveType), put(fetchUnchecked ? 1u : 0u);
    uint32_t refBits = 0;
    std::memcpy(&refBits, &d.alphaTest.ref, sizeof refBits);
    put(d.alphaTest.enabled ? 1u : 0u), put(d.alphaTest.func);
    put(refBits);
    put(d.program.paramGen ? 1u : 0u), put(d.program.paramGenPos);
    put(d.alphaToMask), put(d.target.samples), put(d.colorWriteMask), put(d.target.colorFormat);
    put(uint32_t(d.target.colorExpBias));
    put(uint32_t(d.textureSlots.size()));
    for (const TextureSlot& t : d.textureSlots)
    {
        put(t.slot);
        put(t.gamma ? 1u : 0u), put(t.biasedMask), put(t.format), put(t.numFormat), put(uint32_t(t.expAdjust));
        put(uint32_t(t.lodBias));
    }
    out.count = n;
    return true;
}

uint32_t SwapIndex(uint32_t raw, bool is32Bit, uint32_t endian)
{
    if (is32Bit)
        return endian == 2 ? __builtin_bswap32(raw) : raw;  // 8in32
    return endian == 1 ? uint32_t(__builtin_bswap16(uint16_t(raw))) : raw & 0xFFFF;  // 8in16
}

KeyResult BuildHostIndices(const DrawState& d, GuestIndexReader read, const void* context, HostIndices& out)
{
    out = {};
    const uint32_t count = d.indices.count;
    if (count == 0)
        return Refuse("a draw of 0 indices");
    const IndexBuffer& ib = d.indices;
    if (ib.indexed)
    {
        if (read == nullptr)
            return Refuse("an indexed draw needs a guest index reader");
        if (ib.is32Bit ? (ib.endian != 0 && ib.endian != 2) : (ib.endian != 0 && ib.endian != 1))
            return Refuse("index buffer endian " + Num(ib.endian) + " with " + (ib.is32Bit ? "32" : "16") + "-bit indices is not modelled");
        if (uint64_t(count) * (ib.is32Bit ? 4 : 2) > ib.bytes)
            return Refuse("the index buffer holds " + Num(ib.bytes) + " bytes, fewer than " + Num(count) + " " + (ib.is32Bit ? "32" : "16") + "-bit indices");
    }
    // Primitive restart (draw_state.h IndexBuffer::restart; X primitive_processor.cc:829-950): an element whose swapped
    // value (16-bit: whole; 32-bit: low 24 bits) equals the reset index, before the offset and the clamp, becomes the
    // host's 0xFFFFFFFF (Vulkan's restart value for 32-bit indices; X ReplaceResetIndex32To24, primitive_processor.h:
    // 422-434, and ReplaceResetIndex16To24). No other host index can be 0xFFFFFFFF: the rest are masked to 24 bits.
    constexpr uint32_t kHostRestart = 0xFFFFFFFFu;
    if (ib.restart && d.hostPrimitive != HostPrimitive::TriangleStrip)
        return Refuse("primitive restart on a host primitive other than a triangle strip");
    const auto guest = [&](uint32_t i) {
        const uint32_t raw = ib.indexed ? SwapIndex(read(context, i), ib.is32Bit, ib.endian) : i;
        if (ib.restart && (raw & 0xFFFFFFu) == ib.resetIndex)
            return kHostRestart;
        return std::clamp((raw + d.indexOffset) & 0xFFFFFFu, d.minIndex, d.maxIndex);
    };
    const bool identity = !ib.indexed && d.indexOffset == 0 && d.minIndex == 0 && d.maxIndex >= count - 1;
    switch (d.hostPrimitive)
    {
    case HostPrimitive::RectList:
        if (count % 3 != 0)
            return Refuse("a rect list of " + Num(count) + " indices is not whole rects (3 each)");
        out.kind = HostIndices::Kind::RectList;
        out.indices.reserve(count);
        for (uint32_t i = 0; i < count; i++)
            out.indices.push_back(guest(i));
        out.vertexCount = count / 3 * 6;
        break;
    case HostPrimitive::QuadList:
        if (count % 4 != 0)
            return Refuse("a quad list of " + Num(count) + " indices is not whole quads (4 each)");
        out.kind = HostIndices::Kind::Indexed;
        out.indices.reserve(count / 4 * 6);
        for (uint32_t q = 0; q < count; q += 4)
        {
            const uint32_t v[4] = {guest(q), guest(q + 1), guest(q + 2), guest(q + 3)};
            for (const uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u})
                out.indices.push_back(v[k]);
        }
        out.vertexCount = count / 4 * 6;
        break;
    case HostPrimitive::TriangleFan:
        // FM4 Recomp (alpha): a fan as a triangle list in Xenia's order, (previous, current, first) for i = 2.. (ShiftGlue 72b7dd6f
        // include/rex/graphics/primitive_processor.h:494-512 TriangleFanToList, BSD-style, Xenia-derived; a rotation of (first,
        // previous, current), so the winding is the fan's). Fewer than 3 indices: no triangle (GetTriangleFanListIndexCount).
        out.kind = HostIndices::Kind::Indexed;
        out.indices.reserve(count > 2 ? (count - 2) * 3 : 0);
        for (uint32_t i = 2; i < count; i++)
            for (const uint32_t v : {guest(i - 1), guest(i), guest(0)})
                out.indices.push_back(v);
        out.vertexCount = count > 2 ? (count - 2) * 3 : 0;
        break;
    case HostPrimitive::TriangleList: case HostPrimitive::TriangleStrip: case HostPrimitive::LineList:
        if (identity)
        {
            out.kind = HostIndices::Kind::Direct;
            out.vertexCount = count;
            out.highIndex = count - 1;
            return Accept();
        }
        out.kind = HostIndices::Kind::Indexed;
        out.indices.reserve(count);
        for (uint32_t i = 0; i < count; i++)
            out.indices.push_back(guest(i));
        out.vertexCount = count;
        break;
    case HostPrimitive::PointNoWrite:
        return Refuse("a point list that writes nothing is not drawn");
    }
    out.highIndex = 0;  // the largest real index (a draw made only of reset indices assembles nothing; its window is index 0)
    for (const uint32_t index : out.indices)
        if (index != kHostRestart)
            out.highIndex = std::max(out.highIndex, index);
    return Accept();
}

void PlanVertexWindows(const DrawState& d, uint32_t highIndex, std::vector<VertexWindow>& out, uint32_t& totalDwords)
{
    out.clear();
    totalDwords = 0;
    for (const VertexSlot& s : d.vertexSlots)
    {
        VertexWindow w;
        w.slot = s.slot, w.physical = s.physical, w.endian = s.endian;
        w.dwords = VertexWindowDwords(s, highIndex);
        w.baseDword = totalDwords;
        totalDwords += w.dwords;
        out.push_back(w);
    }
}

void FillXeSystem(const std::vector<VertexWindow>& windows, const HostViewport& viewport, uint32_t vteFlags, uint8_t out[kSystemBytes])
{
    std::memset(out, 0, kSystemBytes);
    uint32_t* fetch = reinterpret_cast<uint32_t*>(out + kSystemFetchOffset);
    uint32_t* bases = reinterpret_cast<uint32_t*>(out + kSystemVertexBaseOffset);
    for (const VertexWindow& w : windows)
    {
        bases[w.slot] = w.baseDword;
        if (w.dwords == 0)
            continue;  // an invalid or empty constant stays type 0 / size 0: an executed fetch of it fails the shader's bounds check
        fetch[2 * w.slot] = w.physical | 3u;
        fetch[2 * w.slot + 1] = w.dwords << 2 | (w.endian & 3);
    }
    std::memcpy(out + kSystemViewportScaleOffset, viewport.scale, 16);
    std::memcpy(out + kSystemViewportOffsetOffset, viewport.offset, 16);
    std::memcpy(out + kSystemFlagsOffset, &vteFlags, 4);
}

void FillConstants(const DrawState& d, bool pixel, uint8_t out[kConstantsBytes])
{
    // FM4 Recomp (M7 CPU cost): only the words the stage's shader can read are written (DrawState::vsConstantWords / psConstantWords; the
    // block beyond them is never read), then the bool/loop tail.
    const uint32_t words = std::min<uint32_t>(pixel ? d.psConstantWords : d.vsConstantWords, 1024);
    if (d.aluConstants.size() >= 2048)
        std::memcpy(out + kConstantsAluOffset, d.aluConstants.data() + (pixel ? 1024 : 0), size_t(words) * 4), std::memset(out + 4096, 0, kConstantsBytes - 4096);
    else
        std::memset(out, 0, kConstantsBytes);
    std::memcpy(out + kConstantsBoolOffset, d.boolConstants, sizeof d.boolConstants);
    std::memcpy(out + kConstantsLoopOffset, d.loopConstants, sizeof d.loopConstants);
}

bool SameStageConstants(const DrawState& d, bool pixel, uint32_t lastWords, const std::vector<uint32_t>& lastAlu, const uint32_t lastBools[8],
                        const uint32_t lastLoops[32])
{
    const uint32_t words = pixel ? d.psConstantWords : d.vsConstantWords;
    if (d.aluConstants.size() < 2048 || lastAlu.size() < 2048 || words != lastWords || words > 1024)
        return false;
    const size_t first = pixel ? 1024 : 0;
    return std::memcmp(d.aluConstants.data() + first, lastAlu.data() + first, size_t(words) * 4) == 0 &&
           std::memcmp(d.boolConstants, lastBools, sizeof d.boolConstants) == 0 && std::memcmp(d.loopConstants, lastLoops, sizeof d.loopConstants) == 0;
}

bool BoundTargetHeight(const DrawState& d, const shader::ShaderTranslation& vs, const uint32_t* window, size_t windowDwords, uint32_t endian,
                       const HostIndices& indices, uint32_t& height, std::string& note)
{
    height = d.target.targetHeight;
    note.clear();
    if (!d.extentNeedsVertices)
        return false;
    const shader::ShaderTranslation::PositionFetch& p = vs.positionFetch;
    if (!p.valid)
        return note = "the extent needs the vertices (clipping off, 8192 scissor) but the vertex shader's position is not a plain vertex fetch: " + p.why,
               false;
    if (window == nullptr)
        return note = "the extent needs the vertices but vertex fetch slot " + std::to_string(p.slot) + " has no data window", false;
    std::vector<edram::VertexPosition> positions, one;
    std::string why;
    const auto add = [&](uint32_t index) {
        const size_t at = size_t(index) * p.strideDwords;
        if (at >= windowDwords || !edram::PassThroughPositions(window + at, windowDwords - at, endian, p.strideDwords, p.offsetDwords, 1, one, why))
        {
            note = "the extent needs vertex " + std::to_string(index) + " of slot " + std::to_string(p.slot) + ", outside its " +
                   std::to_string(windowDwords) + "-dword window" + (why.empty() ? std::string() : ": " + why);
            return false;
        }
        positions.push_back(one[0]);
        return true;
    };
    if (indices.kind == HostIndices::Kind::Direct)
    {
        for (uint32_t i = 0; i < indices.vertexCount; i++)
            if (!add(i))
                return false;
    }
    else
        for (const uint32_t index : indices.indices)
            if (!add(index))
                return false;
    const uint32_t vertexMaxY = edram::VertexMaxY(d.extent, positions.data(), positions.size());
    height = std::min(d.target.edramRows, edram::EstimateMaxY(d.extent, &vertexMaxY));
    return true;
}

OverwriteRect FindOverwriteRect(const DrawState& d, const shader::ShaderTranslation& vs, bool psKills, const uint32_t* window, size_t windowDwords,
                                uint32_t endian)
{
    OverwriteRect o;
    const auto no = [&](std::string why) { o.why = std::move(why); return o; };
    // FM4 Recomp (M7 GPU cost): DepthOnly (RB_MODECONTROL 5) rasterizes and writes depth exactly as Draw with every colour write off; FM4's
    // depth clears of the 4x scene depth are this kind (720 tiles were copied in twice a swap just before it).
    if ((d.kind != DrawKind::Draw && d.kind != DrawKind::DepthOnly) || d.primitiveType != 8 || d.indices.indexed || d.indices.count != 3)
        return no("not a rect-list draw of three auto-indexed vertices");
    if (d.cullFront || d.cullBack)
        return no("culling is on");
    const GuestViewport& v = d.viewport;
    if (!(d.systemFlags & shader::kFlagXyDividedByW) || v.scale[0] != 1.0f || v.scale[1] != 1.0f || v.offset[0] != 0.0f || v.offset[1] != 0.0f)
        return no("the positions are not screen space (vtx_xy_fmt with no viewport scale or offset)");
    const shader::ShaderTranslation::PositionFetch& p = vs.positionFetch;
    if (!p.valid)
        return no("the vertex shader's position is not a plain vertex fetch: " + p.why);
    if (window == nullptr)
        return no("vertex fetch slot " + std::to_string(p.slot) + " has no data window");
    std::vector<edram::VertexPosition> positions;
    std::string why;
    if (!edram::PassThroughPositions(window, windowDwords, endian, p.strideDwords, p.offsetDwords, 3, positions, why))
        return no(why);
    float sx[3], sy[3];
    for (uint32_t i = 0; i < 3; i++)
        sx[i] = positions[i].x + v.offsetAddXy[0], sy[i] = positions[i].y + v.offsetAddXy[1];
    for (float f : {sx[0], sx[1], sx[2], sy[0], sy[1], sy[2]})
        if (!(f == f) || f < -16384.0f || f > 16384.0f)
            return no("a position is not finite or is out of range");
    const float minX = std::min({sx[0], sx[1], sx[2]}), maxX = std::max({sx[0], sx[1], sx[2]});
    const float minY = std::min({sy[0], sy[1], sy[2]}), maxY = std::max({sy[0], sy[1], sy[2]});
    // Three distinct corners of the axis-aligned rectangle [minX, maxX] x [minY, maxY].
    for (uint32_t i = 0; i < 3; i++)
    {
        if ((sx[i] != minX && sx[i] != maxX) || (sy[i] != minY && sy[i] != maxY))
            return no("the vertices are not the corners of an axis-aligned rectangle");
        for (uint32_t j = 0; j < i; j++)
            if (sx[i] == sx[j] && sy[i] == sy[j])
                return no("two vertices coincide");
    }
    const int32_t x0 = int32_t(std::ceil(minX - 0.5f)), x1 = int32_t(std::ceil(maxX - 0.5f)), y0 = int32_t(std::ceil(minY - 0.5f)), y1 = int32_t(std::ceil(maxY - 0.5f));
    const int32_t cx0 = std::max({x0, 0, int32_t(d.scissor.x)}), cy0 = std::max({y0, 0, int32_t(d.scissor.y)});
    const int32_t cx1 = std::min(x1, int32_t(d.scissor.x + d.scissor.width)), cy1 = std::min(y1, int32_t(d.scissor.y + d.scissor.height));
    if (cx1 <= cx0 || cy1 <= cy0)
        return no("the rectangle covers no pixel inside the scissor");
    o.rect = {uint32_t(cx0), uint32_t(cy0), uint32_t(cx1 - cx0), uint32_t(cy1 - cy0)};
    if (psKills || d.alphaTest.enabled || d.alphaToMask != 0)
        return no("a fragment may be discarded (kill, alpha test or alpha to mask)");
    const DepthStencil& ds = d.depthStencil;
    const auto replaces = [](const DepthStencil::Face& f) { return f.func == 7 && f.zPass == 2 && f.writeMask == 0xFF; };
    const auto passes = [](const DepthStencil::Face& f) { return f.func == 7; };
    o.depth = ds.zEnable && ds.zWriteEnable && ds.zFunc == 7 && ds.stencilEnable && replaces(ds.front) && (!ds.backfaceEnable || replaces(ds.back));
    o.color = d.colorWriteMask == 0xF && !d.blend.enabled && (!ds.zEnable || ds.zFunc == 7) &&
              (!ds.stencilEnable || (passes(ds.front) && (!ds.backfaceEnable || passes(ds.back))));
    if (!o.depth && !o.color)
        o.why = "neither depth and stencil (ALWAYS, write, REPLACE 0xFF) nor all colour channels without blending are replaced";
    return o;
}

ClearDraw FindClearDraw(const DrawState& d, const OverwriteRect& o, const shader::ShaderTranslation& vs, const shader::ShaderTranslation& ps,
                        const uint32_t* window, size_t windowDwords, uint32_t endian, uint32_t targetHeight)
{
    ClearDraw c;
    const auto no = [&](std::string why) { c.why = std::move(why); return c; };
    if (!o.depth && !o.color)
        return no(o.why.empty() ? std::string("not an overwriting draw") : o.why);
    const bool colorBound = d.kind == DrawKind::Draw && d.colorWriteMask != 0;  // KeyFromDrawState's colorUsed
    const bool depthBound = d.depthStencil.zEnable || d.depthStencil.stencilEnable;  // its depthUsed
    if (colorBound && !o.color)
        return no("colour is written but not all of it replaced (a partial mask, blending or a rejecting test)");
    if (depthBound && !o.depth)
        return no("depth or stencil is tested but not both replaced (z ALWAYS with write, stencil ALWAYS REPLACE 0xFF)");
    if (!colorBound && !depthBound)
        return no("no attachment");
    if (!d.textureSlots.empty())
        return no("a texture is bound");
    if (d.program.paramGen)
        return no("pixel parameter generation is on");
    c.rect = o.rect;
    if (c.rect.y >= targetHeight)
        return no("the rectangle lies below the rows the draw can touch");
    c.rect.height = std::min(c.rect.height, targetHeight - c.rect.y);
    // The vertices as the GPU loads them (the window, swapped by the fetch's endian, as PassThroughPositions reads x and y).
    const shader::ShaderTranslation::PositionFetch& p = vs.positionFetch;
    if (!p.valid || window == nullptr)
        return no("the vertex shader's position is not a plain vertex fetch with a data window");
    const auto dword = [&](uint32_t vertex, uint32_t stride, uint32_t offset, uint32_t component, uint32_t& out) {
        const size_t at = size_t(vertex) * stride + offset + component;
        if (at >= windowDwords)
            return false;
        out = edram::GpuSwap32(window[at], endian);
        return true;
    };
    const auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; };
    uint32_t xy[3][2], z[3];
    for (uint32_t v = 0; v < 3; v++)
        if (!dword(v, p.strideDwords, p.offsetDwords, 0, xy[v][0]) || !dword(v, p.strideDwords, p.offsetDwords, 1, xy[v][1]) ||
            !dword(v, p.strideDwords, p.offsetDwords, 2, z[v]))
            return no("the vertex window is shorter than three positions");
    if (d.target.samples > 1)
        for (uint32_t v = 0; v < 3; v++)
            for (uint32_t axis = 0; axis < 2; axis++)
            {
                const float s = asFloat(xy[v][axis]) + d.viewport.offsetAddXy[axis];  // the screen position, as FindOverwriteRect
                if (s != std::floor(s))
                    return no("a rectangle edge is not on a pixel boundary: on a multisampled target the draw would not write every sample");
            }
    if (depthBound)
    {
        if (z[0] != z[1] || z[0] != z[2])
            return no("the three vertices' z differ");
        const float zf = asFloat(z[0]);
        if (!std::isfinite(zf) || z[0] == 1u << 31)  // -0: the clamp's sign is the implementation's
            return no("z is not finite or is -0");
        const GuestViewport& v = d.viewport;
        if (!v.clipDisabled || v.scale[2] != 1.0f || v.offset[2] != 0.0f)
            return no("the depth fold is not the identity under depth clamp (clip_disable with z viewport scale 1 and offset 0)");
        if (d.polyOffsetScale != 0 || d.polyOffsetOffset != 0)
            return no("polygon offset is on");
        const DepthStencil& ds = d.depthStencil;
        if (ds.backfaceEnable && ds.back.ref != ds.front.ref)
            return no("the back-face stencil reference differs from the front's");
        c.depthValue = std::min(std::max(zf, 0.0f), 1.0f);  // the pipeline's depth clamp (depthClampEnable = clipDisabled) over 0..1
        c.stencil = ds.front.ref & 0xFF;
        c.depth = true;
    }
    if (colorBound)
    {
        if (!ps.colorPassThrough.valid)
            return no("the pixel shader is not a pass-through of an interpolator: " + ps.colorPassThrough.why);
        if (ps.colorPassThrough.interpolator != 0)
            return no("the pixel shader passes interpolator " + std::to_string(ps.colorPassThrough.interpolator) + " through (only 0 is modelled)");
        if (vs.interpolatorCount < 1)
            return no("the vertex shader exports no interpolator");
        const shader::ShaderTranslation::PositionFetch& k = vs.interpolator0Fetch;
        if (!k.valid)
            return no("the vertex shader's interpolator 0 is not a plain vertex fetch: " + k.why);
        if (k.slot != p.slot)
            return no("the colour's vertex fetch slot differs from the position's");
        uint32_t rgba[3][4];
        for (uint32_t v = 0; v < 3; v++)
            for (uint32_t i = 0; i < 4; i++)
                if (!dword(v, k.strideDwords, k.offsetDwords, i, rgba[v][i]))
                    return no("the vertex window is shorter than three colours");
        for (uint32_t i = 0; i < 4; i++)
            if (rgba[0][i] != rgba[1][i] || rgba[0][i] != rgba[2][i])
                return no("the three vertices' colours differ");
        for (uint32_t i = 0; i < 4; i++)
        {
            const float f = asFloat(rgba[0][i]);
            if (!std::isfinite(f))
                return no("a colour component is not finite");
            switch (d.target.colorFormat)
            {
            case 0: case 2: c.rgba[i] = std::min(std::max(f, 0.0f), 1.0f); break;         // the UNORM attachment's clamp
            case 3: c.rgba[i] = f > 0.0f ? std::min(f, i < 3 ? 31.875f : 1.0f) : 0.0f; break;  // XE_COLOR0_7E3 (shader_glsl.cpp)
            default: return no("colour storage format " + std::to_string(d.target.colorFormat) + " is not cleared from a draw (0, 2 and 3 are)");
            }
        }
        c.color = true;
    }
    c.ok = true;
    return c;
}

}  // namespace runtime::video
