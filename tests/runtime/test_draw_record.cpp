// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// CPU-only tests of the draw recorder's pure part (runtime/video/draw_record.{h,cpp}): DrawState
// structs filled by hand with the register values FM4's first screens use (TRACE: the local capture trace10 (not distributed)
// the per-draw line's fields), no Vulkan, no guest memory, no translator. Expected keys, indices and
// block bytes are computed by hand next to each check (Xenia 67f16c4e315f registers.h / xenos.h for the
// bit meanings, shader_glsl.h for the block layouts), never from the code under test. "CONTROL" names a
// negative control.
//
//   tests/run_draw_record_tests.sh
#include <algorithm>
#include "video/draw_record.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace runtime::video;

namespace {

int failures = 0, controls = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}
#define CONTROL(x, name) (controls++, Expect(!(x), "control: " name))
bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

CachedShader Vs(std::vector<uint32_t> slots = {}, std::vector<uint32_t> textures = {})
{
    CachedShader s;
    s.hash = 0xb6cb6460159a82b1ull, s.pixel = false;  // TRACE: the clear VS
    s.t.interpolatorCount = 1;
    s.t.vertexFetchSlots = slots;
    for (size_t i = 0; i < slots.size(); i++)
        s.t.vertexFetchStrides.push_back(4), s.t.vertexFetchEnds.push_back(3);
    s.t.textureSlots = textures;
    for (size_t i = 0; i < textures.size(); i++)
        s.t.textureDimensions.push_back(1), s.t.textureFilters.push_back({});
    return s;
}
CachedShader Ps(std::vector<uint32_t> textures = {})
{
    CachedShader s;
    s.hash = 0x277a2f4a3e7ec198ull, s.pixel = true;  // TRACE: the clear PS
    s.t.interpolatorCount = 1;
    s.t.textureSlots = textures;
    for (size_t i = 0; i < textures.size(); i++)
        s.t.textureDimensions.push_back(1), s.t.textureFilters.push_back({});
    return s;
}

// TRACE draw 48: the 640x360 4X clear. surface 0A020280 (pitch 640, msaa 2 = 4X), color 00020000 (format 2),
// depth 000102D0 (base 720, D24FS8), initiator 00030088 (rect list, auto-index, 3), colormask FFFF (0xF for
// target 0), depthctl 00008777: stencil on, z on, z write, zfunc 7 (always), stencilfunc 7, stencilzpass 2
// (replace), backface off; blend0 00010001 (off); vte 300; sumode 00010000 (no cull).
DrawState Clear()
{
    DrawState d;
    d.kind = DrawKind::Draw;
    d.opcode = 0x36, d.initiator = 0x00030088, d.primitiveType = 8, d.hostPrimitive = HostPrimitive::RectList, d.sourceSelect = 2;
    d.indices.count = 3;
    d.maxIndex = 0xFFFFFF;
    d.edramMode = 4;
    d.depthStencil.stencilEnable = true, d.depthStencil.zEnable = true, d.depthStencil.zWriteEnable = true, d.depthStencil.zFunc = 7;
    d.depthStencil.front = {7, 0, 2, 0, 0, 0xFF, 0xFF};
    d.depthStencil.back = d.depthStencil.front;
    d.colorWriteMask = 0xF;
    d.target.pitch = 640, d.target.msaa = 2, d.target.samples = 4, d.target.colorFormat = 2, d.target.colorFormatRaw = 2;
    d.target.depthBase = 720, d.target.depthFormat = 1, d.target.colorWritten = true, d.target.depthWritten = true;
    d.target.edramRows = 360, d.target.targetHeight = 360;
    d.scissor = {0, 0, 8192, 8192};
    d.viewport.scale[0] = 320, d.viewport.scale[1] = -180, d.viewport.offset[0] = 320, d.viewport.offset[1] = 180;  // CONSTRUCTED (D3D9 viewport)
    d.vertexSlots.push_back({0, true, 0x1000, 12, 2, 4, 3});
    d.aluConstants.assign(2048, 0);
    d.aluConstants[0] = 0x3F800000, d.aluConstants[1024] = 0x40000000;  // VS c0.x = 1.0, PS c0.x = 2.0
    d.boolConstants[0] = 0x5, d.loopConstants[31] = 0x40;
    return d;
}

// TRACE draw 357-type UI draw: surface 14000500 (pitch 1280, 1X), color 000A0000 (format 10 -> storage 2),
// depth 000102D0, initiator 0020008D (quad list, auto-index, 32), colormask F, depthctl 00700730 (z off, stencil
// off: zfunc 3 irrelevant), blend0 07060706 (src alpha / inv src alpha, add, both), colorctl 8700000F (alpha test
// enabled with func 7 = always: not enabled), sumode 00018006 (cull back, front CW), clip 00080000 (dx clip space).
DrawState Ui()
{
    DrawState d;
    d.kind = DrawKind::Draw;
    d.opcode = 0x22, d.initiator = 0x0020008D, d.primitiveType = 13, d.hostPrimitive = HostPrimitive::QuadList, d.sourceSelect = 2;
    d.indices.count = 32;
    d.maxIndex = 0xFFFFFF;
    d.edramMode = 4;
    d.cullBack = true, d.frontClockwise = true;
    d.depthStencil.zFunc = 3;
    d.blend.enabled = true, d.blend.srcColor = 6, d.blend.dstColor = 7, d.blend.colorOp = 0, d.blend.srcAlpha = 6, d.blend.dstAlpha = 7, d.blend.alphaOp = 0;
    d.colorWriteMask = 0xF;
    d.target.pitch = 1280, d.target.samples = 1, d.target.colorFormat = 2, d.target.colorFormatRaw = 10;
    d.target.depthBase = 720, d.target.depthFormat = 1, d.target.colorWritten = true;
    d.target.edramRows = 720, d.target.targetHeight = 720;
    d.scissor = {0, 0, 1280, 720};
    d.viewport.dxClipSpace = true;
    d.vertexSlots.push_back({95, true, 0x20000, 1000, 2, 4, 3});
    TextureSlot t;
    t.slot = 0, t.type = 2, t.sign[0] = t.sign[1] = t.sign[2] = 3, t.gamma = true, t.fetchDimension = 1, t.dimension = 1;
    d.textureSlots.push_back(t);
    d.aluConstants.assign(2048, 0);
    return d;
}

PipelineKey KeyOf(const DrawState& d, const CachedShader& vs, const CachedShader& ps)
{
    PipelineKey k;
    const KeyResult r = KeyFromDrawState(d, vs, ps, 0, 0, k);
    if (!r)
        std::printf("  unexpected refusal: %s\n", r.reason.c_str());
    return k;
}

