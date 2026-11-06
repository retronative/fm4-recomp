// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// T0 tests of the shader cache (runtime/video/shader_cache.{h,cpp}), the class tag (shader_class.h)
// and the shader hash header (kernel/shader_hash.h), CPU only: the glslang library compiles GLSL to
// SPIR-V in-process; no Vulkan header, no device. Expected values are derived independently: the
// hashes by Python (big integers, the recipe of cp_trace.h), the SPIR-V header words from the SPIR-V
// specification (magic 0x07230203, version 1.0 = 0x00010000), the class tags from the control-flow
// opcodes each program was assembled with. The microcode encoders and the three programs (ColourPs,
// ClearVs, LoopPs, BranchesVs) are copied from tests/shader/test_shader_glsl.cpp (ForzaRecomp
// tests/gpu/test_shader_glsl.cpp, GPL-3.0-or-later, same owner), which keeps them in an
// anonymous namespace with its own main. "CONTROL" names a negative control.
//
//   tests/run_shader_pipeline_tests.sh   (argv[1]: a fresh directory for the dump check)
#include "kernel/shader_hash.h"
#include "shader/ucode.h"
#include "video/shader_cache.h"
#include "video/shader_class.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace runtime::video;
using namespace runtime::shader;

namespace {

int failures = 0, controls = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}
#define CONTROL(x, name) (controls++, Expect(!(x), "control: " name))

bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

// ---- Encoders (tests/shader/test_shader_glsl.cpp; bit positions from ucode.h) -------------------

