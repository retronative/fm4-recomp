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
#include <tuple>
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
    // FM4 Recomp (the rectangular car shadow): a signed-integer k_16_16 fetch (format 25, number format 1) reads the int16 itself:
    // its R16G16_SNORM view (int16 / 32767) scaled by 32767; with exp_adjust 3 also 2^3.
    d = Ui(), d.textureSlots[0].gamma = false, d.textureSlots[0].format = 25, d.textureSlots[0].numFormat = 1;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_TEXTURE_0_SCALE 32767.0 * ldexp(1.0, 0)"},
           "signed-integer k_16_16: SCALE 32767.0 * ldexp(1.0, 0)");
    d.textureSlots[0].expAdjust = 3;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines == std::vector<std::string>{"XE_TEXTURE_0_SCALE 32767.0 * ldexp(1.0, 3)"},
           "signed-integer k_16_16 with exp_adjust 3: SCALE 32767.0 * ldexp(1.0, 3)");
    d = Ui(), d.textureSlots[0].gamma = false, d.textureSlots[0].format = 25, d.textureSlots[0].numFormat = 0;
    Expect(VariantsFromDrawState(d, uvs, ups, v, p).ok && p.lines.empty(), "CONTROL: a fraction k_16_16 fetch gets no integer scale");
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

