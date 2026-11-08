// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See pipeline_key.h. Bit positions are Xenia's (67f16c4e315f registers.h / xenos.h, cited there);
// the normalization rules are Xenia's GetCurrentStateDescription (vulkan_pipeline_cache.cc:509-660)
// where they are not ours, each marked.
#include "pipeline_key.h"
#include "shader/ucode.h"

#include <algorithm>
#include <cstdio>

namespace runtime::video {

namespace {

std::string Hex8(uint32_t v)
{
    char s[12];
    std::snprintf(s, sizeof s, "%08X", v);
    return s;
}

// A float as a GLSL float literal (always with a '.' or exponent, so `a < 1` is not an int compare).
std::string FloatLiteral(float f)
{
    char s[32];
    std::snprintf(s, sizeof s, "%.9g", double(f));
    std::string t = s;
    if (t.find_first_of(".eEn") == std::string::npos)  // "1", "-2"; inf/nan carry 'n'
        t += ".0";
    return t;
}

bool Polygonal(Topology t) { return t == Topology::TriangleList || t == Topology::TriangleStrip; }

KeyResult TextureDefines(const DrawStateView& s, const CachedShader& shader, ShaderVariantDefines& defines)
{
    const char* stage = shader.pixel ? "pixel" : "vertex";
    for (size_t i = 0; i < shader.t.textureSlots.size(); i++)
    {
        const uint32_t slot = shader.t.textureSlots[i];
        const uint32_t* c = s.fetchConstants + 6 * slot;
        const std::string where = std::string(stage) + " shader texture slot " + std::to_string(slot) + " (fetch constant " + Hex8(c[0]) + " " +
                                  Hex8(c[1]) + " " + Hex8(c[2]) + " " + Hex8(c[3]) + " " + Hex8(c[4]) + " " + Hex8(c[5]) + ")";
        if ((c[0] & 3) != 2)
            return Refuse(where + ": not a texture fetch constant (type " + std::to_string(c[0] & 3) + ")");
        const uint32_t dimension = Field(c[5], 9, 2), stacked = Field(c[1], 10, 1), wanted = shader.t.textureDimensions[i];
        if (wanted == 1 && (dimension != 1 || stacked))
            return Refuse(where + ": the shader samples it as 2D but the constant is dimension " + std::to_string(dimension) +
                          (stacked ? " stacked" : ""));
        if (wanted == 2 && dimension != 2)
            return Refuse(where + ": the shader samples it as 3D but the constant is dimension " + std::to_string(dimension) +
                          (dimension == 1 && stacked ? " (a stacked 2D texture fetched as 3D is not supported)" : ""));
        if (wanted == 3 && dimension != 3)
            return Refuse(where + ": the shader samples it as a cube but the constant is dimension " + std::to_string(dimension));
        // Gamma: the signs (dword 0 bits 2-9, two per channel) of the channels the constant's swizzle
        // (dword 3 bits 1-12, three per result component: 0-3 = x, y, z, w; 4 = 0; 5 = 1) puts in x, y, z.
        uint32_t gamma = 0, linear = 0;
        for (uint32_t k = 0; k < 3; k++)
        {
            const uint32_t sw = Field(c[3], 1 + 3 * k, 3);
            if (sw == 4 || sw == 5)
                continue;  // a constant channel: neither (0 and 1 are fixed points of the PWL curve)
            if (sw > 5)
                return Refuse(where + ": swizzle value " + std::to_string(sw) + " (meaning unknown)");
            (Field(c[0], 2 + 2 * sw, 2) == 3 ? gamma : linear)++;
        }
        if (gamma && linear)
            return Refuse(where + ": gamma and non-gamma channels mixed in x, y, z (the shader converts all or none)");
        const std::string name = "XE_TEXTURE_" + std::to_string(slot);
        if (gamma)
            defines.Add(name + "_GAMMA");
        if (const int32_t exp = runtime::shader::ucode::SignedBits(c[3], 13, 6); exp != 0)
            defines.Add(name + "_SCALE ldexp(1.0, " + std::to_string(exp) + ")");
        if (const int32_t bias = runtime::shader::ucode::SignedBits(c[4], 12, 10); bias != 0)
            defines.Add(name + "_LOD_BIAS " + FloatLiteral(float(bias) / 32.0f));
    }
    return Accept();
}

}  // namespace

KeyResult DerivePipelineKey(const DrawStateView& s, const CachedShader& vs, const CachedShader& ps, uint64_t vsVariant, uint64_t psVariant,
                            PipelineKey& out)
{
    PipelineKey k;
    std::memset(&k, 0, sizeof k);
    k.vsHash = vs.hash, k.psHash = ps.hash, k.vsVariant = vsVariant, k.psVariant = psVariant;

    const uint32_t mode = s.rbModeControl & 7;
    if (mode != 4 && mode != 5)
        return Refuse("RB_MODECONTROL " + Hex8(s.rbModeControl) + ": edram_mode " + std::to_string(mode) + " is not a draw (4 colour + depth, 5 depth)");

    switch (s.primitiveType)
    {
    case 1: k.topology = uint8_t(Topology::PointList); break;
    case 2: k.topology = uint8_t(Topology::LineList); break;
    case 3: k.topology = uint8_t(Topology::LineStrip); break;
    case 4: case 8: k.topology = uint8_t(Topology::TriangleList); break;  // 8: rect list, expanded by the VS variant
    case 6: k.topology = uint8_t(Topology::TriangleStrip); break;
    default: return Refuse("primitive type " + std::to_string(s.primitiveType) + " is not supported (point, line list/strip, triangle list/strip, rect list)");
    }
    const Topology topology = Topology(k.topology);
    const bool strip = topology == Topology::LineStrip || topology == Topology::TriangleStrip;
    k.primitiveRestart = uint8_t(strip && Field(s.paSuScModeCntl, 21, 1));

    if (Polygonal(topology))
    {
        k.cullFront = uint8_t(Field(s.paSuScModeCntl, 0, 1));
        k.cullBack = uint8_t(Field(s.paSuScModeCntl, 1, 1));
        k.frontClockwise = uint8_t(Field(s.paSuScModeCntl, 2, 1));
        // Xenia: the polygon type of the unculled faces when poly_mode is dual (1); fill otherwise.
        if (Field(s.paSuScModeCntl, 3, 2) == 1)
        {
            const uint32_t front = Field(s.paSuScModeCntl, 5, 3), back = Field(s.paSuScModeCntl, 8, 3);
            if ((!k.cullFront && front != 2) || (!k.cullBack && back != 2))
                return Refuse("PA_SU_SC_MODE_CNTL " + Hex8(s.paSuScModeCntl) + ": dual poly_mode with polygon type front " + std::to_string(front) +
                              " back " + std::to_string(back) + " (only triangles, 2, are supported)");
        }
        k.depthBias = uint8_t(Field(s.paSuScModeCntl, 11, 1) | Field(s.paSuScModeCntl, 12, 1) << 1);
    }
    k.depthClamp = uint8_t(Field(s.paClClipCntl, 16, 1));

    if (const uint32_t samples = Field(s.rbSurfaceInfo, 16, 2); samples != 0)
        return Refuse("RB_SURFACE_INFO " + Hex8(s.rbSurfaceInfo) + ": msaa_samples " + std::to_string(samples) + " (MSAA needs the EDRAM core, unit 6)");
    k.samples = 1;

    // Depth / stencil: attached when the test, the write or the stencil is on; normalized as Xenia
    // (z_enable off: compare always, no write; stencil off: zeros; backface_enable off: the back
    // state is the front state, ForzaRecomp DecodeDepthStencil's reading of RB_DEPTHCONTROL bit 7).
    const uint32_t dc = s.rbDepthControl;
    const bool zEnable = Field(dc, 1, 1) != 0, stencilEnable = Field(dc, 0, 1) != 0;
    if (zEnable || stencilEnable)
    {
        k.depthUsed = 1;
        k.depthFormat = uint8_t(Field(s.rbDepthInfo, 16, 1));
        k.depthWrite = uint8_t(zEnable ? Field(dc, 2, 1) : 0);
        k.depthFunc = uint8_t(zEnable ? Field(dc, 4, 3) : 7);
        if (stencilEnable)
        {
            k.stencilEnable = 1;
            k.stencilFront[0] = uint8_t(Field(dc, 11, 3)), k.stencilFront[1] = uint8_t(Field(dc, 14, 3));
            k.stencilFront[2] = uint8_t(Field(dc, 17, 3)), k.stencilFront[3] = uint8_t(Field(dc, 8, 3));
            if (Field(dc, 7, 1))
            {
                k.stencilBack[0] = uint8_t(Field(dc, 23, 3)), k.stencilBack[1] = uint8_t(Field(dc, 26, 3));
                k.stencilBack[2] = uint8_t(Field(dc, 29, 3)), k.stencilBack[3] = uint8_t(Field(dc, 20, 3));
            }
            else
                std::memcpy(k.stencilBack, k.stencilFront, sizeof k.stencilBack);
        }
    }

    // Colour 0 only (the translator exports xe_color0 alone); attached when its write mask is non-zero
    // and the mode draws colour.
    if (const uint32_t mask = Field(s.rbColorMask, 0, 4); mask != 0 && mode == 4)
    {
        k.colorUsed = 1;
        PipelineKeyColor& c = k.color[0];
        c.writeMask = uint8_t(mask);
        c.format = uint8_t(Field(s.rbColorInfo[0], 16, 4));
        if (!ColorFormatMapped(c.format))
            return Refuse("RB_COLOR_INFO " + Hex8(s.rbColorInfo[0]) + ": colour format " + std::to_string(c.format) + " is not mapped on the host");
        // color_exp_bias (bits 20-25, signed): pipeline_key.h ColorExpBiasAccepted (only the biases whose host scale is 1).
        if (const int32_t bias = runtime::shader::ucode::SignedBits(s.rbColorInfo[0], 20, 6); !ColorExpBiasAccepted(c.format, bias))
            return Refuse("RB_COLOR_INFO " + Hex8(s.rbColorInfo[0]) + ": colour exponent bias " + std::to_string(bias) + " is not supported yet");
        const uint32_t bc = s.rbBlendControl[0];
        const uint32_t factors[4] = {Field(bc, 0, 5), Field(bc, 8, 5), Field(bc, 16, 5), Field(bc, 24, 5)};
        for (const uint32_t f : factors)
            if (f == 2 || f == 3 || f > 16)
                return Refuse("RB_BLENDCONTROL0 " + Hex8(bc) + ": blend factor " + std::to_string(f) + " (meaning unknown)");
        const uint32_t ops[2] = {Field(bc, 5, 3), Field(bc, 21, 3)};
        for (const uint32_t op : ops)
            if (op > 4)
                return Refuse("RB_BLENDCONTROL0 " + Hex8(bc) + ": blend op " + std::to_string(op) + " (meaning unknown)");
        c.srcColor = uint8_t(factors[0]), c.dstColor = uint8_t(factors[1]), c.colorOp = uint8_t(ops[0]);
        c.srcAlpha = uint8_t(factors[2]), c.dstAlpha = uint8_t(factors[3]), c.alphaOp = uint8_t(ops[1]);
    }
    if (!k.colorUsed && !k.depthUsed)
        return Refuse("a draw with no colour write (RB_COLOR_MASK " + Hex8(s.rbColorMask) + ") and no depth or stencil (RB_DEPTHCONTROL " + Hex8(dc) +
                      ") has no attachment to render into");
    out = k;
    return Accept();
}

KeyResult DeriveVariants(const DrawStateView& s, const CachedShader& vs, const CachedShader& ps, ShaderVariantDefines& vsDefines,
                         ShaderVariantDefines& psDefines)
{
    vsDefines = {}, psDefines = {};
    if (s.fetchConstants == nullptr)
        return Refuse("the draw state has no fetch constants");
    if (s.primitiveType == 8)
        vsDefines.Add("XE_RECT_LIST");
    if (KeyResult r = TextureDefines(s, vs, vsDefines); !r)
        return r;
    if (KeyResult r = TextureDefines(s, ps, psDefines); !r)
        return r;
    if (Field(s.rbColorControl, 3, 1))
    {
        psDefines.Add("XE_ALPHA_TEST " + std::to_string(Field(s.rbColorControl, 0, 3)));
        psDefines.Add("XE_ALPHA_REF " + FloatLiteral(s.alphaRef));
    }
    if (Field(s.rbColorMask, 0, 4) != 0 && (s.rbModeControl & 7) == 4)
    {
        const uint32_t format = Field(s.rbColorInfo[0], 16, 4);
        if (format == 1)
            psDefines.Add("XE_COLOR0_GAMMA");
        else if (format == 3)
            psDefines.Add("XE_COLOR0_7E3");
    }
    return Accept();
}

KeyResult PlanVertexSlots(const DrawStateView& s, const CachedShader& vs, uint32_t highIndex, std::vector<VertexSlotPlan>& out, uint32_t& totalDwords)
{
    out.clear();
    totalDwords = 0;
    if (s.fetchConstants == nullptr)
        return Refuse("the draw state has no fetch constants");
    for (size_t i = 0; i < vs.t.vertexFetchSlots.size(); i++)
    {
        VertexSlotPlan p;
        p.slot = vs.t.vertexFetchSlots[i];
        const uint32_t w0 = s.fetchConstants[2 * p.slot], w1 = s.fetchConstants[2 * p.slot + 1];
        const std::string where = "vertex fetch slot " + std::to_string(p.slot) + " (fetch constant " + Hex8(w0) + " " + Hex8(w1) + ")";
        if ((w0 & 3) != 3)
            return Refuse(where + ": not a vertex fetch constant (type " + std::to_string(w0 & 3) + ")");
        p.guestAddress = w0 & ~3u;
        p.endian = w1 & 3;
        p.sizeDwords = Field(w1, 2, 24);
        if (p.sizeDwords == 0)
            return Refuse(where + ": size 0");
        if (uint64_t(p.guestAddress) + uint64_t(p.sizeDwords) * 4 > (uint64_t(1) << 29))
            return Refuse(where + ": the range leaves the 512 MB physical space");
        const uint64_t need = uint64_t(highIndex) * vs.t.vertexFetchStrides[i] + vs.t.vertexFetchEnds[i];
        p.copyDwords = uint32_t(std::min<uint64_t>(need, p.sizeDwords));
        p.baseDword = totalDwords;
        totalDwords += p.copyDwords;
        out.push_back(p);
    }
    return Accept();
}

KeyResult PlanDescriptors(const CachedShader& vs, const CachedShader& ps, const SamplerLimits& limits, DescriptorPlan& out)
{
    out = {};
    for (const CachedShader* shader : {&vs, &ps})
        for (size_t i = 0; i < shader->t.textureSlots.size(); i++)
        {
            const uint32_t slot = shader->t.textureSlots[i], d = shader->t.textureDimensions[i];
            if (slot >= kTextureBindings || d < 1 || d > 3)
                return Refuse(std::string(shader->pixel ? "pixel" : "vertex") + " shader texture slot " + std::to_string(slot) + " dimension " +
                              std::to_string(d) + " is out of range");
            if (out.dimension[slot] != 0 && out.dimension[slot] != d)
                return Refuse("texture slot " + std::to_string(slot) + " is sampled as dimension " + std::to_string(out.dimension[slot]) +
                              " by one stage and " + std::to_string(d) + " by the other (one binding, one sampler type)");
            out.dimension[slot] = uint8_t(d);
            (shader->pixel ? out.psSlots : out.vsSlots) |= 1u << slot;
        }
    const uint32_t perStage = std::min(limits.perStageSamplers, limits.perStageSampledImages);
    const uint32_t perSet = std::min(limits.setSamplers, limits.setSampledImages);
    const struct { const char* name; uint32_t slots, buffers; } stages[2] = {{"vertex", out.vsSlots, kVsBufferBindings}, {"pixel", out.psSlots, kPsBufferBindings}};
    for (const auto& st : stages)
    {
        const uint32_t n = uint32_t(__builtin_popcount(st.slots));
        if (n > perStage)
            return Refuse(std::string(st.name) + " shader samples " + std::to_string(n) + " texture slots, over the device's per-stage limit of " +
                          std::to_string(perStage) + " (maxPerStageDescriptorSamplers / SampledImages)");
        if (n + st.buffers > limits.perStageResources)
            return Refuse(std::string(st.name) + " stage needs " + std::to_string(n + st.buffers) + " resources (" + std::to_string(n) + " samplers, " +
                          std::to_string(st.buffers) + " buffers), over maxPerStageResources " + std::to_string(limits.perStageResources));
    }
    if (const uint32_t n = uint32_t(__builtin_popcount(out.vsSlots | out.psSlots)); n > perSet)
        return Refuse("the pair samples " + std::to_string(n) + " distinct texture slots, over the device's per-set limit of " + std::to_string(perSet) +
                      " (maxDescriptorSetSamplers / SampledImages)");
    out.layoutKey = SamplerLayoutKey(out.vsSlots, out.psSlots);
    return Accept();
}

}  // namespace runtime::video