struct Cf
{
    uint32_t w0, w1;
};
Cf ExecCf(uint32_t address, uint32_t count, uint32_t sequence, bool end = false, bool clean = false)
{
    return {address | count << 12 | sequence << 16, uint32_t(clean) << 9 | uint32_t(end ? 2 : 1) << 12};
}
Cf PredExecCf(uint32_t address, uint32_t count, bool condition, bool clean = true, uint32_t sequence = 0)
{
    return {address | count << 12 | sequence << 16, uint32_t(clean) << 9 | uint32_t(condition) << 10 | 5u << 12};
}
Cf CondExecCf(uint32_t address, uint32_t count, uint32_t boolIndex, bool condition, uint32_t sequence = 0)
{
    return {address | count << 12 | sequence << 16, boolIndex << 2 | uint32_t(condition) << 10 | 13u << 12};
}
Cf AllocCf(uint32_t type) { return {0, type << 9 | 12u << 12}; }
Cf LoopStartCf(uint32_t address, uint32_t id) { return {address | id << 16, 7u << 12}; }
Cf LoopEndCf(uint32_t address, uint32_t id) { return {address | id << 16, 1u << 10 | 8u << 12}; }
Cf OpCf(uint32_t opcode, uint32_t word0 = 0) { return {word0, opcode << 12}; }
void PackCf(std::vector<uint32_t>& p, Cf a, Cf b)
{
    p.push_back(a.w0);
    p.push_back((a.w1 & 0xFFFF) | b.w0 << 16);
    p.push_back(b.w0 >> 16 | b.w1 << 16);
}
uint32_t Swz(const char* s)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        const uint32_t c = s[i] == 'x' ? 0 : s[i] == 'y' ? 1 : s[i] == 'z' ? 2 : 3;
        v |= ((c - i) & 3) << (2 * i);
    }
    return v;
}
struct Src
{
    bool temp = true;
    uint32_t reg = 0, swz = 0;
    bool neg = false;
};
Src T(uint32_t r, const char* s = "xyzw") { return {true, r, Swz(s), false}; }
Src C(uint32_t r, const char* s = "xyzw") { return {false, r, Swz(s), false}; }
struct Alu
{
    uint32_t vop = 2, sop = 50, vdest = 0, sdest = 0, vmask = 0, smask = 0;
    bool exp = false, sdestRel = false, vclamp = false, sclamp = false, absConst = false, pred = false, c0rel = false, predCond = false;
    Src s1, s2, s3;
};
void Put(std::vector<uint32_t>& p, const Alu& a)
{
    p.push_back(a.vdest | uint32_t(a.absConst) << 7 | a.sdest << 8 | uint32_t(a.sdestRel) << 14 | uint32_t(a.exp) << 15 | a.vmask << 16 |
                a.smask << 20 | uint32_t(a.vclamp) << 24 | uint32_t(a.sclamp) << 25 | a.sop << 26);
    p.push_back(a.s3.swz | a.s2.swz << 8 | a.s1.swz << 16 | uint32_t(a.s3.neg) << 24 | uint32_t(a.s2.neg) << 25 | uint32_t(a.s1.neg) << 26 |
                uint32_t(a.predCond) << 27 | uint32_t(a.pred) << 28 | uint32_t(a.c0rel) << 31);
    p.push_back(a.s3.reg | a.s2.reg << 8 | a.s1.reg << 16 | a.vop << 24 | uint32_t(a.s3.temp) << 29 | uint32_t(a.s2.temp) << 30 |
                uint32_t(a.s1.temp) << 31);
}
uint32_t DSwz(const char* s)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        const char c = s[i];
        const uint32_t code = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : c == '0' ? 4 : c == '1' ? 5 : c == '?' ? 6 : 7;
        v |= code << (3 * i);
    }
    return v;
}
struct VFetch
{
    uint32_t src = 0, srcComp = 0, dest = 0, slot = 0, format = 37, stride = 0, dswz = 0;
    int32_t offset = 0, expAdjust = 0;
    bool mini = false, pred = false, rounded = false, sgn = true, integer = false, rf = false;
};
void Put(std::vector<uint32_t>& p, const VFetch& f)
{
    p.push_back(f.src << 5 | f.dest << 12 | 1u << 19 | (f.slot / 3) << 20 | (f.slot % 3) << 25 | f.srcComp << 30);
    p.push_back(f.dswz | uint32_t(f.sgn) << 12 | uint32_t(f.integer) << 13 | uint32_t(f.rf) << 14 | uint32_t(f.rounded) << 15 | f.format << 16 |
                (uint32_t(f.expAdjust) & 63) << 24 | uint32_t(f.mini) << 30 | uint32_t(f.pred) << 31);
    p.push_back(f.stride | (uint32_t(f.offset) & 0x7FFFFF) << 8);
}
struct TFetch
{
    uint32_t src = 0, dest = 0, slot = 0, srcSwz = 0x4, dswz = 0x688, mag = 3, min = 3, mip = 3, aniso = 7, arb = 0, vmag = 3, vmin = 3, dim = 1;
    bool valid = true, unnorm = false, compLod = true, regLod = false, regGrad = false, pred = false;
    int32_t bias = 0, ox = 0, oy = 0, oz = 0;
};
void Put(std::vector<uint32_t>& p, const TFetch& t)
{
    p.push_back(1u | t.src << 5 | t.dest << 12 | uint32_t(t.valid) << 19 | t.slot << 20 | uint32_t(t.unnorm) << 25 | t.srcSwz << 26);
    p.push_back(t.dswz | t.mag << 12 | t.min << 14 | t.mip << 16 | t.aniso << 18 | t.arb << 21 | t.vmag << 24 | t.vmin << 26 |
                uint32_t(t.compLod) << 28 | uint32_t(t.regLod) << 29 | uint32_t(t.pred) << 31);
    p.push_back(uint32_t(t.regGrad) | (uint32_t(t.bias) & 0x7F) << 2 | t.dim << 14 | (uint32_t(t.ox) & 31) << 16 | (uint32_t(t.oy) & 31) << 21 |
                (uint32_t(t.oz) & 31) << 26);
}

// ---- Programs (tests/shader/test_shader_glsl.cpp 1, 2, 8, 9) ----------------------------------------

