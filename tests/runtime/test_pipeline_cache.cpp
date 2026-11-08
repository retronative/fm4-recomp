// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// T0 tests of the pipeline key and the CPU-side plans (runtime/video/pipeline_key.{h,cpp}): no
// Vulkan header, no device, no compiler. The shaders are hand-built CachedShader records (the
// reflection fields the plans read), the draw state hand-built register values with bit positions
// from Xenia 67f16c4e315f registers.h / xenos.h (cited in pipeline_key.h); expected plans are
// computed by hand next to each check. Equal states must map to one key, each pipeline-relevant
// field to a different key, each irrelevant bit to the same key, and every refused state is
// paired with an accepted neighbour ("CONTROL": the negative control).
//
//   tests/run_shader_pipeline_tests.sh
#include "video/pipeline_key.h"

#include <cstdio>
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

// ---- Fixtures ------------------------------------------------------------------------------------

CachedShader MakeVs()
{
    CachedShader s;
    s.hash = 0x1111111111111111ull, s.pixel = false, s.words = 24, s.cls = kClassStraight;
    s.t.interpolatorCount = 1;
    s.t.vertexFetchSlots = {0, 95};
    s.t.vertexFetchStrides = {7, 4};
    s.t.vertexFetchEnds = {4, 2};
    return s;
}

CachedShader MakePs(uint32_t slot = 0, uint32_t dimension = 1)
{
    CachedShader s;
    s.hash = 0x2222222222222222ull, s.pixel = true, s.words = 6, s.cls = kClassStraight;
    s.t.interpolatorCount = 1;
    s.t.textureSlots = {slot};
    s.t.textureDimensions = {dimension};
    s.t.textureFilters = {{}};
    return s;
}

// 192 fetch dwords: vertex slot 0 (dwords 0-1) and slot 95 (dwords 190-191); texture slot 0 shares
// dwords 0-5 with vertex slots 0-2, so the pixel-shader fixtures use texture slot 1 (dwords 6-11).
struct Fetch
{
    uint32_t words[192] = {};
    Fetch()
    {
        // xe_gpu_vertex_fetch_t: type bits 0-1 (3 = vertex), address bits 2-31 in dwords; endian
        // bits 0-1, size bits 2-25 in dwords. Slot 0: bytes 0x1000, endian 2 (8in32), 100 dwords.
        // Slot 95: bytes 0x20000, endian 0, 1000 dwords.
        words[0] = (0x1000u >> 2) << 2 | 3u, words[1] = 100u << 2 | 2u;
        words[190] = (0x20000u >> 2) << 2 | 3u, words[191] = 1000u << 2 | 0u;
        // Texture slot 1: type 2, signs unsigned, dimension 1 (2D, dword 5 bits 9-10), identity
        // swizzle (dword 3 bits 1-12: x 0, y 1, z 2, w 3 -> 0xD10).
        words[6] = 2u, words[9] = 0xD10u, words[11] = 1u << 9;
    }
};

float alu[512 * 4] = {};
uint32_t bools[8] = {}, loops[32] = {};

DrawStateView Base(const Fetch& f)
{
    DrawStateView s;
    s.primitiveType = 4;  // triangle list
    s.indexCount = 6, s.indexed = true;
    s.rbModeControl = 4;  // colour + depth
    s.paSuScModeCntl = 1u << 1;  // cull_back
    s.rbDepthControl = 1u << 1 | 1u << 2 | 1u << 4;  // z_enable, z_write_enable, zfunc 1 (less)
    s.rbColorMask = 0xF;
    s.rbColorInfo[0] = 0;  // format 0 at bits 16-19 (8_8_8_8)
    s.rbBlendControl[0] = 0x00010001;  // (one, zero, add) colour and alpha: blending off
    s.rbSurfaceInfo = 1280;            // pitch, 1x
    s.fetchConstants = f.words;
    s.aluConstants = alu, s.boolConstants = bools, s.loopConstants = loops;
    return s;
}

PipelineKey Key(const DrawStateView& s, const CachedShader& vs, const CachedShader& ps, uint64_t vsVariant = 0, uint64_t psVariant = 0)
{
    PipelineKey k;
    const KeyResult r = DerivePipelineKey(s, vs, ps, vsVariant, psVariant, k);
    if (!r)
        std::fprintf(stderr, "  unexpected refusal: %s\n", r.reason.c_str());
    return k;
}

bool Refused(const DrawStateView& s, const CachedShader& vs, const CachedShader& ps, const char* what)
{
    PipelineKey k;
    const KeyResult r = DerivePipelineKey(s, vs, ps, 0, 0, k);
    if (r)
        return false;
    if (!Has(r.reason, what))
        std::fprintf(stderr, "  refusal \"%s\" lacks \"%s\"\n", r.reason.c_str(), what);
    return Has(r.reason, what);
}

// ---- Tests -------------------------------------------------------------------------------------