void TestKeys()
{
    const CachedShader vs = Vs({0}), ps = Ps();
    const DrawState clear = Clear();
    const PipelineKey k = KeyOf(clear, vs, ps);
    Expect(k.topology == uint8_t(Topology::TriangleList) && k.samples == 4 && k.cullFront == 0 && k.cullBack == 0 && k.frontClockwise == 0 &&
               k.depthUsed == 1 && k.depthFormat == 1 && k.depthWrite == 1 && k.depthFunc == 7 && k.stencilEnable == 1 && k.stencilFront[0] == 0 &&
               k.stencilFront[1] == 2 && k.stencilFront[2] == 0 && k.stencilFront[3] == 7 && std::memcmp(k.stencilBack, k.stencilFront, 4) == 0 &&
               k.colorUsed == 1 && k.color[0].format == 2 && k.color[0].writeMask == 0xF && k.color[0].srcColor == 1 && k.color[0].dstColor == 0 &&
               k.color[0].colorOp == 0 && k.color[0].srcAlpha == 1 && k.color[0].dstAlpha == 0 && k.color[0].alphaOp == 0 && k.depthClamp == 0,
           "the clear (TRACE draw 48): 4-sample triangle-list pipeline, depth always + write, stencil replace, format 2, no blend");
    const DrawState ui = Ui();
    const PipelineKey u = KeyOf(ui, Vs({95}), Ps({0}));
    Expect(u.samples == 1 && u.cullBack == 1 && u.frontClockwise == 1 && u.depthUsed == 0 && u.depthFunc == 0 && u.stencilEnable == 0 &&
               u.colorUsed == 1 && u.color[0].format == 2 && u.color[0].srcColor == 6 && u.color[0].dstColor == 7 && u.color[0].srcAlpha == 6 &&
               u.color[0].dstAlpha == 7 && u.color[0].colorOp == 0 && u.color[0].alphaOp == 0,
           "the UI draw (TRACE op 22): 1-sample, cull back CW, no depth attachment (z and stencil off), src-alpha blend, storage format 2");
    {
        DrawState t = ui;
        t.depthStencil.zFunc = 1;  // irrelevant while z_enable is off
        Expect(KeyOf(t, Vs({95}), Ps({0})) == u, "zfunc without z_enable does not change the key");
        t = ui, t.depthStencil.zEnable = true;  // a test without a write still attaches depth
        const PipelineKey z = KeyOf(t, Vs({95}), Ps({0}));
        Expect(z != u && z.depthUsed == 1 && z.depthWrite == 0 && z.depthFunc == 3, "z_enable without z_write attaches depth with func 3, no write");
    }
    {
        DrawState t = clear;
        t.kind = DrawKind::DepthOnly, t.colorWriteMask = 0;
        const PipelineKey dOnly = KeyOf(t, vs, ps);
        Expect(dOnly.colorUsed == 0 && dOnly.depthUsed == 1 && dOnly.color[0].format == 0, "DepthOnly: no colour attachment, blend normalized away");
        t = clear, t.hostPrimitive = HostPrimitive::QuadList;
        Expect(KeyOf(t, vs, ps) == k, "a quad list keys as a triangle list (the indices are rewritten)");
        t = clear, t.hostPrimitive = HostPrimitive::TriangleStrip;
        Expect(KeyOf(t, vs, ps).topology == uint8_t(Topology::TriangleStrip), "a strip keys as a strip");
        t = clear, t.hostPrimitive = HostPrimitive::LineList;  // FM4 Recomp
        Expect(KeyOf(t, vs, ps).topology == uint8_t(Topology::LineList), "a line list keys as a line list");
        PipelineKey out;
        t = clear, t.kind = DrawKind::NoWrite;
        KeyResult r = KeyFromDrawState(t, vs, ps, 0, 0, out);
        Expect(!r && Has(r.reason, "kind 1"), "a NoWrite state has no pipeline");
        t = clear, t.kind = DrawKind::Resolve;
        Expect(!KeyFromDrawState(t, vs, ps, 0, 0, out).ok, "a Resolve state has no pipeline");
        t = clear, t.hostPrimitive = HostPrimitive::PointNoWrite;
        Expect(!KeyFromDrawState(t, vs, ps, 0, 0, out).ok, "a no-write point list has no pipeline");
        t = clear, t.target.samples = 8;
        r = KeyFromDrawState(t, vs, ps, 0, 0, out);
        Expect(!r && Has(r.reason, "8 samples"), "8 samples are refused");
        // FM4 Recomp (alpha, RT format 5): 16_16_16_16 is mapped (R16G16B16A16_SNORM); 6 (16_16_FLOAT) is not.
        t = clear, t.target.colorFormat = 5, t.target.colorFormatRaw = 5;
        Expect(KeyFromDrawState(t, vs, ps, 0, 0, out).ok && out.colorUsed == 1 && out.color[0].format == 5, "colour format 5 (16_16_16_16) keys as format 5");
        t = clear, t.target.colorFormat = 6;
        r = KeyFromDrawState(t, vs, ps, 0, 0, out);
        Expect(!r && Has(r.reason, "colour format 6"), "an unmapped colour format is refused");
        t = clear, t.colorWriteMask = 0, t.depthStencil = {};
        r = KeyFromDrawState(t, vs, ps, 0, 0, out);
        Expect(!r && Has(r.reason, "no attachment"), "no colour write and no depth/stencil test: refused");
        Expect(KeyFromDrawState(clear, vs, ps, 7, 0, out).ok && out != k && KeyFromDrawState(clear, vs, ps, 0, 7, out).ok && out != k,
               "the variant ids are part of the key");
    }
    {
        // FM4 Recomp: polygon offset. The key's depth bias needs a depth attachment and a non-zero pair
        // (X vulkan_pipeline_cache.cc:2083-2092); the factors as X vulkan_command_processor.cc:3276-3297: offset 4 on kD24FS8 ->
        // 4 x 2^24 = 67,108,864; on kD24S8 -> 4 x (2^24 - 1) = 67,108,860; scale 2 -> slope 2 / 16 = 0.125.
        DrawState t = clear;
        t.polyOffsetScale = 2.0f, t.polyOffsetOffset = 4.0f;
        Expect(k.depthBias == 0 && KeyOf(t, vs, ps).depthBias == 1, "a polygon offset with a depth attachment enables depth bias");
        t.target.depthFormat = 1;
        HostDepthBias b = DepthBiasFor(t);
        Expect(b.constant == 67108864.0f && b.slope == 0.125f, "kD24FS8: constant 4 x 2^24, slope 2 / 16");
        t.target.depthFormat = 0;
        b = DepthBiasFor(t);
        Expect(b.constant == 67108860.0f && b.slope == 0.125f, "kD24S8: constant 4 x (2^24 - 1)");
        DrawState noDepth = ui;
        noDepth.polyOffsetScale = 2.0f, noDepth.polyOffsetOffset = 4.0f;
        Expect(KeyOf(noDepth, Vs({95}), Ps({0})).depthBias == 0, "CONTROL-PAIR: no depth attachment, no depth bias");
    }
}