std::vector<uint32_t> ClearVs()
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(3, 2, 0x5), AllocCf(2));
    PackCf(p, ExecCf(5, 1, 0), AllocCf(1));
    PackCf(p, ExecCf(6, 1, 0), ExecCf(7, 1, 0, true));
    VFetch full;
    full.dest = 1, full.slot = 0, full.format = 57, full.stride = 7, full.offset = 0, full.dswz = DSwz("xyz1");
    Put(p, full);
    VFetch mini;
    mini.dest = 0, mini.format = 38, mini.offset = 3, mini.mini = true, mini.dswz = DSwz("xyzw");
    Put(p, mini);
    Alu e0;
    e0.exp = true, e0.vdest = 0, e0.vmask = 0xF, e0.s1 = T(0), e0.s2 = T(0), e0.s3 = T(0);
    Put(p, e0);
    Alu e1 = e0;
    e1.vdest = 62, e1.s1 = T(1), e1.s2 = T(1);
    Put(p, e1);
    Put(p, Alu{});
    return p;
}

std::vector<uint32_t> ColourPs()
{
    std::vector<uint32_t> p;
    PackCf(p, AllocCf(2), ExecCf(1, 1, 0, true));
    Alu e;
    e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(0), e.s2 = T(0), e.s3 = T(0);
    Put(p, e);
    return p;
}

std::vector<uint32_t> BranchesVs()
{
    std::vector<uint32_t> p;
    PackCf(p, AllocCf(1), ExecCf(3, 1, 0));
    PackCf(p, CondExecCf(4, 1, 5, true), CondExecCf(5, 1, 5, false));
    PackCf(p, ExecCf(6, 1, 0, true), OpCf(0));
    Alu copy;
    copy.vdest = 1, copy.vmask = 0x1, copy.s1 = T(0, "xxxx"), copy.s2 = T(0, "xxxx"), copy.s3 = T(0, "xxxx");
    Put(p, copy);
    Alu a = copy;
    a.vdest = 2, a.s1 = T(1, "xxxx"), a.s2 = T(1, "xxxx");
    Put(p, a);
    Alu b = copy;
    b.vdest = 2;
    Put(p, b);
    Alu e;
    e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(2, "xxxx"), e.s2 = T(2, "xxxx"), e.s3 = T(2, "xxxx");
    Put(p, e);
    return p;
}

std::vector<uint32_t> LoopPs()
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(3, 1, 0), LoopStartCf(4, 31));
    PackCf(p, ExecCf(4, 3, 0x4), LoopEndCf(2, 31));
    PackCf(p, AllocCf(2), ExecCf(7, 1, 0, true));
    Alu a;
    a.vop = 0, a.vdest = 1, a.vmask = 0xF, a.s1 = T(0), a.s2 = C(0), a.s3 = T(0);
    Put(p, a);
    Alu m;
    m.vop = 11, m.vdest = 0, m.vmask = 0x5, m.s1 = T(0, "zzzz"), m.s2 = C(1), m.s3 = C(2);
    Put(p, m);
    TFetch f;
    f.dest = 2, f.mag = 1, f.min = 1;
    Put(p, f);
    Alu s;
    s.vop = 0, s.vdest = 1, s.vmask = 0xF, s.s1 = T(2), s.s2 = T(1), s.s3 = T(0);
    Put(p, s);
    Alu e;
    e.vop = 1, e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(1), e.s2 = C(3, "zzzz"), e.s3 = T(0);
    Put(p, e);
    return p;
}

// Control-flow-only programs for the class scan (never translated).
std::vector<uint32_t> CfOnly(Cf a, Cf b)
{
    std::vector<uint32_t> p;
    PackCf(p, a, b);
    Put(p, Alu{});
    return p;
}

std::string work;
std::string ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

// A compiler that fails every request with a fixed text, for the error path.
struct FailingCompiler final : SpirvCompiler
{
    int calls = 0;
    bool Compile(bool, const std::string&, std::vector<uint32_t>&, std::string&, std::string& error) override
    {
        calls++;
        error = "boom: a fake compiler failure";
        return false;
    }
};