void TestKeyEquality()
{
    const Fetch f;
    const CachedShader vs = MakeVs(), ps = MakePs(1);
    const DrawStateView base = Base(f);
    const PipelineKey a = Key(base, vs, ps), b = Key(base, vs, ps);
    Expect(a == b && a.Hash() == b.Hash(), "equal states map to one key with one hash");
    Expect(a.vsHash == vs.hash && a.psHash == ps.hash && a.topology == uint8_t(Topology::TriangleList) && a.cullBack == 1 && a.cullFront == 0 &&
               a.frontClockwise == 0 && a.depthUsed == 1 && a.depthWrite == 1 && a.depthFunc == 1 && a.stencilEnable == 0 && a.colorUsed == 1 &&
               a.color[0].format == 0 && a.color[0].writeMask == 0xF && a.color[0].srcColor == 1 && a.color[0].dstColor == 0 && a.color[0].colorOp == 0 &&
               a.color[0].srcAlpha == 1 && a.color[0].dstAlpha == 0 && a.color[0].alphaOp == 0 && a.samples == 1 && a.primitiveRestart == 0 &&
               a.depthClamp == 0 && a.depthBias == 0,
           "the base key's fields are the state's");
    PipelineKey zero;
    std::memset(&zero, 0, sizeof zero);
    Expect(zero.Hash() == 0x0243cfa845185aa5ull, "the documented hash: FNV-1a-64 over the 96 bytes (Python value for all zero)");
    Expect(sizeof(PipelineKey) == 96, "the key is 96 bytes (88 + the sampler layout key)");
    Expect(a.samplerLayout == 0, "DerivePipelineKey leaves the sampler layout to PlanDescriptors (0 until set)");

    // Each pipeline-relevant field changes the key.
    struct Change
    {
        const char* name;
        void (*apply)(DrawStateView&);
    };
    const Change changes[] = {
        {"primitive type (strip)", [](DrawStateView& s) { s.primitiveType = 6; }},
        {"primitive type (points)", [](DrawStateView& s) { s.primitiveType = 1; }},
        {"primitive type (line strip)", [](DrawStateView& s) { s.primitiveType = 3; }},
        {"cull_front", [](DrawStateView& s) { s.paSuScModeCntl |= 1u; }},
        {"cull_back off", [](DrawStateView& s) { s.paSuScModeCntl &= ~2u; }},
        {"front face CW", [](DrawStateView& s) { s.paSuScModeCntl |= 1u << 2; }},
        {"poly offset front", [](DrawStateView& s) { s.paSuScModeCntl |= 1u << 11; }},
        {"poly offset back", [](DrawStateView& s) { s.paSuScModeCntl |= 1u << 12; }},
        {"depth func", [](DrawStateView& s) { s.rbDepthControl = (s.rbDepthControl & ~(7u << 4)) | 3u << 4; }},
        {"depth write off", [](DrawStateView& s) { s.rbDepthControl &= ~(1u << 2); }},
        {"depth off (stencil off too: no depth attachment)", [](DrawStateView& s) { s.rbDepthControl = 0; s.rbColorMask = 0xF; }},
        {"depth format", [](DrawStateView& s) { s.rbDepthInfo = 1u << 16; }},
        {"stencil on", [](DrawStateView& s) { s.rbDepthControl |= 1u; }},
        {"stencil front fail op", [](DrawStateView& s) { s.rbDepthControl |= 1u | 2u << 11; }},
        {"stencil back face (backface_enable, back zpass)", [](DrawStateView& s) { s.rbDepthControl |= 1u | 1u << 7 | 3u << 26; }},
        {"colour format", [](DrawStateView& s) { s.rbColorInfo[0] = 2u << 16; }},
        {"colour write mask", [](DrawStateView& s) { s.rbColorMask = 0x7; }},
        {"colour off (depth only)", [](DrawStateView& s) { s.rbColorMask = 0; }},
        {"blend src colour", [](DrawStateView& s) { s.rbBlendControl[0] = 0x00010006; }},
        {"blend op", [](DrawStateView& s) { s.rbBlendControl[0] = 0x00010001 | 1u << 5; }},
        {"blend dst alpha", [](DrawStateView& s) { s.rbBlendControl[0] = 0x0B010001; }},
        {"depth clamp (clip_disable)", [](DrawStateView& s) { s.paClClipCntl |= 1u << 16; }},
    };
    for (const Change& c : changes)
    {
        DrawStateView s = base;
        c.apply(s);
        const PipelineKey k = Key(s, vs, ps);
        std::string name = std::string("a different key for: ") + c.name;
        Expect(k != a && k.Hash() != a.Hash(), name.c_str());
    }
    {
        DrawStateView s = base;
        s.primitiveType = 6, s.paSuScModeCntl |= 1u << 21;  // strip with multi_prim_ib_ena
        DrawStateView t = base;
        t.primitiveType = 6;
        Expect(Key(s, vs, ps) != Key(t, vs, ps) && Key(s, vs, ps).primitiveRestart == 1, "a different key for: primitive restart on a strip");
    }
    CachedShader vs2 = MakeVs(), ps2 = MakePs(1);  // CachedShader owns its variants: not copyable
    vs2.hash ^= 1, ps2.hash ^= 1;
    Expect(Key(base, vs2, ps) != a && Key(base, vs, ps2) != a, "a different key for: the vertex or pixel shader hash");
    Expect(Key(base, vs, ps, 5, 0) != a && Key(base, vs, ps, 0, 5) != a && Key(base, vs, ps, 5, 0) != Key(base, vs, ps, 0, 5),
           "a different key for: the vertex or pixel variant id");
    {
        PipelineKey l = a;
        l.samplerLayout = SamplerLayoutKey(0, 1);  // the pixel shader samples slot 0
        PipelineKey m = a;
        m.samplerLayout = SamplerLayoutKey(1, 0);  // the vertex shader samples slot 0
        Expect(l != a && m != a && l != m && l.Hash() != m.Hash(), "a different key for: the sampler layout (which slots, which stage)");
    }
}

