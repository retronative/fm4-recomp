// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See draw_record.h.
#include "draw_record.h"

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

KeyResult KeyFromDrawState(const DrawState& d, const CachedShader& vs, const CachedShader& ps, uint64_t vsVariant, uint64_t psVariant, PipelineKey& out)
{
    if (d.kind != DrawKind::Draw && d.kind != DrawKind::DepthOnly)
        return Refuse("a draw of kind " + Num(uint32_t(d.kind)) + " has no pipeline (Draw 2, DepthOnly 3 do)");
    PipelineKey k;
    std::memset(&k, 0, sizeof k);
    k.vsHash = vs.hash, k.psHash = ps.hash, k.vsVariant = vsVariant, k.psVariant = psVariant;
    switch (d.hostPrimitive)
    {
    case HostPrimitive::TriangleList: case HostPrimitive::RectList: case HostPrimitive::QuadList: k.topology = uint8_t(Topology::TriangleList); break;
    case HostPrimitive::TriangleStrip: k.topology = uint8_t(Topology::TriangleStrip); break;
    case HostPrimitive::PointNoWrite: return Refuse("a point list that writes nothing has no pipeline");
    }
    k.primitiveRestart = 0;  // DrawState refuses multi_prim_ib_ena on strips
    k.cullFront = d.cullFront, k.cullBack = d.cullBack, k.frontClockwise = d.frontClockwise;  // every host primitive is polygonal
    k.depthBias = 0;  // DrawState refuses the poly offset enables
    k.depthClamp = d.viewport.clipDisabled;
    if (d.target.samples != 1 && d.target.samples != 2 && d.target.samples != 4)
        return Refuse("RB_SURFACE_INFO: " + Num(d.target.samples) + " samples");
    k.samples = uint8_t(d.target.samples);
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
    out = k;
    return Accept();
}

KeyResult VariantsFromDrawState(const DrawState& d, const CachedShader& vs, const CachedShader& ps, ShaderVariantDefines& vsDefines,
                                ShaderVariantDefines& psDefines)
{
    vsDefines = {}, psDefines = {};
    if (d.hostPrimitive == HostPrimitive::RectList)
        vsDefines.Add("XE_RECT_LIST");
    for (const TextureSlot& t : d.textureSlots)
    {
        const std::string name = "XE_TEXTURE_" + Num(t.slot);
        for (ShaderVariantDefines* defines : {Lists(vs, t.slot) ? &vsDefines : nullptr, Lists(ps, t.slot) ? &psDefines : nullptr})
        {
            if (defines == nullptr)
                continue;
            if (t.gamma)
                defines->Add(name + "_GAMMA");
            if (t.expAdjust != 0)
                defines->Add(name + "_SCALE ldexp(1.0, " + std::to_string(t.expAdjust) + ")");
            if (t.lodBias != 0)
                defines->Add(name + "_LOD_BIAS " + FloatLiteral(float(t.lodBias) / 32.0f));
        }
    }
    if (d.alphaTest.enabled)
    {
        psDefines.Add("XE_ALPHA_TEST " + Num(d.alphaTest.func));
        psDefines.Add("XE_ALPHA_REF " + FloatLiteral(d.alphaTest.ref));
    }
    if (d.kind == DrawKind::Draw && d.colorWriteMask != 0)
    {
        if (d.target.colorFormat == 1)
            psDefines.Add("XE_COLOR0_GAMMA");
        else if (d.target.colorFormat == 3)
            psDefines.Add("XE_COLOR0_7E3");
    }
    return Accept();
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
    const auto guest = [&](uint32_t i) {
        const uint32_t raw = ib.indexed ? SwapIndex(read(context, i), ib.is32Bit, ib.endian) : i;
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
    case HostPrimitive::TriangleList: case HostPrimitive::TriangleStrip:
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
    out.highIndex = *std::max_element(out.indices.begin(), out.indices.end());
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
    std::memset(out, 0, kConstantsBytes);
    if (d.aluConstants.size() >= 2048)
        std::memcpy(out + kConstantsAluOffset, d.aluConstants.data() + (pixel ? 1024 : 0), 4096);
    std::memcpy(out + kConstantsBoolOffset, d.boolConstants, sizeof d.boolConstants);
    std::memcpy(out + kConstantsLoopOffset, d.loopConstants, sizeof d.loopConstants);
}

}  // namespace runtime::video