void TestHash()
{
    // Python: FNV-1a-64, seed cbf29ce484222325 ^ type, each word loaded little-endian from its
    // big-endian bytes (0x12345678 -> 0x78563412).
    const uint32_t one[] = {0x12345678u}, two[] = {0x12345678u, 0x9abcdef0u};
    Expect(runtime::gpu::ShaderHash(0, one, 1) == 0x0157d018f9437275ull, "ShaderHash of one word (vertex) is the Python value");
    Expect(runtime::gpu::ShaderHash(1, two, 2) == 0xe6013dd857223988ull, "ShaderHash of two words (pixel) is the Python value");
    CONTROL(runtime::gpu::ShaderHash(1, one, 1) == 0x0157d018f9437275ull, "the type changes the hash");
    CONTROL(runtime::gpu::ShaderHash(0, two, 2) == 0xe6013dd857223988ull, "the type changes the hash (pixel words as vertex)");
    ShaderVariantDefines d;
    d.Add("XE_RECT_LIST");
    d.Add("XE_ALPHA_TEST 4");
    ShaderVariantDefines e;
    e.Add("XE_ALPHA_TEST 4");
    e.Add("XE_RECT_LIST");
    e.Add("XE_RECT_LIST");  // a duplicate is one line
    Expect(d.Id() == 0xb5da53780200f113ull && e.Id() == d.Id() && e.lines.size() == 2, "variant id: sorted lines joined by \\n, order-independent, distinct");
    ShaderVariantDefines single;
    single.Add("XE_RECT_LIST");
    Expect(single.Id() == 0x82'3f50e0'861d1442ull /* FNV-1a-64 of the one line; digit separators keep tests/test_no_title_addresses.py's title-address pattern from matching a hash */ && ShaderVariantDefines{}.Id() == 0, "variant id of one line, and 0 for none");
    Expect(VariantGlsl("#version 450\nvoid main() {}\n", single) == "#version 450\n#define XE_RECT_LIST\nvoid main() {}\n",
           "the defines are inserted after line 1");
    Expect(VariantGlsl("#version 450\nvoid main() {}\n", {}) == "#version 450\nvoid main() {}\n", "no defines: the GLSL unchanged");
}

void TestClass()
{
    const auto cls = [](const std::vector<uint32_t>& p) { return ScanShaderClass(p.data(), uint32_t(p.size())); };
    Expect(cls(ColourPs()) == kClassStraight && cls(ClearVs()) == kClassStraight, "ColourPs and ClearVs are straight-line");
    Expect(cls(LoopPs()) == kClassLoop, "LoopPs is class loop");
    Expect(cls(BranchesVs()) == kClassBranch, "BranchesVs (bool cexecs) is class branch");
    CONTROL(cls(LoopPs()) == kClassStraight, "a loop shader is not straight-line");
    Expect(cls(CfOnly(OpCf(11, 2), ExecCf(1, 1, 0, true))) == kClassBranch, "a cond_jmp is class branch");
    Expect(cls(CfOnly(PredExecCf(1, 1, true), ExecCf(1, 1, 0, true))) == kClassPredicate, "a predicated exec is class predicate");
    Expect(cls(CfOnly(OpCf(9, 2), ExecCf(1, 1, 0, true))) == kClassCall, "a cond_call is class call");
    Expect(cls(CfOnly(OpCf(10), ExecCf(1, 1, 0, true))) == kClassCall, "a return is class call");
    Expect(cls(CfOnly(OpCf(15), ExecCf(1, 1, 0, true))) == kClassUnknown, "opcode 15 is class unknown");
    Expect(cls(CfOnly(ExecCf(1, 1, 0), OpCf(0))) == kClassUnknown, "no exec_end: class unknown");
    Expect(cls(CfOnly(LoopStartCf(1, 0), PredExecCf(1, 1, true, true, 0))) == (kClassLoop | kClassPredicate | kClassUnknown),
           "classes accumulate (loop + predicate, and unknown for the missing exec_end)");
    Expect(ShaderClassName(kClassStraight) == "straight" && ShaderClassName(kClassLoop | kClassBranch) == "branch,loop" &&
               ShaderClassName(kClassCall | kClassUnknown) == "call,unknown",
           "class names");

    ShaderClass allowed = ~0u;
    std::string error;
    Expect(ParseShaderClassAllowList("none", allowed, error) && allowed == kClassStraight, "--shader-classes none allows straight-line only");
    Expect(ParseShaderClassAllowList("loop", allowed, error) && allowed == kClassLoop, "--shader-classes loop");
    Expect(ParseShaderClassAllowList("branch,predicate,loop", allowed, error) && allowed == (kClassBranch | kClassPredicate | kClassLoop),
           "--shader-classes branch,predicate,loop");
    Expect(!ParseShaderClassAllowList("", allowed, error) && allowed == kClassStraight && Has(error, "empty"), "an empty allow-list is refused");
    Expect(!ParseShaderClassAllowList(nullptr, allowed, error), "a missing allow-list is refused");
    Expect(!ParseShaderClassAllowList("call", allowed, error) && Has(error, "\"call\""), "call is never allowed");
    Expect(!ParseShaderClassAllowList("loops", allowed, error) && allowed == kClassStraight && Has(error, "\"loops\""), "a typo is refused, nothing allowed");
    Expect(!ParseShaderClassAllowList("loop,", allowed, error), "a trailing comma is refused");
    Expect(RefusedClasses(kClassLoop, kClassStraight, kClassStraight) == kClassLoop, "a loop VS with no allow-list is refused as loop");
    Expect(RefusedClasses(kClassLoop, kClassBranch, kClassLoop) == kClassBranch, "the PS's branch is what remains refused");
    Expect(RefusedClasses(kClassStraight, kClassStraight, kClassStraight) == 0, "straight-line pairs always run");
    CONTROL(RefusedClasses(kClassCall, kClassStraight, kClassLoop) == 0, "call is refused even with loops allowed");
}