void TestKeyNormalization()
{
    const Fetch f;
    const CachedShader vs = MakeVs(), ps = MakePs(1);
    const DrawStateView base = Base(f);
    const PipelineKey a = Key(base, vs, ps);
    {
        // z_enable off: zfunc and z_write_enable are irrelevant (Xenia: compare always, no write).
        DrawStateView s = base, t = base;
        s.rbDepthControl = 1u | 1u << 2 | 5u << 4;  // stencil on, write, func 5, z_enable off
        t.rbDepthControl = 1u | 2u << 4;            // stencil on, no write, func 2, z_enable off
        const PipelineKey ks = Key(s, vs, ps), kt = Key(t, vs, ps);
        Expect(ks == kt && ks.depthFunc == 7 && ks.depthWrite == 0 && ks.depthUsed == 1, "z_enable off: zfunc/zwrite normalized to always/no write");
        CONTROL(ks == a, "it still differs from the base (depth test on)");
    }
    {
        // stencil off: every stencil field is irrelevant.
        DrawStateView s = base;
        s.rbDepthControl |= 7u << 8 | 7u << 11 | 7u << 14 | 7u << 17 | 1u << 7 | 7u << 20 | 7u << 23 | 7u << 26 | 7u << 29;
        const PipelineKey k = Key(s, vs, ps);
        Expect(k == a && k.stencilFront[0] == 0 && k.stencilBack[3] == 0, "stencil off: stencil ops/functions normalized to 0");
    }
    {
        // backface_enable off: the back state is the front state whatever the back fields hold.
        DrawStateView s = base, t = base;
        s.rbDepthControl |= 1u | 2u << 11 | 3u << 14 | 4u << 17 | 6u << 8 | 7u << 20 | 7u << 23 | 1u << 26 | 5u << 29;
        t.rbDepthControl |= 1u | 2u << 11 | 3u << 14 | 4u << 17 | 6u << 8;
        const PipelineKey ks = Key(s, vs, ps), kt = Key(t, vs, ps);
        Expect(ks == kt && ks.stencilFront[0] == 2 && ks.stencilFront[1] == 3 && ks.stencilFront[2] == 4 && ks.stencilFront[3] == 6 &&
                   std::memcmp(ks.stencilBack, ks.stencilFront, 4) == 0,
               "backface_enable off: back = front (fail 2, zpass 3, zfail 4, func 6)");
        DrawStateView u = s;
        u.rbDepthControl |= 1u << 7;
        const PipelineKey ku = Key(u, vs, ps);
        Expect(ku != ks && ku.stencilBack[0] == 7 && ku.stencilBack[1] == 1 && ku.stencilBack[2] == 5 && ku.stencilBack[3] == 7,
               "backface_enable on: the back fields count (fail 7, zpass 1, zfail 5, func 7)");
    }
    {
        // Colour write mask 0: the format and blend of target 0 are irrelevant; masks of targets 1-3 always are.
        DrawStateView s = base, t = base;
        s.rbColorMask = 0, s.rbColorInfo[0] = 2u << 16, s.rbBlendControl[0] = 0x0B0B0606;
        t.rbColorMask = 0;
        const PipelineKey ks = Key(s, vs, ps), kt = Key(t, vs, ps);
        Expect(ks == kt && ks.colorUsed == 0 && ks.color[0].format == 0 && ks.color[0].srcColor == 0, "write mask 0: colour format and blend normalized away");
        DrawStateView u = base;
        u.rbColorMask = 0xFFF0 | 0xF, u.rbColorInfo[1] = 5u << 16, u.rbBlendControl[1] = 0x12345678;
        Expect(Key(u, vs, ps) == a, "targets 1-3 (masks, formats, blends) are ignored: the shader exports colour 0 only");
    }
    {
        // multi_prim_ib_ena on a list: no restart in the key.
        DrawStateView s = base;
        s.paSuScModeCntl |= 1u << 21;
        Expect(Key(s, vs, ps) == a, "primitive restart on a list topology is normalized away");
    }
    {
        // Points and lines: cull, front face and poly offset are polygon state only.
        DrawStateView s = base, t = base;
        s.primitiveType = 1, s.paSuScModeCntl = 7u | 3u << 11;
        t.primitiveType = 1, t.paSuScModeCntl = 0;
        Expect(Key(s, vs, ps) == Key(t, vs, ps) && Key(s, vs, ps).cullBack == 0, "points: cull, face and poly offset normalized away");
    }
    {
        // A rect list is a triangle list in the key (the VS variant carries XE_RECT_LIST).
        DrawStateView s = base;
        s.primitiveType = 8;
        Expect(Key(s, vs, ps) == a, "a rect list keys as a triangle list");
    }
    {
        // Dual poly mode with triangles everywhere, or lines only on a culled face, is fill.
        DrawStateView s = base;
        s.paSuScModeCntl |= 1u << 3 | 2u << 5 | 2u << 8;
        Expect(Key(s, vs, ps) == a, "dual poly_mode with triangle polygon types is the same fill pipeline");
        DrawStateView t = base;
        t.paSuScModeCntl |= 1u << 3 | 2u << 5 | 1u << 8;  // lines on the back face, which is culled
        Expect(Key(t, vs, ps) == a, "dual poly_mode with lines on the culled face is fill");
    }
}