// FM4 Recomp (M7 CPU cost, FM4_VARIANT_MEMO; draw_record.h VariantMemoKeyFor / VariantMemo). The memo is sound when every input of
// VariantsFromDrawState is in the key: for each input, a change whose define text differs (written by hand, from the define rules
// in draw_record.h and the checks of TestVariants above) must give another key; a change of a field VariantsFromDrawState does not
// read (the defines written by hand stay the base's) keeps the key, so such draws hit. The shader objects stand for the pair.
void TestVariantMemo()
{
    const CachedShader uvs = Vs({95}), ups = Ps({0});
    const CachedShader both = Vs({95}, {0});  // a VS that lists slot 0 too: XE_TEXTURE_0_GAMMA on the VS
    CachedShader ps3 = Ps({0});
    ps3.t.interpolatorCount = 3;  // XE_PS_INTERPOLATORS 3 on the VS
    VariantMemoKey base, k;
    Expect(VariantMemoKeyFor(Ui(), uvs, ups, false, base), "variant memo: the UI draw is keyed");
    Expect(VariantMemoKeyFor(Ui(), uvs, ups, false, k) && k == base && VariantMemoKeyHash{}(k) == VariantMemoKeyHash{}(base),
           "variant memo: the same draw gives the same key and hash");
    ShaderVariantDefines v, p;
    const auto defines = [&](const DrawState& d, const CachedShader& vs, const CachedShader& ps) {
        v.lines.clear(), p.lines.clear();
        return VariantsFromDrawState(d, vs, ps, v, p).ok;
    };
    const auto keyOf = [&](const DrawState& d, const CachedShader& vs, const CachedShader& ps, bool unchecked) {
        VariantMemoKey out;
        if (!VariantMemoKeyFor(d, vs, ps, unchecked, out))
            out.count = 0;
        return out;
    };
    const std::vector<std::string> gamma{"XE_TEXTURE_0_GAMMA"};

    // Inputs VariantsFromDrawState reads: each changed once; the expected defines by hand; the key must differ.
    struct Case
    {
        const char* name;
        DrawState d;
        std::vector<std::string> ps;  // the PS defines by hand
    };
    std::vector<Case> cases;
    DrawState d = Ui();
    d.textureSlots[0].gamma = false;
    cases.push_back({"variant memo: gamma off (no define) is keyed", d, {}});
    d = Ui(), d.textureSlots[0].gamma = false, d.textureSlots[0].biasedMask = 7;
    cases.push_back({"variant memo: biasedMask 7 is keyed", d, {"XE_TEXTURE_0_BIASED bvec4(true, true, true, false)"}});
    d = Ui(), d.textureSlots[0].expAdjust = -2;
    cases.push_back({"variant memo: exp_adjust -2 is keyed", d, {"XE_TEXTURE_0_GAMMA", "XE_TEXTURE_0_SCALE ldexp(1.0, -2)"}});
    d = Ui(), d.textureSlots[0].lodBias = -16;
    cases.push_back({"variant memo: lod_bias -16 is keyed", d, {"XE_TEXTURE_0_GAMMA", "XE_TEXTURE_0_LOD_BIAS -0.5"}});
    d = Ui(), d.textureSlots[0].gamma = false, d.textureSlots[0].format = 25, d.textureSlots[0].numFormat = 1;
    cases.push_back({"variant memo: the signed-integer k_16_16 fetch is keyed", d, {"XE_TEXTURE_0_SCALE 32767.0 * ldexp(1.0, 0)"}});
    d = Ui(), d.textureSlots[0].slot = 1;  // the PS lists slot 0 only: no define for slot 1
    cases.push_back({"variant memo: the texture slot number is keyed", d, {}});
    d = Ui(), d.alphaTest.enabled = true, d.alphaTest.func = 4, d.alphaTest.ref = 0.5f;
    cases.push_back({"variant memo: the alpha test is keyed", d, {"XE_ALPHA_REF 0.5", "XE_ALPHA_TEST 4", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.alphaTest.enabled = true, d.alphaTest.func = 4, d.alphaTest.ref = 0.25f;
    cases.push_back({"variant memo: the alpha reference alone is keyed (0.25 vs 0.5)", d, {"XE_ALPHA_REF 0.25", "XE_ALPHA_TEST 4", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.alphaTest.enabled = true, d.alphaTest.func = 3, d.alphaTest.ref = 0.5f;
    cases.push_back({"variant memo: the alpha function alone is keyed (3 vs 4)", d, {"XE_ALPHA_REF 0.5", "XE_ALPHA_TEST 3", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.program.paramGen = true, d.program.paramGenPos = 3, d.primitiveType = 4;
    cases.push_back({"variant memo: param_gen at 3 is keyed", d, {"XE_PARAM_GEN 3", "XE_PARAM_GEN_POLYGONAL 1", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.program.paramGen = true, d.program.paramGenPos = 5, d.primitiveType = 4;
    cases.push_back({"variant memo: the param_gen position alone is keyed (5 vs 3)", d, {"XE_PARAM_GEN 5", "XE_PARAM_GEN_POLYGONAL 1", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.program.paramGen = true, d.program.paramGenPos = 3, d.primitiveType = 8;
    cases.push_back({"variant memo: the primitive type alone is keyed (rect list: not polygonal)", d,
                     {"XE_PARAM_GEN 3", "XE_PARAM_GEN_POLYGONAL 0", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.alphaToMask = 0x1AA, d.target.samples = 4;
    cases.push_back({"variant memo: alpha to mask is keyed", d, {"XE_ALPHA_TO_MASK 170u", "XE_ALPHA_TO_MASK_SAMPLES 4", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.alphaToMask = 0x1AA, d.target.samples = 2;
    cases.push_back({"variant memo: the sample count alone is keyed (2 vs 4)", d, {"XE_ALPHA_TO_MASK 170u", "XE_ALPHA_TO_MASK_SAMPLES 2", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.target.colorFormat = 1;
    cases.push_back({"variant memo: colour format 1 is keyed", d, {"XE_COLOR0_GAMMA", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.target.colorFormat = 5, d.target.colorFormatRaw = 5, d.target.colorExpBias = 0;
    cases.push_back({"variant memo: colour format 5 bias 0 is keyed", d, {"XE_COLOR0_SCALE 0.03125", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.target.colorFormat = 5, d.target.colorFormatRaw = 5, d.target.colorExpBias = -1;
    cases.push_back({"variant memo: the colour exp bias alone is keyed (-1 vs 0)", d, {"XE_COLOR0_SCALE 0.015625", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.colorWriteMask = 0;
    cases.push_back({"variant memo: the colour mask is keyed", d, {"XE_NO_COLOR_OUTPUT", "XE_TEXTURE_0_GAMMA"}});
    d = Ui(), d.kind = DrawKind::DepthOnly;
    cases.push_back({"variant memo: the kind is keyed (DepthOnly)", d, {"XE_NO_COLOR_OUTPUT", "XE_TEXTURE_0_GAMMA"}});
    const DrawState colour5 = cases[15].d;  // format 5 bias 0, for the pairwise check below
    for (const Case& c : cases)
    {
        const VariantMemoKey ck = keyOf(c.d, uvs, ups, false);
        Expect(defines(c.d, uvs, ups) && p.lines == c.ps && p.lines != gamma && ck.count != 0 && !(ck == base), c.name);
    }
    // Pairs whose difference is one input on top of a non-base state (the inputs above that only matter together with another).
    Expect(!(keyOf(cases[6].d, uvs, ups, false) == keyOf(cases[7].d, uvs, ups, false)), "variant memo: alpha ref 0.5 and 0.25 have different keys");
    Expect(!(keyOf(cases[6].d, uvs, ups, false) == keyOf(cases[8].d, uvs, ups, false)), "variant memo: alpha func 4 and 3 have different keys");
    Expect(!(keyOf(cases[9].d, uvs, ups, false) == keyOf(cases[10].d, uvs, ups, false)), "variant memo: param_gen positions 3 and 5 have different keys");
    Expect(!(keyOf(cases[9].d, uvs, ups, false) == keyOf(cases[11].d, uvs, ups, false)), "variant memo: triangle and rect lists have different keys");
    Expect(!(keyOf(cases[12].d, uvs, ups, false) == keyOf(cases[13].d, uvs, ups, false)), "variant memo: 4 and 2 samples have different keys");
    Expect(!(keyOf(colour5, uvs, ups, false) == keyOf(cases[16].d, uvs, ups, false)), "variant memo: exp bias 0 and -1 have different keys");
    Expect(!(keyOf(cases[17].d, uvs, ups, false) == keyOf(cases[18].d, uvs, ups, false)), "variant memo: a zero colour mask and DepthOnly have different keys");
    // The rect list (XE_RECT_LIST on the VS) and the renderer's XE_FETCH_UNCHECKED (added to the VS after VariantsFromDrawState).
    d = Ui(), d.hostPrimitive = HostPrimitive::RectList;
    Expect(defines(d, uvs, ups) && v.lines == std::vector<std::string>{"XE_RECT_LIST"} && !(keyOf(d, uvs, ups, false) == base),
           "variant memo: the host primitive (rect list) is keyed");
    Expect(!(keyOf(Ui(), uvs, ups, true) == base), "variant memo: the XE_FETCH_UNCHECKED decision is keyed");
    // The pair: another VS object that lists slot 0, another PS object that reads 3 interpolators.
    Expect(defines(Ui(), both, ups) && v.lines == gamma && !(keyOf(Ui(), both, ups, false) == base), "variant memo: the vertex shader is keyed");
    Expect(defines(Ui(), uvs, ps3) && v.lines == std::vector<std::string>{"XE_PS_INTERPOLATORS 3"} && !(keyOf(Ui(), uvs, ps3, false) == base),
           "variant memo: the pixel shader is keyed");

    // Fields VariantsFromDrawState does not read: the defines stay the base's (by hand: XE_TEXTURE_0_GAMMA only) and the key too.
    d = Ui(), d.scissor = {16, 16, 640, 360}, d.blend.constant[0] = 0.5f, d.depthStencil.front.ref = 0x80, d.vertexSlots[0].physical = 0x40000;
    d.textureSlots[0].physical = 0x123000, d.textureSlots[0].words[0] = 0xDEADBEEF, d.textureSlots[0].width = 256, d.aluConstants[5] = 0x3F800000;
    d.cullBack = false, d.target.pitch = 640, d.target.depthBase = 0, d.indices.count = 96;
    Expect(defines(d, uvs, ups) && v.lines.empty() && p.lines == gamma && keyOf(d, uvs, ups, false) == base,
           "variant memo: scissor, blend constant, stencil ref, vertex and texture addresses, constants, cull and pitch keep the key (a hit)");
    CONTROL(keyOf(d, uvs, ups, false) == keyOf(d, uvs, ups, true), "variant memo: the fetch decision on top of unread fields still differs");

    // More texture slots than the key holds: no key (the renderer takes the full path).
    d = Ui();
    const TextureSlot slot0 = d.textureSlots[0];
    d.textureSlots.resize(VariantMemoKey::kMaxTextures + 1, slot0);
    CONTROL(VariantMemoKeyFor(d, uvs, ups, false, k), "variant memo: 33 texture slots are not keyed");
    d.textureSlots.resize(VariantMemoKey::kMaxTextures);
    Expect(VariantMemoKeyFor(d, uvs, ups, false, k) && k.count == VariantMemoKey::kMaxWords, "variant memo: 32 texture slots fill the key exactly");

    // The memo: a held key returns the variants inserted for it; another key, nothing; a full memo is cleared whole.
    ShaderVariant a, b, c2, e;
    VariantMemo memo(2);
    Expect(memo.Find(base) == nullptr && memo.Size() == 0, "variant memo: empty, nothing held");
    memo.Insert(base, &a, &b);
    const VariantMemo::Entry* hit = memo.Find(base);
    Expect(hit != nullptr && hit->vs == &a && hit->ps == &b, "variant memo: a held key returns its two variants");
    const VariantMemoKey other = keyOf(Ui(), uvs, ups, true);
    CONTROL(memo.Find(other) != nullptr, "variant memo: another key finds nothing");
    memo.Insert(other, &c2, nullptr);
    CONTROL(memo.Find(other) != nullptr, "variant memo: an entry without both variants is not held");
    memo.Insert(other, &c2, &e);
    Expect(memo.Size() == 2 && memo.Clears() == 0, "variant memo: two entries in a memo of 2");
    const VariantMemoKey third = keyOf(cases[0].d, uvs, ups, false);
    memo.Insert(third, &e, &a);
    hit = memo.Find(third);
    Expect(memo.Size() == 1 && memo.Clears() == 1 && memo.Find(base) == nullptr && hit != nullptr && hit->vs == &e && hit->ps == &a,
           "variant memo: a third entry clears the memo of 2 whole and is held alone");
    memo.Clear();
    Expect(memo.Size() == 0 && memo.Find(third) == nullptr, "variant memo: Clear drops every entry");
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
    {
        // FM4 Recomp (alpha): an auto-indexed triangle fan of 5 as a list in Xenia's order (previous, current, first)
        // (ShiftGlue primitive_processor.h:494-512): (1 2 0)(2 3 0)(3 4 0), 9 host vertices, high index 4; a fan of 2: no triangle.
        DrawState f = Clear();
        f.hostPrimitive = HostPrimitive::TriangleFan, f.indices.count = 5;
        Expect(BuildHostIndices(f, nullptr, nullptr, h).ok && h.kind == HostIndices::Kind::Indexed &&
                   h.indices == std::vector<uint32_t>{1, 2, 0, 2, 3, 0, 3, 4, 0} && h.vertexCount == 9 && h.highIndex == 4,
               "an auto-indexed triangle fan of 5: (1 2 0)(2 3 0)(3 4 0), 9 host vertices, high 4");
        f.indices.count = 2;
        Expect(BuildHostIndices(f, nullptr, nullptr, h).ok && h.vertexCount == 0 && h.indices.empty(), "a fan of 2 indices: no triangle");
        f.indices.count = 5, f.indexOffset = 10;
        Expect(BuildHostIndices(f, nullptr, nullptr, h).ok && h.indices == std::vector<uint32_t>{11, 12, 10, 12, 13, 10, 13, 14, 10},
               "a fan with index offset 10: the offset on every index");
        Expect(KeyOf(f, Vs({0}), Ps()).topology == uint8_t(Topology::TriangleList), "a fan's pipeline topology is a triangle list");
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

// FM4 Recomp (FM4_SUBSET_MERGE): a colour-only draw's key for the open colour + depth pass (draw_record.h SubsetMergeKey). By hand from
// the clear (TRACE draw 48: colour format 2, depth format 1, 4 samples, depth always + write, stencil replace): the same clear with z
// and stencil off keys colour-only (depthUsed 0, depthFunc 0); merged with depth format 1 its attachment part (colorUsed, colour
// format, depthUsed, depthFormat, samples: pipeline_cache.cpp RenderPass's key) equals the clear's, so the pipeline is
// render-pass compatible; depth is attached but func 7 (always) with no write, stencil 0, bias 0 (no test, no write: the depth
// image is untouched); the colour state is the colour-only draw's. Controls: the merged key differs from the clear's (it does not
// write depth); a colour-only draw of another colour format does not match the clear's attachment part.
void TestSubsetMergeKey()
{
    const CachedShader vs = Vs({0}), ps = Ps();
    const PipelineKey open = KeyOf(Clear(), vs, ps);
    DrawState colourOnly = Clear();
    colourOnly.depthStencil.zEnable = false, colourOnly.depthStencil.stencilEnable = false;
    const PipelineKey c = KeyOf(colourOnly, vs, ps);
    const PipelineKey m = SubsetMergeKey(c, open.depthFormat);
    auto passPart = [](const PipelineKey& k) { return std::make_tuple(k.colorUsed, k.color[0].format, k.depthUsed, k.depthFormat, k.samples); };
    const uint8_t zero[4] = {0, 0, 0, 0};
    Expect(c.depthUsed == 0 && c.depthFunc == 0 && passPart(m) == passPart(open) && m.depthWrite == 0 && m.depthFunc == 7 && m.stencilEnable == 0 &&
               std::memcmp(m.stencilFront, zero, 4) == 0 && std::memcmp(m.stencilBack, zero, 4) == 0 && m.depthBias == 0 &&
               std::memcmp(&m.color[0], &c.color[0], sizeof c.color[0]) == 0 && m.vsHash == c.vsHash && m.psHash == c.psHash,
           "subset merge: the colour-only key gets the open pass's attachments (render-pass compatible), depth always with no write, no stencil, no bias");
    CONTROL(m == open, "the merged key is not the open pass's own (it neither tests nor writes depth)");
    DrawState other = colourOnly;
    other.target.colorFormat = 3;
    CONTROL(passPart(SubsetMergeKey(KeyOf(other, vs, ps), open.depthFormat)) == passPart(open), "another colour format is not render-pass compatible");
    // The depth-only case: the clear as DepthOnly (colour mask 0) keys without colour; merged with the clear's colour format 2 its
    // attachment part equals the clear's, colour write mask 0 with blend off (1, 0, add both), and its depth/stencil state is
    // unchanged (depth always + write, stencil replace). Control: merged with colour format 3, not compatible.
    DrawState depthOnly = Clear();
    depthOnly.kind = DrawKind::DepthOnly, depthOnly.colorWriteMask = 0;
    const PipelineKey dk = KeyOf(depthOnly, vs, ps);
    const PipelineKey dm = SubsetMergeKeyDepthOnly(dk, open.color[0].format);
    Expect(dk.colorUsed == 0 && passPart(dm) == passPart(open) && dm.color[0].writeMask == 0 && dm.color[0].srcColor == 1 && dm.color[0].dstColor == 0 &&
               dm.color[0].colorOp == 0 && dm.color[0].srcAlpha == 1 && dm.color[0].dstAlpha == 0 && dm.color[0].alphaOp == 0 && dm.depthWrite == dk.depthWrite &&
               dm.depthFunc == dk.depthFunc && dm.stencilEnable == dk.stencilEnable && std::memcmp(dm.stencilFront, dk.stencilFront, 4) == 0,
           "subset merge, depth only: the colour attachment declared with write mask 0 and no blend; the draw's depth and stencil state kept");
    CONTROL(passPart(SubsetMergeKeyDepthOnly(dk, 3)) == passPart(open), "a depth-only draw merged with another colour format is not compatible");
}

// FM4 Recomp (M7 CPU cost, FM4_CONSTANTS_PER_STAGE): SameStageConstants. The bytes a stage's shader reads from its block are the
// first vsConstantWords / psConstantWords alu words (bytes 0 to 4 x words) and the bool / loop tail (4096-4255) (shader_glsl.h
// kConstants*Offset, DrawState::vsConstantWords): a stage may be bound again exactly when those are equal. Values by hand.
void TestStageConstants()
{
    DrawState last;
    last.aluConstants.assign(2048, 0);
    for (uint32_t i = 0; i < 2048; i++)
        last.aluConstants[i] = 0x1000 + i;
    last.vsConstantWords = 8, last.psConstantWords = 4;
    last.boolConstants[0] = 0x5, last.loopConstants[31] = 0x40;
    const auto same = [&](const DrawState& d, bool pixel) {
        return SameStageConstants(d, pixel, pixel ? last.psConstantWords : last.vsConstantWords, last.aluConstants, last.boolConstants, last.loopConstants);
    };
    DrawState d = last;
    Expect(same(d, false) && same(d, true), "per-stage constants: an identical draw reuses both stages");
    d.aluConstants[1024 + 3] = 0;  // PS c0.w (read: the PS reads 4 words)
    Expect(same(d, false) && !same(d, true), "per-stage constants: a PS word it reads differs: the VS block is reused, the PS block is filled");
    d = last;
    d.aluConstants[7] = 0;  // VS c1.w, the last word the VS reads
    Expect(!same(d, false) && same(d, true), "per-stage constants: the VS's last read word differs: only the PS block is reused");
    d = last;
    d.aluConstants[8] = 0, d.aluConstants[1024 + 4] = 0;  // the first word past each stage's range (never read)
    Expect(same(d, false) && same(d, true), "per-stage constants: words past the stage's range do not matter");
    {
        // The effect: FillConstants writes the same read bytes for both draws (0-31 for the VS, 4096-4255) when SameStageConstants holds.
        uint8_t a[kConstantsBytes], b[kConstantsBytes];
        FillConstants(last, false, a), FillConstants(d, false, b);
        Expect(std::memcmp(a, b, 32) == 0 && std::memcmp(a + 4096, b + 4096, kConstantsBytes - 4096) == 0,
               "per-stage constants: the VS block's read bytes are those of the previous draw");
    }
    d = last;
    d.boolConstants[0] = 0x4;
    Expect(!same(d, false) && !same(d, true), "per-stage constants: a bool dword is in both blocks' tails: neither is reused");
    d = last;
    d.loopConstants[31] = 0;
    Expect(!same(d, false) && !same(d, true), "per-stage constants: a loop dword is in both tails: neither is reused");
    d = last;
    d.psConstantWords = 8;  // the PS now reads c1, which the previous block may hold stale
    Expect(same(d, false) && !same(d, true), "per-stage constants: a different word count is never reused");
    d = last;
    d.aluConstants.clear();  // FillConstants zero-fills such a block
    CONTROL(same(d, false) || same(d, true), "an alu vector without its 2048 words is never reused");
}

int main()
{
    TestStageConstants();
    TestSubsetMergeKey();
    TestKeys();
    TestVariants();
    TestVariantMemo();
    TestIndices();
    TestUploads();
    TestOverwrite();
    TestClearDraw();
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures == 0 ? 0 : 1;
}