void TestGlslang()
{
    GlslangCompiler glslang;
    std::vector<uint32_t> spirv;
    std::string warnings, error;
    const std::string tiny = "#version 450\nvoid main()\n{\n    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n}\n";
    Expect(glslang.Compile(false, tiny, spirv, warnings, error) && spirv.size() > 5 && spirv[0] == 0x07230203u && spirv[1] == 0x00010000u,
           "a tiny vertex shader compiles to SPIR-V 1.0 (magic 07230203, version 00010000: the census's -V target)");
    const std::string frag = "#version 450\nlayout(location = 0) out vec4 c;\nvoid main()\n{\n    c = vec4(1.0);\n}\n";
    Expect(glslang.Compile(true, frag, spirv, warnings, error) && spirv[0] == 0x07230203u && spirv[1] == 0x00010000u, "a tiny fragment shader compiles");
    CONTROL(spirv[1] == 0x00010500u, "the version word is not SPIR-V 1.5 (the vulkan1.2 target of tests/shader)");
    Expect(!glslang.Compile(false, "#version 450\nvoid main()\n{\n    gl_Position = nonsense;\n}\n", spirv, warnings, error) && spirv.empty() &&
               Has(error, "glslang parse") && Has(error, "nonsense") && Has(error, "undeclared identifier"),
           "malformed GLSL fails with the compiler's text naming the symbol");
    Expect(!glslang.Compile(true, "#version 450\nlayout(location = 0) out vec4 c;\nvoid main()\n{\n    c = vec4(1.0)\n}\n", spirv, warnings, error) &&
               Has(error, "syntax error"),
           "a missing semicolon is a syntax error");
    // Vulkan rules: a GLSL uniform without a block is refused for Vulkan (EShMsgVulkanRules).
    Expect(!glslang.Compile(false, "#version 450\nuniform float f;\nvoid main()\n{\n    gl_Position = vec4(f);\n}\n", spirv, warnings, error),
           "a non-block uniform is refused under the Vulkan rules");
}