void TestRefusals()
{
    const Fetch f;
    const CachedShader vs = MakeVs(), ps = MakePs(1);
    const DrawStateView base = Base(f);
    PipelineKey k;
    Expect(DerivePipelineKey(base, vs, ps, 0, 0, k).ok, "the base state is accepted");
    const struct
    {
        const char* name;
        void (*apply)(DrawStateView&);
        const char* reason;
    } refused[] = {
        {"triangle fan", [](DrawStateView& s) { s.primitiveType = 5; }, "primitive type 5"},
        {"quad list", [](DrawStateView& s) { s.primitiveType = 13; }, "primitive type 13"},
        {"line loop", [](DrawStateView& s) { s.primitiveType = 12; }, "primitive type 12"},
        {"primitive type 0", [](DrawStateView& s) { s.primitiveType = 0; }, "primitive type 0"},
        {"edram_mode ignore", [](DrawStateView& s) { s.rbModeControl = 0; }, "edram_mode 0"},
        {"edram_mode copy", [](DrawStateView& s) { s.rbModeControl = 6; }, "edram_mode 6"},
        {"MSAA 2x", [](DrawStateView& s) { s.rbSurfaceInfo |= 1u << 16; }, "msaa_samples 1"},
        {"MSAA 4x", [](DrawStateView& s) { s.rbSurfaceInfo |= 2u << 16; }, "msaa_samples 2"},
        {"colour format 4 (16_16)", [](DrawStateView& s) { s.rbColorInfo[0] = 4u << 16; }, "colour format 4"},
        {"colour format 5 (16_16_16_16)", [](DrawStateView& s) { s.rbColorInfo[0] = 5u << 16; }, "colour format 5"},
        {"colour format 10", [](DrawStateView& s) { s.rbColorInfo[0] = 10u << 16; }, "colour format 10"},
        {"blend factor 2", [](DrawStateView& s) { s.rbBlendControl[0] = 0x00010002; }, "blend factor 2"},
        {"blend factor 3 (dst)", [](DrawStateView& s) { s.rbBlendControl[0] = 0x00010301; }, "blend factor 3"},
        {"blend factor 17", [](DrawStateView& s) { s.rbBlendControl[0] = 0x00110001; }, "blend factor 17"},
        {"blend op 5", [](DrawStateView& s) { s.rbBlendControl[0] = 0x00010001 | 5u << 5; }, "blend op 5"},
        {"alpha blend op 7", [](DrawStateView& s) { s.rbBlendControl[0] = 0x00010001 | 7u << 21; }, "blend op 7"},
        {"dual poly_mode, lines on the unculled front", [](DrawStateView& s) { s.paSuScModeCntl |= 1u << 3 | 1u << 5 | 2u << 8; }, "dual poly_mode"},
        {"dual poly_mode, points on the back with no culling", [](DrawStateView& s) { s.paSuScModeCntl = 1u << 3 | 2u << 5 | 0u << 8; }, "dual poly_mode"},
        {"no attachment at all", [](DrawStateView& s) { s.rbColorMask = 0, s.rbDepthControl = 0; }, "no attachment"},
    };
    for (const auto& r : refused)
    {
        DrawStateView s = base;
        r.apply(s);
        std::string name = std::string("refused: ") + r.name;
        Expect(Refused(s, vs, ps, r.reason), name.c_str());
    }
    // Neighbours of the refusals that are accepted.
    {
        DrawStateView s = base;
        s.rbColorInfo[0] = 4u << 16, s.rbColorMask = 0;  // an unmapped format while nothing is written
        Expect(DerivePipelineKey(s, vs, ps, 0, 0, k).ok, "an unmapped colour format with write mask 0 is accepted (not attached)");
        DrawStateView t = base;
        t.rbModeControl = 5;  // depth only: colour normalized away
        Expect(DerivePipelineKey(t, vs, ps, 0, 0, k).ok && k.colorUsed == 0, "edram_mode depth (5) is accepted with no colour attachment");
        DrawStateView u = base;
        u.rbBlendControl[0] = 0x100F0C06 | 4u << 5 | 3u << 21;  // src alpha, constant colour, rev-subtract; one-minus-constant-alpha, saturate, max
        Expect(DerivePipelineKey(u, vs, ps, 0, 0, k).ok && k.color[0].srcColor == 6 && k.color[0].dstColor == 12 && k.color[0].colorOp == 4 &&
                   k.color[0].srcAlpha == 15 && k.color[0].dstAlpha == 16 && k.color[0].alphaOp == 3,
               "every named blend factor and op is accepted and kept as the register holds it");
        for (const uint32_t format : {0u, 1u, 2u, 3u, 7u, 14u, 15u})
        {
            DrawStateView v = base;
            v.rbColorInfo[0] = format << 16;
            if (!DerivePipelineKey(v, vs, ps, 0, 0, k).ok)
                Expect(false, "a mapped colour format is accepted");
        }
        Expect(true, "colour formats 0, 1, 2, 3, 7, 14, 15 are accepted");
    }
}

uint32_t FloatBits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