void TestVariants()
{
    const CachedShader vs = Vs({0}), ps = Ps();
    ShaderVariantDefines v, p;
    Expect(VariantsFromDrawState(Clear(), vs, ps, v, p).ok && v.lines == std::vector<std::string>{"XE_RECT_LIST"} && p.lines.empty(),
           "the clear: XE_RECT_LIST on the VS, nothing on the PS");
    const CachedShader uvs = Vs({95}), ups = Ps({0});
    Expect(VariantsFromDrawState(Ui(), uvs, ups, v, p).ok && v.lines.empty() && p.lines == std::vector<std::string>{"XE_TEXTURE_0_GAMMA"},
           "the UI draw: the gamma texture define on the PS only (the VS does not list slot 0)");
    DrawState d = Ui();
    d.textureSlots[0].expAdjust = -2, d.textureSlots[0].lodBias = -16;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok &&
               p.lines == std::vector<std::string>{"XE_TEXTURE_0_GAMMA", "XE_TEXTURE_0_LOD_BIAS -0.5", "XE_TEXTURE_0_SCALE ldexp(1.0, -2)"},
           "exp_adjust -2 and lod_bias -16: SCALE ldexp(1.0, -2), LOD_BIAS -0.5");
    d = Ui(), d.alphaTest.enabled = true, d.alphaTest.func = 4, d.alphaTest.ref = 0.5f;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_ALPHA_REF 0.5", "XE_ALPHA_TEST 4", "XE_TEXTURE_0_GAMMA"},
           "alpha test func 4 ref 0.5");
    d = Ui(), d.alphaToMask = 0x1AA, d.target.samples = 4;  // AA000017 on a 4x target
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok &&
               p.lines == std::vector<std::string>{"XE_ALPHA_TO_MASK 170u", "XE_ALPHA_TO_MASK_SAMPLES 4", "XE_TEXTURE_0_GAMMA"},
           "alpha to mask 0x1AA on 4 samples: offsets 170u (0xAA, the enable bit dropped) and the sample count");
    d = Ui(), d.textureSlots[0].gamma = false, d.textureSlots[0].biasedMask = 7;  // signs 2220, X Y Z W
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_TEXTURE_0_BIASED bvec4(true, true, true, false)"},
           "biased mask 7: XE_TEXTURE_0_BIASED bvec4(true, true, true, false)");
    // FM4 Recomp: PsParamGen (X shader_translator.cc:329-347): the position and whether the primitive is
    // polygonal (X draw_util.h:56-74: a triangle list is, a rect list is not); position 16 is never an interpolator.
    d = Ui(), d.program.paramGen = true, d.program.paramGenPos = 3, d.primitiveType = 4;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok &&
               p.lines == std::vector<std::string>{"XE_PARAM_GEN 3", "XE_PARAM_GEN_POLYGONAL 1", "XE_TEXTURE_0_GAMMA"},
           "param_gen at 3 on a triangle list: XE_PARAM_GEN 3, polygonal");
    d.primitiveType = 8;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines[1] == "XE_PARAM_GEN_POLYGONAL 0", "a rect list is not polygonal");
    d.program.paramGenPos = 16;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_TEXTURE_0_GAMMA"},
           "CONTROL-PAIR: param_gen_pos 16 (not an interpolator): no define");
    d = Ui(), d.target.colorFormat = 1;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines[0] == "XE_COLOR0_GAMMA", "storage format 1: XE_COLOR0_GAMMA");
    d = Ui(), d.target.colorFormat = 3;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines[0] == "XE_COLOR0_7E3", "storage format 3: XE_COLOR0_7E3");
    // FM4 Recomp (alpha, RT format 5): 16_16_16_16 scales colour 0 by 2^(bias - 5) (X vulkan_command_processor.cc:3757-3774:
    // color_exp_bias -= 5 for k_16_16_16_16 on the host-render-target path, then the float with exponent field 127 + bias). By hand:
    // bias 0 (FM4's RB_COLOR_INFO 00050000) 2^-5 = 1/32 = 0.03125; bias -1 2^-6 = 0.015625; bias 31 2^26 = 67108864 (FloatLiteral
    // adds ".0" to an integral %.9g); bias 5 2^0 = 1: no define. The define sorts before XE_TEXTURE_0_GAMMA ('C' < 'T').
    d = Ui(), d.target.colorFormat = 5, d.target.colorFormatRaw = 5, d.target.colorExpBias = 0;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_COLOR0_SCALE 0.03125", "XE_TEXTURE_0_GAMMA"},
           "storage format 5, bias 0: XE_COLOR0_SCALE 0.03125 (2^-5)");
    d.target.colorExpBias = -1;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines[0] == "XE_COLOR0_SCALE 0.015625", "storage format 5, bias -1: 2^-6");
    d.target.colorExpBias = 31;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines[0] == "XE_COLOR0_SCALE 67108864.0", "storage format 5, bias 31: 2^26");
    d.target.colorExpBias = 5;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_TEXTURE_0_GAMMA"}, "storage format 5, bias 5: scale 1, no define");
    d = Ui(), d.target.colorFormat = 4, d.target.colorFormatRaw = 4, d.target.colorExpBias = 5;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_TEXTURE_0_GAMMA"},
           "CONTROL-PAIR: storage format 4 (16_16, bias 5) has no XE_COLOR0_SCALE");
    d = Ui(), d.target.colorFormat = 5, d.target.colorExpBias = 0, d.kind = DrawKind::DepthOnly, d.colorWriteMask = 0;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_NO_COLOR_OUTPUT", "XE_TEXTURE_0_GAMMA"},
           "CONTROL-PAIR: storage format 5 without a colour write: no XE_COLOR0_SCALE");
    d = Ui(), d.target.colorFormat = 1, d.kind = DrawKind::DepthOnly, d.colorWriteMask = 0;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_NO_COLOR_OUTPUT", "XE_TEXTURE_0_GAMMA"},
           "DepthOnly: no colour conversion define, and XE_NO_COLOR_OUTPUT (no colour attachment)");
    // FM4 Recomp: the welcome screen's kDepth clear (the local capture glow (not distributed)): its decoded alpha test is off (draw_state.cpp), so no
    // XE_ALPHA_TEST reaches the PS, which would otherwise discard the clear (alpha 0 under GREATER 0).
    d = Ui(), d.kind = DrawKind::DepthOnly, d.colorWriteMask = 0, d.alphaTest.enabled = false, d.alphaTest.func = 4;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_NO_COLOR_OUTPUT", "XE_TEXTURE_0_GAMMA"},
           "DepthOnly with the decoded alpha test off: no XE_ALPHA_TEST");
    const CachedShader both = Vs({95}, {0});
    Expect(VariantsFromDrawState(Ui(), both, ups, v, p).ok && v.lines == std::vector<std::string>{"XE_TEXTURE_0_GAMMA"}, "a VS that lists the slot gets the define too");
    // FM4 Recomp: a pixel shader reading 3 interpolators of a vertex shader exporting 1: XE_PS_INTERPOLATORS 3 on the VS (the
    // others written as 0, shader_glsl.cpp); equal counts add nothing.
    {
        CachedShader ps3 = Ps({0});
        ps3.t.interpolatorCount = 3;
        Expect(VariantsFromDrawState(Ui(), uvs, ps3, v, p).ok && v.lines == std::vector<std::string>{"XE_PS_INTERPOLATORS 3"},
               "a PS reading 3 interpolators of a VS exporting 1: XE_PS_INTERPOLATORS 3");
        Expect(VariantsFromDrawState(Ui(), uvs, ups, v, p).ok && v.lines.empty(), "equal interpolator counts: no XE_PS_INTERPOLATORS");
    }
}

struct FakeIndices
{
    std::vector<uint32_t> raw;
};
uint32_t ReadFake(const void* ctx, uint32_t i) { return static_cast<const FakeIndices*>(ctx)->raw.at(i); }