void TestCache()
{
    GlslangCompiler glslang;
    ShaderCache cache(glslang, work.c_str());
    std::string reason;
    const std::vector<uint32_t> ps = ColourPs(), vs = ClearVs(), loop = LoopPs();
    const CachedShader* p = cache.Load(1, ps.data(), uint32_t(ps.size()), reason);
    Expect(p != nullptr && p->pixel && p->hash == runtime::gpu::ShaderHash(1, ps.data(), uint32_t(ps.size())) && p->words == ps.size() &&
               p->cls == kClassStraight && !p->t.glsl.empty(),
           "a pixel shader loads: translated, hashed as the CP trace does, class straight");
    Expect(cache.Current(true) == p && cache.Current(false) == nullptr, "it is the current pixel shader; no vertex shader yet");
    Expect(cache.Load(1, ps.data(), uint32_t(ps.size()), reason) == p && cache.Stats().hits == 1 && cache.Stats().translations == 1,
           "a re-load is a hit (no second translation)");
    const CachedShader* v = cache.Load(0, vs.data(), uint32_t(vs.size()), reason);
    Expect(v != nullptr && !v->pixel && cache.Current(false) == v && v->t.vertexFetchSlots == std::vector<uint32_t>{0}, "a vertex shader loads with its reflection");
    const CachedShader* l = cache.Load(1, loop.data(), uint32_t(loop.size()), reason);
    Expect(l != nullptr && l->cls == kClassLoop && l->t.loopConstants == std::vector<uint32_t>{31} && l->t.textureSlots == std::vector<uint32_t>{0} &&
               cache.Current(true) == l,
           "a loop shader loads with class loop and its loop constant; it replaces the current pixel shader");
    Expect(cache.Find(p->hash) == p && cache.Find(0x1234) == nullptr && cache.Size() == 3, "Find by hash");
    Expect(cache.Load(2, ps.data(), uint32_t(ps.size()), reason) == nullptr && Has(reason, "type 2"), "type 2 is refused");
    Expect(cache.Load(1, ps.data(), 4, reason) == nullptr && Has(reason, "4 words"), "a size that is not whole slots is refused");
    // A translator refusal: the loop program with its loop_end's loop constant changed (no matching loop_start).
    std::vector<uint32_t> bad = loop;
    std::vector<uint32_t> badCf;
    PackCf(badCf, ExecCf(4, 3, 0x4), LoopEndCf(2, 30));
    bad[3] = badCf[0], bad[4] = badCf[1], bad[5] = badCf[2];
    const uint64_t badHash = runtime::gpu::ShaderHash(1, bad.data(), uint32_t(bad.size()));
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(badHash));
    Expect(cache.Load(1, bad.data(), uint32_t(bad.size()), reason) == nullptr && Has(reason, std::string("ps_") + hex) && Has(reason, "refused") &&
               Has(reason, "class loop") && cache.Stats().refusals == 1 && cache.Current(true) == l,
           "a translator refusal names the hash, the class and the reason; the current shader is unchanged");
    const uint64_t translations = cache.Stats().translations;
    Expect(cache.Load(1, bad.data(), uint32_t(bad.size()), reason) == nullptr && Has(reason, hex) && cache.Stats().translations == translations &&
               cache.Stats().refusals == 2,
           "a refused shader re-loaded is refused again without translating");

    // Variants.
    ShaderVariant* base = cache.Variant(*p, {}, reason);
    Expect(base != nullptr && base->id == 0 && base->spirv.size() > 5 && base->spirv[0] == 0x07230203u && base->spirv[1] == 0x00010000u &&
               base->module == 0 && base->defines.empty(),
           "the no-define variant compiles to SPIR-V 1.0 (module not yet created)");
    Expect(cache.Variant(*p, {}, reason) == base && cache.Stats().compiles == 1, "the same variant again is the same object, no recompile");
    ShaderVariantDefines rect;
    rect.Add("XE_RECT_LIST");
    ShaderVariant* rectVs = cache.Variant(*v, rect, reason);
    Expect(rectVs != nullptr && rectVs->id == rect.Id() && rectVs->defines == "#define XE_RECT_LIST\n" && rectVs->spirv[0] == 0x07230203u,
           "the rect-list variant of the vertex shader compiles");
    Expect(cache.Variant(*v, {}, reason) != rectVs && v->variants.size() == 2, "the plain variant is another object");
    ShaderVariantDefines alpha;
    alpha.Add("XE_ALPHA_TEST 4");
    alpha.Add("XE_ALPHA_REF 0.5");
    Expect(cache.Variant(*p, alpha, reason) != nullptr && p->variants.size() == 2, "an alpha-test variant of the pixel shader compiles");
    ShaderVariantDefines broken;
    broken.Add("XE_ALPHA_TEST 4");  // XE_ALPHA_REF missing: the GLSL references it
    Expect(cache.Variant(*p, broken, reason) == nullptr && Has(reason, std::string("ps_")) && Has(reason, "failed to compile") &&
               Has(reason, "XE_ALPHA_REF") && cache.Stats().compileFailures == 1,
           "a variant whose defines are incomplete fails with glslang's text");
    const uint64_t compiles = cache.Stats().compiles;
    Expect(cache.Variant(*p, broken, reason) == nullptr && cache.Stats().compiles == compiles && Has(reason, "failed to compile"),
           "the failed variant is refused again without recompiling");

    // The dump: the base GLSL of each translated shader and each define variant's source.
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(p->hash));
    Expect(ReadFile(work + "/ps_" + hex + ".frag") == p->t.glsl, "FM4_SHADER_DUMP: ps_<hash>.frag holds the translated GLSL");
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(v->hash));
    char vid[17];
    std::snprintf(vid, sizeof vid, "%016llx", static_cast<unsigned long long>(rect.Id()));
    Expect(ReadFile(work + "/vs_" + hex + ".vert") == v->t.glsl && ReadFile(work + "/vs_" + hex + "_" + vid + ".vert") == VariantGlsl(v->t.glsl, rect),
           "FM4_SHADER_DUMP: vs_<hash>.vert and vs_<hash>_<variant>.vert");
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(badHash));
    Expect(::access((work + "/ps_" + hex + ".frag").c_str(), F_OK) != 0, "a refused shader writes no GLSL (it has none)");
}