void TestVariants()
{
    Fetch f;
    const CachedShader vs = MakeVs(), ps = MakePs(1);
    DrawStateView base = Base(f);
    ShaderVariantDefines v, p;
    Expect(DeriveVariants(base, vs, ps, v, p).ok && v.lines.empty() && p.lines.empty(), "the base state needs no defines");
    {
        DrawStateView s = base;
        s.primitiveType = 8;
        Expect(DeriveVariants(s, vs, ps, v, p).ok && v.lines == std::vector<std::string>{"XE_RECT_LIST"} && p.lines.empty(), "a rect list: XE_RECT_LIST on the VS");
    }
    {
        DrawStateView s = base;
        s.rbColorControl = 1u << 3 | 4u, s.alphaRef = 0.5f;  // alpha test, func 4 (greater)
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines == std::vector<std::string>{"XE_ALPHA_REF 0.5", "XE_ALPHA_TEST 4"},
               "alpha test: XE_ALPHA_TEST 4 and XE_ALPHA_REF 0.5 on the PS");
        s.alphaRef = 1.0f;
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines[0] == "XE_ALPHA_REF 1.0", "an integral reference is written as a float literal");
        s.alphaRef = 0.1f;
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines[0] == "XE_ALPHA_REF 0.100000001", "the reference keeps its exact float value (%.9g)");
        s.rbColorControl = 4u;  // enable bit clear
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines.empty(), "alpha test disabled: no defines");
    }
    {
        DrawStateView s = base;
        s.rbColorInfo[0] = 1u << 16;
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines == std::vector<std::string>{"XE_COLOR0_GAMMA"}, "colour format 1: XE_COLOR0_GAMMA");
        s.rbColorInfo[0] = 3u << 16;
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines == std::vector<std::string>{"XE_COLOR0_7E3"}, "colour format 3: XE_COLOR0_7E3");
        s.rbColorMask = 0;
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines.empty(), "no colour write: no colour define");
    }
    // Texture defines from the fetch constant of slot 1 (dwords 6-11).
    {
        Fetch g = f;
        g.words[6] = 2u | 3u << 2 | 3u << 4 | 3u << 6;  // signs x, y, z gamma (dword 0 bits 2-7), w unsigned
        DrawStateView s = Base(g);
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines == std::vector<std::string>{"XE_TEXTURE_1_GAMMA"}, "gamma signs on x, y, z: XE_TEXTURE_1_GAMMA");
        g.words[9] = 0xD10u | (uint32_t(-2) & 63u) << 13;  // exp_adjust -2 (dword 3 bits 13-18)
        g.words[10] = 16u << 12;                            // lod_bias 16/32 = 0.5 (dword 4 bits 12-21)
        s = Base(g);
        Expect(DeriveVariants(s, vs, ps, v, p).ok &&
                   p.lines == std::vector<std::string>{"XE_TEXTURE_1_GAMMA", "XE_TEXTURE_1_LOD_BIAS 0.5", "XE_TEXTURE_1_SCALE ldexp(1.0, -2)"},
               "exp_adjust -2 and lod_bias 16: SCALE ldexp(1.0, -2) and LOD_BIAS 0.5");
        g.words[10] = (uint32_t(-32) & 0x3FFu) << 12;  // lod_bias -32 = -1 LOD
        s = Base(g);
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines[1] == "XE_TEXTURE_1_LOD_BIAS -1.0", "a negative lod_bias is sign-extended (-32 -> -1.0)");
        // Swizzle (w, 1, x, y): x <- w (unsigned), y <- 1, z <- x (gamma): mixed -> refused.
        g.words[9] = (3u | 5u << 3 | 0u << 6 | 1u << 9) << 1;
        s = Base(g);
        KeyResult r = DeriveVariants(s, vs, ps, v, p);
        Expect(!r && Has(r.reason, "mixed"), "gamma and non-gamma channels mixed through the swizzle: refused");
        // Swizzle (x, 1, 0, w): x gamma, y and z constants: gamma (constants are neutral).
        g.words[9] = (0u | 5u << 3 | 4u << 6 | 3u << 9) << 1;
        g.words[10] = 0;  // no LOD bias
        s = Base(g);
        Expect(DeriveVariants(s, vs, ps, v, p).ok && p.lines == std::vector<std::string>{"XE_TEXTURE_1_GAMMA"}, "constant channels are neutral for gamma");
        g.words[9] = (6u << 3) << 1;
        s = Base(g);
        r = DeriveVariants(s, vs, ps, v, p);
        Expect(!r && Has(r.reason, "swizzle value 6"), "swizzle value 6 is refused");
    }
    {
        Fetch g = f;
        g.words[6] = 3u;  // a vertex fetch constant where a texture is expected
        DrawStateView s = Base(g);
        KeyResult r = DeriveVariants(s, vs, ps, v, p);
        Expect(!r && Has(r.reason, "not a texture fetch constant"), "a non-texture fetch constant is refused");
        g = f, g.words[11] = 2u << 9;  // dimension 3D for a 2D sampler
        s = Base(g);
        r = DeriveVariants(s, vs, ps, v, p);
        Expect(!r && Has(r.reason, "samples it as 2D but the constant is dimension 2"), "a 3D constant for a 2D sampler is refused");
        g = f, g.words[7] = 1u << 10;  // stacked
        s = Base(g);
        r = DeriveVariants(s, vs, ps, v, p);
        Expect(!r && Has(r.reason, "stacked"), "a stacked 2D constant for a 2D sampler is refused");
        const CachedShader ps3 = MakePs(1, 2);
        g = f, g.words[11] = 1u << 9, g.words[7] = 1u << 10;  // stacked 2D fetched as 3D
        s = Base(g);
        r = DeriveVariants(s, vs, ps3, v, p);
        Expect(!r && Has(r.reason, "stacked 2D texture fetched as 3D"), "a stacked 2D texture fetched as 3D is refused");
        g.words[11] = 2u << 9, g.words[7] = 0;
        s = Base(g);
        Expect(DeriveVariants(s, vs, ps3, v, p).ok, "a 3D constant for a 3D sampler is accepted");
        const CachedShader cube = MakePs(1, 3);
        g.words[11] = 3u << 9;
        s = Base(g);
        Expect(DeriveVariants(s, vs, cube, v, p).ok, "a cube constant for a cube sampler is accepted");
        g.words[11] = 1u << 9;
        s = Base(g);
        r = DeriveVariants(s, vs, cube, v, p);
        Expect(!r && Has(r.reason, "as a cube"), "a 2D constant for a cube sampler is refused");
    }
    {
        DrawStateView s = base;
        s.fetchConstants = nullptr;
        Expect(!DeriveVariants(s, vs, ps, v, p).ok, "no fetch constants: refused");
    }
    (void)FloatBits;
}