void TestIndices()
{
    HostIndices h;
    Expect(BuildHostIndices(Clear(), nullptr, nullptr, h).ok && h.kind == HostIndices::Kind::RectList && h.indices == std::vector<uint32_t>{0, 1, 2} &&
               h.vertexCount == 6 && h.highIndex == 2,
           "an auto-indexed rect list: 3 guest indices for xe_indices, 6 host vertices");
    Expect(BuildHostIndices(Ui(), nullptr, nullptr, h).ok && h.kind == HostIndices::Kind::Indexed && h.vertexCount == 48 && h.indices.size() == 48 &&
               h.indices[0] == 0 && h.indices[1] == 1 && h.indices[2] == 2 && h.indices[3] == 0 && h.indices[4] == 2 && h.indices[5] == 3 &&
               h.indices[42] == 28 && h.indices[47] == 31 && h.highIndex == 31,
           "an auto-indexed quad list of 32: (0 1 2)(0 2 3) per quad, 48 host indices, high 31");
    {
        // FM4 Recomp: an auto-indexed line list of 4 passes through as the triangle list does: 4 host vertices, high index 3.
        DrawState l = Clear();
        l.hostPrimitive = HostPrimitive::LineList, l.indices.count = 4;
        Expect(BuildHostIndices(l, nullptr, nullptr, h).ok && h.kind == HostIndices::Kind::Direct && h.vertexCount == 4 && h.highIndex == 3,
               "an auto-indexed line list of 4: drawn directly, 4 vertices, high index 3");
        // Its pixel parameters mark a line (XE_PARAM_GEN_LINE, z's sign as Xenia).
        l.program.paramGen = true, l.program.paramGenPos = 0, l.primitiveType = 2;
        ShaderVariantDefines lv, lp;
        Expect(VariantsFromDrawState(l, Vs({0}), Ps(), lv, lp).ok &&
                   std::find(lp.lines.begin(), lp.lines.end(), "XE_PARAM_GEN_LINE 1") != lp.lines.end() &&
                   std::find(lp.lines.begin(), lp.lines.end(), "XE_PARAM_GEN_POLYGONAL 0") != lp.lines.end(),
               "a line list's param_gen: XE_PARAM_GEN_LINE 1, not polygonal");
    }
    DrawState d = Clear();
    d.hostPrimitive = HostPrimitive::TriangleList, d.indices.count = 6;
    Expect(BuildHostIndices(d, nullptr, nullptr, h).ok && h.kind == HostIndices::Kind::Direct && h.vertexCount == 6 && h.indices.empty() && h.highIndex == 5,
           "an auto-indexed triangle list with no offset draws directly (gl_VertexIndex = guest index)");
    d.indexOffset = 10;
    Expect(BuildHostIndices(d, nullptr, nullptr, h).ok && h.kind == HostIndices::Kind::Indexed && h.indices == std::vector<uint32_t>{10, 11, 12, 13, 14, 15} &&
               h.highIndex == 15,
           "VGT_INDX_OFFSET 10 on an auto-indexed list: indices 10-15 through an index buffer");
    d.indexOffset = 0, d.maxIndex = 3;
    Expect(BuildHostIndices(d, nullptr, nullptr, h).ok && h.kind == HostIndices::Kind::Indexed && h.indices == std::vector<uint32_t>{0, 1, 2, 3, 3, 3},
           "VGT_MAX_VTX_INDX 3 clamps indices 4 and 5 to 3");
    d.maxIndex = 0xFFFFFF, d.minIndex = 2;
    Expect(BuildHostIndices(d, nullptr, nullptr, h).ok && h.indices == std::vector<uint32_t>{2, 2, 2, 3, 4, 5}, "VGT_MIN_VTX_INDX 2 clamps indices 0 and 1 to 2");
    d.minIndex = 0, d.indexOffset = 0xFFFFFE;  // (i + 0xFFFFFE) & 0xFFFFFF wraps: 0xFFFFFE, 0xFFFFFF, 0, 1, 2, 3 (X registers.h:327-341)
    Expect(BuildHostIndices(d, nullptr, nullptr, h).ok && h.indices == std::vector<uint32_t>{0xFFFFFE, 0xFFFFFF, 0, 1, 2, 3} && h.highIndex == 0xFFFFFF,
           "the 24-bit index offset wraps");
    d.indexOffset = 0, d.hostPrimitive = HostPrimitive::TriangleStrip, d.indices.count = 4;
    Expect(BuildHostIndices(d, nullptr, nullptr, h).ok && h.kind == HostIndices::Kind::Direct && h.vertexCount == 4, "an auto-indexed strip draws directly");
    // DMA indices: 16-bit 8in16 (the guest's big-endian shorts loaded little-endian: 0x0100 is index 1).
    FakeIndices fake{{0x0100, 0x0200, 0x0300}};
    d = Clear(), d.indices.indexed = true, d.indices.is32Bit = false, d.indices.endian = 1, d.indices.bytes = 6;
    Expect(BuildHostIndices(d, ReadFake, &fake, h).ok && h.indices == std::vector<uint32_t>{1, 2, 3} && h.highIndex == 3,
           "16-bit 8in16 DMA indices are byte-swapped (0x0100 -> 1)");
    d.indices.endian = 0;
    Expect(BuildHostIndices(d, ReadFake, &fake, h).ok && h.indices == std::vector<uint32_t>{0x100, 0x200, 0x300}, "16-bit indices with endian none are taken as loaded");
    FakeIndices fake32{{0x01000000, 0x02000000, 0x03000000}};
    d.indices.is32Bit = true, d.indices.endian = 2, d.indices.bytes = 12;
    Expect(BuildHostIndices(d, ReadFake, &fake32, h).ok && h.indices == std::vector<uint32_t>{1, 2, 3}, "32-bit 8in32 DMA indices are byte-swapped");
    d.indices.endian = 1;
    KeyResult r = BuildHostIndices(d, ReadFake, &fake32, h);
    Expect(!r && Has(r.reason, "endian 1 with 32-bit"), "8in16 with 32-bit indices is refused");
    d.indices.endian = 3, d.indices.is32Bit = false, d.indices.bytes = 6;
    r = BuildHostIndices(d, ReadFake, &fake, h);
    Expect(!r && Has(r.reason, "endian 3"), "16in32 with 16-bit indices is refused");
    d.indices.endian = 1, d.indices.bytes = 4;
    r = BuildHostIndices(d, ReadFake, &fake, h);
    Expect(!r && Has(r.reason, "4 bytes, fewer than 3"), "a DMA buffer too short for the count is refused");
    d.indices.bytes = 6;
    r = BuildHostIndices(d, nullptr, nullptr, h);
    Expect(!r && Has(r.reason, "reader"), "an indexed draw without a reader is refused");
    d = Clear(), d.indices.count = 4;
    r = BuildHostIndices(d, nullptr, nullptr, h);
    Expect(!r && Has(r.reason, "whole rects"), "a rect list of 4 indices is refused");
    d = Ui(), d.indices.count = 6;
    r = BuildHostIndices(d, nullptr, nullptr, h);
    Expect(!r && Has(r.reason, "whole quads"), "a quad list of 6 indices is refused");
    d = Clear(), d.indices.count = 0;
    r = BuildHostIndices(d, nullptr, nullptr, h);
    Expect(!r && Has(r.reason, "0 indices"), "a draw of 0 indices is refused");
    d = Clear(), d.hostPrimitive = HostPrimitive::PointNoWrite;
    CONTROL(BuildHostIndices(d, nullptr, nullptr, h).ok, "a no-write point list is not drawn");
    Expect(SwapIndex(0x1234, false, 1) == 0x3412 && SwapIndex(0x12345678, true, 2) == 0x78563412 && SwapIndex(0x12345678, false, 0) == 0x5678,
           "SwapIndex: 8in16 swaps the two bytes, 8in32 all four, a 16-bit element keeps its low 16 bits");

    // Primitive restart (draw_state.h IndexBuffer::restart; X primitive_processor.cc:829-950, primitive_processor.h:80-95,
    // registers.h:306-307: reset check, then offset, then clamp). Expected values by hand; the host restart value is 0xFFFFFFFF.
    d = Clear(), d.hostPrimitive = HostPrimitive::TriangleStrip, d.primitiveType = 6, d.indices.indexed = true;
    d.indices.restart = true, d.indices.resetIndex = 0xFFFF;
    // 16-bit 8in16: guest big-endian shorts 0001 0002 0003 FFFF 0004 0005 0006 loaded little-endian (0x0100 ...); FFFF swaps to FFFF.
    FakeIndices r16{{0x0100, 0x0200, 0x0300, 0xFFFF, 0x0400, 0x0500, 0x0600}};
    d.indices.count = 7, d.indices.is32Bit = false, d.indices.endian = 1, d.indices.bytes = 14;
    Expect(BuildHostIndices(d, ReadFake, &r16, h).ok && h.kind == HostIndices::Kind::Indexed &&
               h.indices == std::vector<uint32_t>{1, 2, 3, 0xFFFFFFFFu, 4, 5, 6} && h.vertexCount == 7 && h.highIndex == 6,
           "16-bit 8in16 strip: FFFF becomes the host restart value, highIndex skips it");
    // 32-bit 8in32, reset index 0x00FFFF: guest 00000001 0000FFFF FF00FFFF 00000002 -> swapped 1, FFFF, FF00FFFF, 2; the low 24 bits
    // of 0xFF00FFFF are 0x00FFFF: a reset too (X compares the low 24 bits, :1128-1135); the 0xFF00FFFF would otherwise be 0x00FFFF.
    FakeIndices r32{{0x01000000, 0xFFFF0000, 0xFFFF00FF, 0x02000000}};
    d.indices.count = 4, d.indices.is32Bit = true, d.indices.endian = 2, d.indices.bytes = 16;
    Expect(BuildHostIndices(d, ReadFake, &r32, h).ok && h.indices == std::vector<uint32_t>{1, 0xFFFFFFFFu, 0xFFFFFFFFu, 2} && h.highIndex == 2,
           "32-bit 8in32 strip: elements whose low 24 bits equal the reset index restart");
    // The comparison precedes VGT_INDX_OFFSET: with offset 1, a raw FFFE is index FFFF (drawn), a raw FFFF restarts.
    FakeIndices off{{0x0100, 0xFEFF, 0xFFFF, 0x0200}};  // raw 1, FFFE, FFFF, 2
    d.indices.count = 4, d.indices.is32Bit = false, d.indices.endian = 1, d.indices.bytes = 8, d.indexOffset = 1;
    Expect(BuildHostIndices(d, ReadFake, &off, h).ok && h.indices == std::vector<uint32_t>{2, 0xFFFF, 0xFFFFFFFFu, 3} && h.highIndex == 0xFFFF,
           "reset before the offset: raw FFFE + 1 = FFFF is drawn, raw FFFF restarts");
    d.indexOffset = 0, d.maxIndex = 2;  // and before the clamp: the reset element is not clamped to 2
    Expect(BuildHostIndices(d, ReadFake, &off, h).ok && h.indices == std::vector<uint32_t>{1, 2, 0xFFFFFFFFu, 2}, "reset before the clamp");
    d.maxIndex = 0xFFFFFF, d.indices.restart = false;
    CONTROL(BuildHostIndices(d, ReadFake, &off, h).ok && h.indices[2] == 0xFFFFFFFFu, "without restart a raw FFFF does not restart");
    Expect(BuildHostIndices(d, ReadFake, &off, h).ok && h.indices[2] == 0xFFFF, "without restart a raw FFFF is vertex 0xFFFF");
    d.indices.restart = true, d.hostPrimitive = HostPrimitive::TriangleList;
    r = BuildHostIndices(d, ReadFake, &off, h);
    Expect(!r && Has(r.reason, "other than a triangle strip"), "restart on a list is refused (DrawState never sets it)");
}