void TestCacheErrorPath()
{
    FailingCompiler failing;
    ShaderCache cache(failing, nullptr, 1);
    std::string reason;
    const std::vector<uint32_t> ps = ColourPs();
    const CachedShader* p = cache.Load(1, ps.data(), uint32_t(ps.size()), reason);
    Expect(p != nullptr, "loading needs no compiler");
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(p->hash));
    Expect(cache.Variant(*p, {}, reason) == nullptr && Has(reason, "boom") && Has(reason, hex) && failing.calls == 1 && p->variants.empty(),
           "a compiler failure is returned with its text and the hash; no variant is kept");
    Expect(cache.Variant(*p, {}, reason) == nullptr && failing.calls == 1 && cache.Stats().compileFailures == 1, "remembered: the compiler is not called again");
    CONTROL(reason.empty(), "the reason is never empty");

    // The per-shader variant cap, on a compiler that succeeds.
    GlslangCompiler glslang;
    ShaderCache capped(glslang, nullptr, 1);
    const CachedShader* q = capped.Load(1, ps.data(), uint32_t(ps.size()), reason);
    Expect(q != nullptr && capped.Variant(*q, {}, reason) != nullptr, "the first variant fits the cap of 1");
    ShaderVariantDefines alpha;
    alpha.Add("XE_ALPHA_TEST 7");
    alpha.Add("XE_ALPHA_REF 0.25");
    Expect(capped.Variant(*q, alpha, reason) == nullptr && Has(reason, "more than 1 variants") && capped.Stats().compiles == 1,
           "the second distinct variant is refused by the cap without compiling");
    ShaderVariantDefines empty;
    Expect(capped.Variant(*q, empty, reason) != nullptr, "the existing variant is still served");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: test_shader_cache <fresh work dir>\n");
        return 2;
    }
    work = argv[1];
    struct stat st{};
    if (::stat(work.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    {
        std::fprintf(stderr, "test_shader_cache: %s is not a directory\n", work.c_str());
        return 2;
    }
    TestHash();
    TestClass();
    TestGlslang();
    TestCache();
    TestCacheErrorPath();
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures == 0 ? 0 : 1;
}