void TestVertexPlans()
{
    const Fetch f;
    const CachedShader vs = MakeVs();
    const DrawStateView s = Base(f);
    std::vector<VertexSlotPlan> plans;
    uint32_t total = 0;
    // highIndex 3: slot 0 needs 3 * 7 + 4 = 25 of 100 dwords; slot 95 needs 3 * 4 + 2 = 14 of 1000.
    Expect(PlanVertexSlots(s, vs, 3, plans, total).ok && plans.size() == 2 && total == 39, "two slots planned, 39 dwords in all");
    Expect(plans[0].slot == 0 && plans[0].guestAddress == 0x1000 && plans[0].endian == 2 && plans[0].sizeDwords == 100 && plans[0].copyDwords == 25 &&
               plans[0].baseDword == 0,
           "slot 0: address 0x1000, endian 8in32, size 100, copy 25 at base 0");
    Expect(plans[1].slot == 95 && plans[1].guestAddress == 0x20000 && plans[1].endian == 0 && plans[1].sizeDwords == 1000 && plans[1].copyDwords == 14 &&
               plans[1].baseDword == 25,
           "slot 95: address 0x20000, size 1000, copy 14 at base 25");
    // highIndex 20: slot 0 would need 144 > 100: the copy is clamped to the constant's size.
    Expect(PlanVertexSlots(s, vs, 20, plans, total).ok && plans[0].copyDwords == 100 && plans[1].copyDwords == 82 && total == 182,
           "the copy never exceeds the constant's size (100), slot 95 copies 20 * 4 + 2 = 82");
    Expect(PlanVertexSlots(s, vs, 0, plans, total).ok && plans[0].copyDwords == 4 && plans[1].copyDwords == 2, "index 0 alone needs only the element's end");
    {
        Fetch g = f;
        g.words[0] = (0x1000u >> 2) << 2 | 2u;  // type 2 (texture)
        const DrawStateView t = Base(g);
        const KeyResult r = PlanVertexSlots(t, vs, 3, plans, total);
        Expect(!r && Has(r.reason, "not a vertex fetch constant (type 2)"), "a texture constant in a vertex slot is refused");
    }
    {
        Fetch g = f;
        g.words[191] = 0;  // size 0
        const DrawStateView t = Base(g);
        const KeyResult r = PlanVertexSlots(t, vs, 3, plans, total);
        Expect(!r && Has(r.reason, "slot 95") && Has(r.reason, "size 0"), "a size-0 constant is refused");
    }
    {
        Fetch g = f;
        g.words[0] = 0x1FFFFF00u | 3u;  // 0x1FFFFF00 + 400 bytes > 512 MB
        const DrawStateView t = Base(g);
        const KeyResult r = PlanVertexSlots(t, vs, 3, plans, total);
        Expect(!r && Has(r.reason, "512 MB"), "a range past the physical space is refused");
        g.words[0] = 0x1FFFFE70u | 3u;  // 0x1FFFFE70 + 400 = 0x20000000 exactly: accepted
        const DrawStateView u = Base(g);
        Expect(PlanVertexSlots(u, vs, 3, plans, total).ok && plans[0].guestAddress == 0x1FFFFE70u, "a range ending exactly at 512 MB is accepted");
    }
    {
        DrawStateView t = s;
        t.fetchConstants = nullptr;
        Expect(!PlanVertexSlots(t, vs, 3, plans, total).ok, "no fetch constants: refused");
    }
    CachedShader none = MakeVs();
    none.t.vertexFetchSlots.clear(), none.t.vertexFetchStrides.clear(), none.t.vertexFetchEnds.clear();
    Expect(PlanVertexSlots(s, none, 3, plans, total).ok && plans.empty() && total == 0, "a shader without vertex fetches plans nothing");
}