void TestUploads()
{
    DrawState d = Clear();
    d.vertexSlots.push_back({95, true, 0x20000, 1000, 0, 4, 3});
    d.vertexSlots.push_back({1, false, 0, 0, 0, 4, 3});  // an invalid constant: an empty window
    std::vector<VertexWindow> windows;
    uint32_t total = 0;
    // high index 2: slot 0 needs 2 * 4 + 3 = 11 of 12 dwords; slot 95 needs 11 of 1000; slot 1 is empty.
    PlanVertexWindows(d, 2, windows, total);
    Expect(windows.size() == 3 && total == 22 && windows[0].slot == 0 && windows[0].dwords == 11 && windows[0].baseDword == 0 && windows[0].physical == 0x1000 &&
               windows[0].endian == 2 && windows[1].slot == 95 && windows[1].dwords == 11 && windows[1].baseDword == 11 && windows[2].dwords == 0 &&
               windows[2].baseDword == 22,
           "vertex windows: 11 + 11 dwords laid out consecutively, an invalid slot is an empty window");
    PlanVertexWindows(d, 100, windows, total);
    // High index 100: 100 * 4 + 3 = 403 for both, past slot 0's 12-dword constant (Xenia does not bound vertex fetch by the
    // size, X spirv_shader_translator_fetch.cc:126-131); 403 + 403 = 806.
    Expect(windows[0].dwords == 403 && windows[1].dwords == 403 && total == 806, "a high index past the data reaches past the constant's size");

    HostViewport hv;
    hv.scale[0] = 0.5f, hv.scale[1] = -0.5f, hv.scale[2] = 1.0f, hv.scale[3] = 1.0f;
    hv.offset[0] = 0.0f, hv.offset[1] = 0.0f, hv.offset[2] = 0.0f, hv.offset[3] = 0.0f;
    uint8_t sys[kSystemBytes];
    std::memset(sys, 0xAA, sizeof sys);
    PlanVertexWindows(d, 2, windows, total);
    FillXeSystem(windows, hv, 0, sys);
    uint32_t w = 0;
    const auto word = [&](uint32_t offset) { std::memcpy(&w, sys + offset, 4); return w; };
    // xe_fetch: slot 0 at dwords 0-1: (0x1000 | 3), (11 << 2 | 2) = 0x2E; slot 95 at dwords 190-191; slot 1 (dwords 2-3) zero; texture entries zero.
    Expect(word(0) == 0x1003 && word(4) == 0x2E && word(190 * 4) == 0x20003 && word(191 * 4) == (11u << 2) && word(8) == 0 && word(12) == 0 && word(24) == 0,
           "xe_fetch: the used slots' constants with the window as the size, everything else 0");
    // xe_vertex_base: slot 0 -> 0 at byte 768, slot 95 -> 11 at byte 768 + 95 * 4 = 1148, slot 1 -> 22 at 772.
    Expect(word(768) == 0 && word(1148) == 11 && word(772) == 22, "xe_vertex_base: each slot's base dword");
    float f = 0;
    std::memcpy(&f, sys + 1152 + 4, 4);
    Expect(f == -0.5f && word(1184) == 0 && word(1152 + 12) == 0x3F800000, "the viewport scale/offset vec4s at 1152 / 1168 and xe_flags 0 at 1184");
    FillXeSystem(windows, hv, 0x5, sys);
    Expect(word(1184) == 5, "xe_flags carries DrawState::systemFlags (the position-format bits) at byte 1184");

    uint8_t vsC[kConstantsBytes], psC[kConstantsBytes];
    FillConstants(d, false, vsC);
    FillConstants(d, true, psC);
    uint32_t c0 = 0, p0 = 0, b0 = 0, l31 = 0;
    std::memcpy(&c0, vsC, 4), std::memcpy(&p0, psC, 4), std::memcpy(&b0, vsC + 4096, 4), std::memcpy(&l31, psC + 4128 + 31 * 4, 4);
    Expect(c0 == 0x3F800000 && p0 == 0x40000000 && b0 == 0x5 && l31 == 0x40, "constants: VS words 0-1023, PS words 1024-2047, bools at 4096, loops at 4128 (both stages)");
    uint32_t pb0 = 0;
    std::memcpy(&pb0, psC + 4096, 4);
    Expect(pb0 == 0x5, "the PS block carries the same bool dwords");
    {
        // FM4 Recomp (M7 CPU cost): the VS reads 8 words (c0-c1) and the PS 4 (c0): FillConstants copies only those (bytes 0-31 / 0-15); the
        // bytes from there to 4096 keep the caller's 0xAA (never read by the shader), the bool/loop tail is written as before and the rest
        // of the tail (after the loops, byte 4128 + 128 = 4256 on) is cleared. CONTROL: the full range (1024 words) overwrites byte 32.
        DrawState n = d;
        n.vsConstantWords = 8, n.psConstantWords = 4;
        std::memset(vsC, 0xAA, sizeof vsC), std::memset(psC, 0xAA, sizeof psC);
        FillConstants(n, false, vsC);
        FillConstants(n, true, psC);
        std::memcpy(&c0, vsC, 4), std::memcpy(&p0, psC, 4), std::memcpy(&b0, vsC + 4096, 4), std::memcpy(&l31, psC + 4128 + 31 * 4, 4);
        Expect(c0 == 0x3F800000 && p0 == 0x40000000 && vsC[31] == 0 && vsC[32] == 0xAA && vsC[4095] == 0xAA && psC[15] == 0 && psC[16] == 0xAA &&
                   b0 == 0x5 && l31 == 0x40 && vsC[kConstantsBytes - 1] == 0,
               "constants: only the range each stage reads is copied; the bytes past it up to 4096 are left alone, the tail is written");
        std::memset(vsC, 0xAA, sizeof vsC);
        FillConstants(d, false, vsC);
        CONTROL(vsC[32] == 0xAA, "the full 1024-word range overwrites byte 32");
    }
    Expect(kNoWriteDraws == NoWriteDraws::Skip, "the NoWrite policy switch is Skip (decision pending; one place to change)");
}

}  // namespace