// The M5 through MoltenVK 1.4.2 (OBSERVED the local capture draw_loop3 (not distributed) "[gpu] device Apple M5 ... per-stage samplers 16,
// sampled images 256"); the per-stage resource and per-set values are CONSTRUCTED (large enough not to bind here).
SamplerLimits M5Limits()
{
    SamplerLimits l;
    l.perStageSamplers = 16, l.perStageSampledImages = 256, l.perStageResources = 1024, l.setSamplers = 1024, l.setSampledImages = 1024;
    return l;
}

void SetTextures(CachedShader& s, std::vector<uint32_t> slots, std::vector<uint32_t> dims)
{
    s.t.textureSlots = std::move(slots), s.t.textureDimensions = std::move(dims);
    s.t.textureFilters.assign(s.t.textureSlots.size(), {});
}

void TestDescriptors()
{
    const SamplerLimits m5 = M5Limits();
    CachedShader vs = MakeVs();
    CachedShader ps = MakePs(0, 1);
    SetTextures(ps, {0, 1, 15}, {1, 2, 1});
    DescriptorPlan plan;
    // Slots 0, 1, 15 from the PS: layout key bits 2s + 1 = 1, 3, 31 -> (1 << 31) | 0xA.
    Expect(PlanDescriptors(vs, ps, m5, plan).ok && plan.dimension[0] == 1 && plan.dimension[1] == 2 && plan.dimension[2] == 0 && plan.dimension[15] == 1 &&
               plan.vsSlots == 0 && plan.psSlots == 0x8003 && plan.layoutKey == (1ull << 31 | 0xA),
           "the plan holds the pixel shader's slots 0, 1 and 15 with their dimensions; layout key bits 31, 3, 1 (fragment bits)");
    // Draw 31269 (the local capture draw_loop3 (not distributed), run1.err "vertex shader texture slot 17"): VS 48b7493f34f9ef46 fetches
    // texture slot 17 as 2D, PS cabc1bc4a6d34db8 slots 1 and 0 as 2D (census report.tsv texture_slots 17 / 0,1; dimension field 1 in
    // word 2 bits 14-15 of the tfetches A9181001 0F1C0FF8 00004000, 10183001 ... 00004000, 10082001 ... 00004000, decoded by hand).
    // Layout key: bit 2 * 17 = 34 (VS) and bits 1, 3 (PS) -> 0x40000000A.
    SetTextures(vs, {17}, {1});
    SetTextures(ps, {0, 1}, {1, 1});
    Expect(PlanDescriptors(vs, ps, m5, plan).ok && plan.vsSlots == 1u << 17 && plan.psSlots == 3 && plan.dimension[17] == 1 &&
               plan.layoutKey == 0x40000000Aull && LayoutVsSlots(plan.layoutKey) == 1u << 17 && LayoutPsSlots(plan.layoutKey) == 3,
           "draw 31269: vertex texture slot 17 is bound (vertex-visible binding 25), the PS's 0 and 1 fragment-visible");
    // One slot both stages fetch: one binding visible to both (bits 2s and 2s + 1).
    SetTextures(vs, {1}, {1});
    Expect(PlanDescriptors(vs, ps, m5, plan).ok && plan.layoutKey == (0x4ull | 0x8ull | 0x2ull), "slot 1 from both stages: key bits 2 and 3 (+ slot 0's 1)");
    SetTextures(vs, {1}, {2});
    KeyResult r = PlanDescriptors(vs, ps, m5, plan);
    Expect(!r && Has(r.reason, "slot 1") && Has(r.reason, "dimension 2") && Has(r.reason, "and 1"), "one slot sampled as 3D and 2D is refused");
    // Every slot 0-31 can be bound now: slot 31 (binding 39) from the PS.
    SetTextures(vs, {}, {});
    SetTextures(ps, {31}, {3});
    Expect(PlanDescriptors(vs, ps, m5, plan).ok && plan.layoutKey == (1ull << 63) && plan.dimension[31] == 3, "slot 31 (a cube) is bound: key bit 63");
    SetTextures(ps, {0, 32}, {1, 1});
    r = PlanDescriptors(vs, ps, m5, plan);
    Expect(!r && Has(r.reason, "slot 32") && Has(r.reason, "out of range"), "slot 32 (past Xenos's 32 fetch constants) is refused");
    SetTextures(ps, {0, 1}, {1, 4});
    r = PlanDescriptors(vs, ps, m5, plan);
    Expect(!r && Has(r.reason, "out of range"), "a dimension beyond 3 is refused");
    // The per-stage limit: 16 fragment samplers fit, 17 do not; the vertex stage counts separately (16 + 16 distinct slots fit).
    std::vector<uint32_t> s16, s17, d16(16, 1), d17(17, 1), v16;
    for (uint32_t i = 0; i < 16; i++)
        s16.push_back(i), v16.push_back(16 + i);
    s17 = s16, s17.push_back(16);
    SetTextures(ps, s16, d16);
    SetTextures(vs, v16, d16);
    Expect(PlanDescriptors(vs, ps, m5, plan).ok && plan.psSlots == 0xFFFF && plan.vsSlots == 0xFFFF0000u, "16 PS + 16 VS samplers fit 16 per stage");
    SetTextures(ps, s17, d17);
    SetTextures(vs, {}, {});
    r = PlanDescriptors(vs, ps, m5, plan);
    Expect(!r && Has(r.reason, "pixel shader samples 17") && Has(r.reason, "per-stage limit of 16"), "CONTROL: 17 pixel samplers exceed the M5's 16, refused by name");
    SetTextures(ps, {}, {});
    SetTextures(vs, s17, d17);
    r = PlanDescriptors(vs, ps, m5, plan);
    Expect(!r && Has(r.reason, "vertex shader samples 17"), "CONTROL: 17 vertex samplers are refused by name");
    // A slot both stages fetch counts in each stage: 16 shared slots fill both budgets (fit); a 17th in the VS does not.
    SetTextures(ps, s16, d16);
    SetTextures(vs, s16, d16);
    Expect(PlanDescriptors(vs, ps, m5, plan).ok, "16 slots fetched by both stages fit");
    // The limits are the smaller of samplers and sampled images; maxPerStageResources counts the stage's buffers (VS 5, PS 1).
    SamplerLimits tight = m5;
    tight.perStageSampledImages = 2;
    SetTextures(vs, {}, {});
    SetTextures(ps, {0, 1, 2}, {1, 1, 1});
    r = PlanDescriptors(vs, ps, tight, plan);
    Expect(!r && Has(r.reason, "per-stage limit of 2"), "the sampled-image limit applies to combined image samplers");
    tight = m5, tight.perStageResources = 7;  // VS: 5 buffers + 3 samplers = 8 > 7
    SetTextures(vs, {0, 1, 2}, {1, 1, 1});
    SetTextures(ps, {}, {});
    r = PlanDescriptors(vs, ps, tight, plan);
    Expect(!r && Has(r.reason, "vertex stage needs 8 resources"), "maxPerStageResources counts the vertex stage's 5 buffers");
    SetTextures(vs, {0, 1}, {1, 1});
    Expect(PlanDescriptors(vs, ps, tight, plan).ok, "CONTROL: 5 + 2 = 7 resources fit");
    tight = m5, tight.setSamplers = 2;
    SetTextures(vs, {0, 1}, {1, 1});
    SetTextures(ps, {2}, {1});
    r = PlanDescriptors(vs, ps, tight, plan);
    Expect(!r && Has(r.reason, "3 distinct texture slots") && Has(r.reason, "per-set limit of 2"), "the per-set sampler limit counts distinct slots");
    Expect(SamplerLayoutKey(0, 0) == 0 && SamplerLayoutKey(1u << 31, 1u << 31) == (3ull << 62) && LayoutVsSlots(0xAAAAAAAAAAAAAAAAull) == 0 &&
               LayoutPsSlots(0xAAAAAAAAAAAAAAAAull) == 0xFFFFFFFFu,
           "the layout key packs 2 bits per slot (VS low, PS high)");
    Expect(kTextureBindings == 32 && kBindingTexture0 == 8 && kBindingVertexData == 0 && kBindingSystem == 1 && kBindingVsConstants == 2 && kBindingPsConstants == 3 &&
               kBindingIndices == 4 && kBindingErrors == 5,
           "the bindings are shader_glsl.h's (8 + slot for slots 0-31)");
    Expect(kSystemBytes == 1200 && kSystemVertexBaseOffset == 768 && kSystemViewportScaleOffset == 1152 && kSystemViewportOffsetOffset == 1168 &&
               kSystemFlagsOffset == 1184 && kConstantsBoolOffset == 4096 && kConstantsLoopOffset == 4128 && kConstantsBytes == 4256,
           "block layouts: std140 offsets of shader_glsl.h (48 + 24 uvec4, two vec4, a uint; 256 vec4, 2 + 8 uvec4)");
}

void TestUploadRing()
{
    UploadRing ring(1024, 256, 16);
    uint64_t at = ~0ull;
    Expect(ring.Allocate(100, false, at) && at == 0 && ring.Used() == 100, "the first storage block is at 0");
    Expect(ring.Allocate(100, true, at) && at == 256 && ring.Used() == 356, "a uniform block is aligned up to 256");
    Expect(ring.Allocate(100, false, at) && at == 368 && ring.Used() == 468, "a storage block is aligned up to 16 (356 -> 368)");
    Expect(!ring.Allocate(1000, false, at) && ring.Used() == 468 && ring.Allocations() == 3, "an allocation past the capacity is refused and changes nothing");
    Expect(ring.Allocate(544, false, at) && at == 480 && ring.Used() == 1024, "the ring can be filled exactly (468 -> 480 aligned, + 544)");
    Expect(!ring.Allocate(1, false, at), "a full ring refuses one more byte");
    ring.Reset();
    Expect(ring.Used() == 0 && ring.Allocations() == 0 && ring.Resets() == 1 && ring.Allocate(1024, false, at) && at == 0, "after a reset the whole ring is free");
    UploadRing odd(1024, 3, 0);
    Expect(!odd.Allocate(4, true, at) && !odd.Allocate(4, false, at), "a non-power-of-two or zero alignment is refused");
    UploadRing small(16, 16, 16);
    Expect(small.Allocate(16, true, at) && at == 0 && !small.Allocate(1, true, at), "a 16-byte ring holds one 16-byte block");
    CONTROL(UploadRing(1024, 256, 16).Allocate(1025, false, at), "more than the capacity never fits");
}

}  // namespace

int main()
{
    TestKeyEquality();
    TestKeyNormalization();
    TestRefusals();
    TestVariants();
    TestVertexPlans();
    TestDescriptors();
    TestUploadRing();
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures == 0 ? 0 : 1;
}