// FindOverwriteRect (FM4_OVERWRITE_CLAIM) on TRACE draw 48, the 640x360 4X clear (Clear()), with the positions the clear VS fetches
// (OBSERVED with [diag overwrite]: vte xy/w 1, scale 1, offset 0; three 7-dword vertices (-0.5, -0.5), (639.5, -0.5),
// (639.5, 359.5)); the pixel-centre offset 0.5 (PA_SU_VTX_CNTL pix_center 0) makes the screen x 0, 640, 640: pixels [ceil(-0.5),
// ceil(639.5)) = [0, 640), rows [0, 360). Expected values from the rasterization rule, never from the code.
void TestOverwrite()
{
    DrawState d = Clear();
    d.systemFlags = runtime::shader::kFlagXyDividedByW | runtime::shader::kFlagZDividedByW;
    d.viewport.scale[0] = d.viewport.scale[1] = 1, d.viewport.offset[0] = d.viewport.offset[1] = 0;
    d.viewport.offsetAddXy[0] = d.viewport.offsetAddXy[1] = 0.5f;
    d.scissor = {0, 0, 640, 8192};
    d.vertexSlots[0].stride = 7, d.vertexSlots[0].end = 3, d.vertexSlots[0].sizeWords = 21;
    CachedShader vs = Vs();
    vs.t.positionFetch.valid = true, vs.t.positionFetch.slot = 0, vs.t.positionFetch.strideDwords = 7, vs.t.positionFetch.offsetDwords = 0,
    vs.t.positionFetch.format = 57;
    const auto be = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return __builtin_bswap32(u); };  // guest big-endian floats; endian 2 swaps the host's load
    std::vector<uint32_t> window(21, 0);
    const float xs[3] = {-0.5f, 639.5f, 639.5f}, ys[3] = {-0.5f, -0.5f, 359.5f};
    for (uint32_t v = 0; v < 3; v++)
        window[v * 7] = be(xs[v]), window[v * 7 + 1] = be(ys[v]), window[v * 7 + 2] = be(0.0f);
    OverwriteRect o = FindOverwriteRect(d, vs.t, false, window.data(), window.size(), 2);
    Expect(o.depth && o.color && o.rect.x == 0 && o.rect.y == 0 && o.rect.width == 640 && o.rect.height == 360, "the clear overwrites depth, stencil and colour of 640x360");
    // FM4 Recomp (M7 GPU cost): the same rectangle as a DepthOnly draw (RB_MODECONTROL 5, colour writes off) overwrites depth and stencil; CONTROL: a
    // Resolve is no overwrite.
    {
        DrawState k = d;
        k.kind = DrawKind::DepthOnly, k.colorWriteMask = 0;
        OverwriteRect q = FindOverwriteRect(k, vs.t, false, window.data(), window.size(), 2);
        Expect(q.depth && !q.color && q.rect.width == 640 && q.rect.height == 360, "a DepthOnly clear overwrites depth and stencil, no colour");
        k.kind = DrawKind::Resolve;
        q = FindOverwriteRect(k, vs.t, false, window.data(), window.size(), 2);
        CONTROL(q.depth || q.color, "a Resolve is no overwrite");
    }
    // The scissor cuts the rectangle: rows 100-199 of columns 0-639.
    DrawState s = d;
    s.scissor = {0, 100, 8192, 100};
    o = FindOverwriteRect(s, vs.t, false, window.data(), window.size(), 2);
    Expect(o.depth && o.color && o.rect.x == 0 && o.rect.y == 100 && o.rect.width == 640 && o.rect.height == 100, "the scissor intersects the rectangle");
    // Controls (each breaks one condition).
    DrawState c = d;
    c.blend.enabled = true;
    o = FindOverwriteRect(c, vs.t, false, window.data(), window.size(), 2);
    CONTROL(o.color, "blending keeps the colour");
    Expect(o.depth, "blending does not affect the depth claim");
    c = d, c.depthStencil.zFunc = 3;
    o = FindOverwriteRect(c, vs.t, false, window.data(), window.size(), 2);
    CONTROL(o.depth, "a LESS-EQUAL depth test may reject");
    Expect(!o.color, "and so may reject the colour");
    c = d, c.depthStencil.stencilEnable = false;
    CONTROL(FindOverwriteRect(c, vs.t, false, window.data(), window.size(), 2).depth, "stencil off: the stencil bits are not replaced");
    c = d, c.depthStencil.front.writeMask = 0x7F;
    CONTROL(FindOverwriteRect(c, vs.t, false, window.data(), window.size(), 2).depth, "a partial stencil write mask");
    c = d, c.cullBack = true;
    o = FindOverwriteRect(c, vs.t, false, window.data(), window.size(), 2);
    CONTROL(o.depth || o.color, "culling on");
    c = d, c.indices.count = 6;
    CONTROL(FindOverwriteRect(c, vs.t, false, window.data(), window.size(), 2).depth, "two rectangles");
    c = d, c.colorWriteMask = 0x7;
    o = FindOverwriteRect(c, vs.t, false, window.data(), window.size(), 2);
    CONTROL(o.color, "three colour channels");
    Expect(o.depth, "colour mask does not affect the depth claim");
    c = d, c.alphaTest.enabled = true;
    o = FindOverwriteRect(c, vs.t, false, window.data(), window.size(), 2);
    CONTROL(o.depth || o.color, "an alpha test may discard");
    o = FindOverwriteRect(d, vs.t, true, window.data(), window.size(), 2);
    CONTROL(o.depth || o.color, "a pixel shader kill may discard");
    c = d, c.systemFlags = 0;
    CONTROL(FindOverwriteRect(c, vs.t, false, window.data(), window.size(), 2).depth, "positions not in screen space (vtx_xy_fmt off)");
    CachedShader nv = Vs();  // (CachedShader is not copyable: its variants are unique_ptrs)
    nv.t.positionFetch.valid = false;
    CONTROL(FindOverwriteRect(d, nv.t, false, window.data(), window.size(), 2).depth, "the VS position is not a plain fetch");
    CONTROL(FindOverwriteRect(d, vs.t, false, window.data(), 14, 2).depth, "a window short of the third vertex");
    std::vector<uint32_t> tri = window;
    tri[2 * 7] = be(300.0f), tri[2 * 7 + 1] = be(200.0f);  // (300, 200): neither x nor y is an edge of the other two vertices' rectangle
    CONTROL(FindOverwriteRect(d, vs.t, false, tri.data(), tri.size(), 2).depth, "a triangle that is not an axis-aligned rectangle");
    std::vector<uint32_t> tiny = window;
    tiny[7] = be(0.4f), tiny[14] = be(0.4f);  // x 0.9: pixel 0's centre 0.5 is covered ([ceil(0), ceil(0.4)) = [0, 1)), nothing else in x
    o = FindOverwriteRect(d, vs.t, false, tiny.data(), tiny.size(), 2);
    Expect(o.depth && o.rect.width == 1 && o.rect.height == 360, "a 0.9-pixel-wide rectangle covers pixel column 0 only");
}

// FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): FindClearDraw on TestOverwrite's clear (TRACE draw 48 with the observed positions), the
// clear colour at dwords 3-6 of each 7-dword vertex as the clear VS fetches it (vs_b6cb6460159a82b1.vert
// xe_fetch1) and the PS passing interpolator 0 through (ps_277a2f4a3e7ec198.frag). Expected values from the rules in draw_record.h,
// never from the code: the depth is clamp(z, 0, 1) under depth clamp (PA_CL_CLIP_CNTL clip_disable, trace13 draw 48 clip 00010000;
// vte 0x300 leaves the z viewport scale 1 and offset 0), the stencil is RB_STENCILREFMASK's reference (trace13: 00FF0000, ref 0), the
// colour is the vertices' value through the target's conversion (storage 2: UNORM clamp; storage 3: the 7e3 clamp).
void TestClearDraw()
{
    DrawState d = Clear();
    d.systemFlags = runtime::shader::kFlagXyDividedByW | runtime::shader::kFlagZDividedByW;
    d.viewport.scale[0] = d.viewport.scale[1] = 1, d.viewport.offset[0] = d.viewport.offset[1] = 0;
    d.viewport.offsetAddXy[0] = d.viewport.offsetAddXy[1] = 0.5f;
    d.viewport.clipDisabled = true;
    d.scissor = {0, 0, 640, 8192};
    d.vertexSlots[0].stride = 7, d.vertexSlots[0].end = 7, d.vertexSlots[0].sizeWords = 21;
    runtime::shader::ShaderTranslation vs = Vs().t;
    vs.positionFetch.valid = true, vs.positionFetch.slot = 0, vs.positionFetch.strideDwords = 7, vs.positionFetch.offsetDwords = 0, vs.positionFetch.format = 57;
    vs.interpolator0Fetch.valid = true, vs.interpolator0Fetch.slot = 0, vs.interpolator0Fetch.strideDwords = 7, vs.interpolator0Fetch.offsetDwords = 3,
    vs.interpolator0Fetch.format = 38;
    runtime::shader::ShaderTranslation ps = Ps().t;
    ps.colorPassThrough.valid = true, ps.colorPassThrough.interpolator = 0;
    const auto be = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return __builtin_bswap32(u); };  // guest big-endian floats; endian 2 swaps the host's load
    // Three vertices (x, y, z, r, g, b, a): the 640x360 corners at z 0 with the colour (0.25, 0.5, 0.75, 1).
    const auto vertices = [&](float z, float r, float g, float b, float a) {
        std::vector<uint32_t> w(21, 0);
        const float xs[3] = {-0.5f, 639.5f, 639.5f}, ys[3] = {-0.5f, -0.5f, 359.5f};
        for (uint32_t v = 0; v < 3; v++)
            w[v * 7] = be(xs[v]), w[v * 7 + 1] = be(ys[v]), w[v * 7 + 2] = be(z), w[v * 7 + 3] = be(r), w[v * 7 + 4] = be(g), w[v * 7 + 5] = be(b), w[v * 7 + 6] = be(a);
        return w;
    };
    const std::vector<uint32_t> window = vertices(0.0f, 0.25f, 0.5f, 0.75f, 1.0f);
    const auto find = [&](const DrawState& s, const runtime::shader::ShaderTranslation& v, const runtime::shader::ShaderTranslation& p,
                          const std::vector<uint32_t>& w, uint32_t targetHeight = 360) {
        return FindClearDraw(s, FindOverwriteRect(s, v, p.kills, w.data(), w.size(), 2), v, p, w.data(), w.size(), 2, targetHeight);
    };
    const auto rgbaIs = [](const ClearDraw& c, float r, float g, float b, float a) { return c.rgba[0] == r && c.rgba[1] == g && c.rgba[2] == b && c.rgba[3] == a; };
    ClearDraw c = find(d, vs, ps, window);
    Expect(c.ok && c.color && c.depth && c.rect.x == 0 && c.rect.y == 0 && c.rect.width == 640 && c.rect.height == 360,
           "the clear is a colour + depth/stencil clear of 640x360");
    Expect(rgbaIs(c, 0.25f, 0.5f, 0.75f, 1.0f) && c.depthValue == 0.0f && c.stencil == 0, "colour (0.25, 0.5, 0.75, 1), depth 0, stencil 0 (the reference)");
    // The rows the bind claims cut the rectangle; a rectangle below them is refused.
    c = find(d, vs, ps, window, 200);
    Expect(c.ok && c.rect.height == 200, "the rectangle is cut to the rows the draw can touch (200)");
    CONTROL(find(d, vs, ps, window, 0).ok, "no row the draw can touch");
    // Depth: z 1.5 clamps to 1, an observed draw's -3.72529e-09 to 0 (the pipeline's depth clamp); the stencil reference 0x5A.
    Expect(find(d, vs, ps, vertices(1.5f, 0, 0, 0, 0)).depthValue == 1.0f, "z 1.5 is written as 1.0 (depth clamp)");
    Expect(find(d, vs, ps, vertices(-3.72529e-09f, 0, 0, 0, 0)).depthValue == 0.0f, "a negative z is written as 0.0 (depth clamp)");
    DrawState s = d;
    s.depthStencil.front.ref = s.depthStencil.back.ref = 0x5A;
    Expect(find(s, vs, ps, window).stencil == 0x5A, "the stencil clear value is the front reference");
    // Depth-only: colour mask 0 (as observed) binds no colour attachment.
    s = d, s.colorWriteMask = 0;
    c = find(s, vs, ps, window);
    Expect(c.ok && c.depth && !c.color, "a colour-mask-0 clear defers depth and stencil only");
    s.blend.enabled = true;  // draw 167893: mask 0 with blending on
    Expect(find(s, vs, ps, window).ok, "blending with colour mask 0 does not matter");
    // Colour conversions: storage 3 clamps rgb to [0, 31.875] and alpha to [0, 1], negatives to 0; storage 0 clamps to [0, 1].
    s = d, s.target.colorFormat = 3;
    c = find(s, vs, ps, vertices(0, 40.0f, -1.0f, 0.5f, 2.0f));
    Expect(c.ok && rgbaIs(c, 31.875f, 0.0f, 0.5f, 1.0f), "storage 3: the 7e3 clamp");
    s.target.colorFormat = 0;
    c = find(s, vs, ps, vertices(0, 1.5f, -0.25f, 0.5f, 1.0f));
    Expect(c.ok && rgbaIs(c, 1.0f, 0.0f, 0.5f, 1.0f), "storage 0: the UNORM clamp");
    // Controls (each breaks one condition of draw_record.h FindClearDraw).
    s = d, s.blend.enabled = true;
    c = find(s, vs, ps, window);
    CONTROL(c.ok, "blending with colour written");
    Expect(Has(c.why, "colour is written"), "names the colour");
    s = d, s.alphaTest.enabled = true;
    CONTROL(find(s, vs, ps, window).ok, "an alpha test");
    s = d, s.colorWriteMask = 0x7;
    CONTROL(find(s, vs, ps, window).ok, "a partial colour mask");
    s = d, s.depthStencil.zFunc = 3;
    CONTROL(find(s, vs, ps, window).ok, "a LESS-EQUAL depth test");
    s = d, s.depthStencil.zEnable = s.depthStencil.zWriteEnable = false;  // stencil still REPLACEs: depth is bound but not replaced
    CONTROL(find(s, vs, ps, window).ok, "stencil only, no depth write");
    s = d;
    TextureSlot t;
    t.slot = 0, t.type = 2, t.fetchDimension = 1, t.dimension = 1;
    s.textureSlots.push_back(t);
    CONTROL(find(s, vs, ps, window).ok, "a texture bound");
    s = d, s.program.paramGen = true;
    CONTROL(find(s, vs, ps, window).ok, "pixel parameter generation");
    std::vector<uint32_t> tri = window;
    tri[2 * 7] = be(300.0f), tri[2 * 7 + 1] = be(200.0f);
    CONTROL(find(d, vs, ps, tri).ok, "a triangle that is not an axis-aligned rectangle");
    std::vector<uint32_t> colours = window;
    colours[2 * 7 + 4] = be(0.75f);  // the third vertex's green differs
    CONTROL(find(d, vs, ps, colours).ok, "the vertices' colours differ");
    std::vector<uint32_t> zs = window;
    zs[7 + 2] = be(0.5f);
    CONTROL(find(d, vs, ps, zs).ok, "the vertices' z differ");
    CONTROL(find(d, vs, ps, vertices(-0.0f, 0, 0, 0, 0)).ok, "z -0");
    s = d, s.viewport.clipDisabled = false;
    CONTROL(find(s, vs, ps, window).ok, "clipping on (no depth clamp)");
    s = d, s.viewport.scale[2] = 0.5f;
    CONTROL(find(s, vs, ps, window).ok, "a z viewport scale");
    s = d, s.polyOffsetOffset = 1.0f;
    CONTROL(find(s, vs, ps, window).ok, "polygon offset");
    s = d, s.depthStencil.backfaceEnable = true, s.depthStencil.back.ref = 1;
    CONTROL(find(s, vs, ps, window).ok, "a back-face stencil reference that differs");
    s = d, s.target.colorFormat = 1;
    CONTROL(find(s, vs, ps, window).ok, "storage 1 (gamma)");
    s.target.colorFormat = 4;
    CONTROL(find(s, vs, ps, window).ok, "storage 4 (16_16)");
    // A fractional edge (x 0.9, TestOverwrite's `tiny`): refused at 4x, where pixel 0's other samples are not covered; exact at 1x.
    std::vector<uint32_t> tiny = window;
    tiny[7] = be(0.4f), tiny[14] = be(0.4f);
    CONTROL(find(d, vs, ps, tiny).ok, "a fractional edge on a 4x target");
    s = d, s.target.samples = 1, s.target.msaa = 0;
    c = find(s, vs, ps, tiny);
    Expect(c.ok && c.rect.width == 1, "a fractional edge on a 1x target (the pixel-centre rule is exact)");
    runtime::shader::ShaderTranslation p2 = ps;
    p2.colorPassThrough.valid = false;
    CONTROL(find(d, vs, p2, window).ok, "the PS is not a pass-through");
    DrawState depthOnly = d;
    depthOnly.colorWriteMask = 0;
    Expect(find(depthOnly, vs, p2, window).ok, "the PS does not matter for a depth-only clear");
    p2 = ps, p2.colorPassThrough.interpolator = 2;
    CONTROL(find(d, vs, p2, window).ok, "the PS passes interpolator 2 through");
    runtime::shader::ShaderTranslation v2 = vs;
    v2.interpolator0Fetch.valid = false;
    CONTROL(find(d, v2, ps, window).ok, "the VS interpolator is not a plain fetch");
    v2 = vs, v2.interpolator0Fetch.slot = 1;
    CONTROL(find(d, v2, ps, window).ok, "the colour's fetch slot differs from the position's");
    CONTROL(FindClearDraw(d, FindOverwriteRect(d, vs, false, window.data(), window.size(), 2), vs, ps, window.data(), 17, 2, 360).ok,
            "a window short of the third vertex's colour");
}

int main()
{
    TestKeys();
    TestVariants();
    TestIndices();
    TestUploads();
    TestOverwrite();
    TestClearDraw();
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures == 0 ? 0 : 1;
}
