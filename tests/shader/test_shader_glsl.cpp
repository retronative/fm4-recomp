// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp tests/gpu/test_shader_glsl.cpp (GPL-3.0-or-later, same owner),
// reviewed and re-run here. FM4 Recomp changes: namespace and include paths as the unit's; the
// scratch directory comes from the runner (tests/shader/run_shader_tests.sh), no default;
// TestMiniFetchScope (mini vertex fetches across a predicated exec) added; TestUniformBranches
// (fetches in bool-constant control flow, terminal jump regions, kill ops, RECIP_CLAMP, MAXAs) added
// and the refusals those capabilities replaced turned into acceptances.
//
// T0 tests of the Xenos microcode -> GLSL translator (runtime/shader/shader_glsl.cpp) and the
// texture fetch decoder it uses (runtime/shader/ucode.h). Every program is hand-assembled here
// with small encoders written from the bit layouts in ucode.h (Xenia ucode.h, see NOTICE); no game
// data is used. The six programs mirror the structure of the ones FM4's first frames use. The
// expected GLSL is written by hand from the ISA definitions quoted in shader_glsl.cpp, not taken
// from the translator; every emitted shader (and the rect-list variant of every vertex shader)
// is compiled with glslangValidator and checked with spirv-val. Negative controls (CONTROL) feed
// a mutated program or a wrong oracle to the same check and require it to fail.
// Run with tests/shader/run_shader_tests.sh (needs glslangValidator and spirv-val; the one
// argument is the scratch directory for the emitted shaders).
#include "shader/shader_glsl.h"
#include "shader/ucode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int passed = 0, failed = 0, controls = 0;
std::string tempDir;

void Check(bool ok, const char* what, int line)
{
    if (ok)
        passed++;
    else
    {
        failed++;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)
#define CONTROL(x)                             \
    do                                         \
    {                                          \
        controls++;                            \
        Check(!(x), "control: " #x, __LINE__); \
    } while (0)

using namespace runtime::shader;
using namespace runtime::shader::ucode;

// ---- Encoders (bit positions from ucode.h) ---------------------------------------------------

struct Cf
{
    uint32_t w0, w1;
};
// Exec: word0 address 0-11, count 12-14, sequence 16-27; word1 predicate clean 9, opcode 12-15
// (1 exec, 2 exec_end).
Cf ExecCf(uint32_t address, uint32_t count, uint32_t sequence, bool end = false, bool clean = false)
{
    return {address | count << 12 | sequence << 16, uint32_t(clean) << 9 | uint32_t(end ? 2 : 1) << 12};
}
// Predicated exec (opcode 5): word0 as exec; word1 predicate clean 9, condition 10.
Cf PredExecCf(uint32_t address, uint32_t count, bool condition, bool clean = true, uint32_t sequence = 0)
{
    return {address | count << 12 | sequence << 16, uint32_t(clean) << 9 | uint32_t(condition) << 10 | 5u << 12};
}
// Conditional exec on a bool constant (opcode 13): word0 as exec; word1 bool constant 2-9,
// condition 10.
Cf CondExecCf(uint32_t address, uint32_t count, uint32_t boolIndex, bool condition, uint32_t sequence = 0)
{
    return {address | count << 12 | sequence << 16, boolIndex << 2 | uint32_t(condition) << 10 | 13u << 12};
}
// Alloc: word1 type 9-10, opcode 12 at 12-15.
Cf AllocCf(uint32_t type) { return {0, type << 9 | 12u << 12}; }
// loop_start (7): word0 skip address 0-12, is_repeat 13, loop constant 16-20; word1 address mode 11.
Cf LoopStartCf(uint32_t address, uint32_t id, bool repeat = false, bool addressMode = false)
{
    return {address | uint32_t(repeat) << 13 | id << 16, uint32_t(addressMode) << 11 | 7u << 12};
}
// loop_end (8): word0 body address 0-12, loop constant 16-20, predicated break 21; word1
// condition 10, address mode 11.
Cf LoopEndCf(uint32_t address, uint32_t id, bool predicatedBreak = false, bool condition = true, bool addressMode = false)
{
    return {address | id << 16 | uint32_t(predicatedBreak) << 21, uint32_t(condition) << 10 | uint32_t(addressMode) << 11 | 8u << 12};
}
// cond_jmp (11): word0 target 0-12, unconditional 13, predicated 14; word1 direction 1, bool
// constant 2-9, condition 10, address mode 11.
struct Jmp
{
    uint32_t target = 4, boolIndex = 128;
    bool condition = false, unconditional = false, predicated = false, direction = false, addressMode = false;
};
Cf JmpCf(const Jmp& j)
{
    return {j.target | uint32_t(j.unconditional) << 13 | uint32_t(j.predicated) << 14,
            uint32_t(j.direction) << 1 | j.boolIndex << 2 | uint32_t(j.condition) << 10 | uint32_t(j.addressMode) << 11 | 11u << 12};
}
Cf OpCf(uint32_t opcode, uint32_t word0 = 0) { return {word0, opcode << 12}; }
// Two control-flow instructions per 3-word slot: a in bits 0-47, b in bits 48-95.
void PackCf(std::vector<uint32_t>& p, Cf a, Cf b)
{
    p.push_back(a.w0);
    p.push_back((a.w1 & 0xFFFF) | b.w0 << 16);
    p.push_back(b.w0 >> 16 | b.w1 << 16);
}

// ALU source swizzle from absolute components: field i = (component - i) & 3.
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
    uint32_t reg = 0;  // temp: index | 0x80 for abs
    uint32_t swz = 0;
    bool neg = false;
};
Src T(uint32_t r, const char* s = "xyzw") { return {true, r, Swz(s), false}; }
Src C(uint32_t r, const char* s = "xyzw") { return {false, r, Swz(s), false}; }
struct Alu
{
    uint32_t vop = 2, sop = 50, vdest = 0, sdest = 0, vmask = 0, smask = 0;
    bool exp = false, sdestRel = false, vclamp = false, sclamp = false, absConst = false, pred = false, c0rel = false;
    bool predCond = false;
    Src s1, s2, s3;
};
// word0: vdest 0-5, abs_constants 7, sdest 8-13, sdest_rel 14, export 15, vmask 16-19,
// smask 20-23, clamps 24/25, scalar opcode 26-31. word1: swizzles 0-7 (src3), 8-15, 16-23 (src1),
// negates 24 (src3), 25, 26 (src1), predicate condition 27, predicated 28, const_0_rel 31. word2: regs 0-7 (src3), 8-15,
// 16-23 (src1), vector opcode 24-28, selects 29 (src3), 30, 31 (src1).
void Put(std::vector<uint32_t>& p, const Alu& a)
{
    p.push_back(a.vdest | uint32_t(a.absConst) << 7 | a.sdest << 8 | uint32_t(a.sdestRel) << 14 |
                uint32_t(a.exp) << 15 | a.vmask << 16 | a.smask << 20 | uint32_t(a.vclamp) << 24 |
                uint32_t(a.sclamp) << 25 | a.sop << 26);
    p.push_back(a.s3.swz | a.s2.swz << 8 | a.s1.swz << 16 | uint32_t(a.s3.neg) << 24 | uint32_t(a.s2.neg) << 25 |
                uint32_t(a.s1.neg) << 26 | uint32_t(a.predCond) << 27 | uint32_t(a.pred) << 28 | uint32_t(a.c0rel) << 31);
    p.push_back(a.s3.reg | a.s2.reg << 8 | a.s1.reg << 16 | a.vop << 24 | uint32_t(a.s3.temp) << 29 |
                uint32_t(a.s2.temp) << 30 | uint32_t(a.s1.temp) << 31);
}

// Fetch destination swizzle, 3 bits per component: xyzw 0-3, '0' 4, '1' 5, '_' 7 (keep).
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
    bool mini = false, pred = false, rounded = false;
    bool sgn = true, integer = false, rf = false;  // format_comp_all, num_format_all, signed_rf_mode_all
};
// word0: opcode 0 at 0-4, src 5-10, dest 12-17, must-be-one 19, const index 20-24, select 25-26,
// src swizzle 30-31. word1: dest swizzle 0-11, signed 12, integer (not normalized) 13, signed
// repeating-fraction mode 14, rounded 15, format 16-21, exp adjust 24-29, mini 30, predicated
// 31. word2: stride 0-7, offset 8-30.
void Put(std::vector<uint32_t>& p, const VFetch& f)
{
    p.push_back(f.src << 5 | f.dest << 12 | 1u << 19 | (f.slot / 3) << 20 | (f.slot % 3) << 25 | f.srcComp << 30);
    p.push_back(f.dswz | uint32_t(f.sgn) << 12 | uint32_t(f.integer) << 13 | uint32_t(f.rf) << 14 | uint32_t(f.rounded) << 15 |
                f.format << 16 | (uint32_t(f.expAdjust) & 63) << 24 |
                uint32_t(f.mini) << 30 | uint32_t(f.pred) << 31);
    p.push_back(f.stride | (uint32_t(f.offset) & 0x7FFFFF) << 8);
}
struct TFetch
{
    uint32_t src = 0, dest = 0, slot = 0, srcSwz = 0x4 /* x, y */, dswz = 0x688 /* xyzw */;
    uint32_t mag = 3, min = 3, mip = 3, aniso = 7, arb = 0, vmag = 3, vmin = 3, dim = 1;
    bool valid = true, unnorm = false, compLod = true, regLod = false, regGrad = false, pred = false;
    int32_t bias = 0, ox = 0, oy = 0, oz = 0;
};
// word0: opcode 1, src 5-10, dest 12-17, fetch_valid_only 19, const 20-24, unnormalized 25, src
// swizzle 26-31. word1: dest swizzle 0-11, mag 12-13, min 14-15, mip 16-17, aniso 18-20,
// arbitrary 21-23, vol mag 24-25, vol min 26-27, computed LOD 28, register LOD 29, predicated 31.
// word2: register gradients 0, sample location 1, LOD bias 2-8, dimension 14-15, offsets 16/21/26.
void Put(std::vector<uint32_t>& p, const TFetch& t)
{
    p.push_back(1u | t.src << 5 | t.dest << 12 | uint32_t(t.valid) << 19 | t.slot << 20 | uint32_t(t.unnorm) << 25 |
                t.srcSwz << 26);
    p.push_back(t.dswz | t.mag << 12 | t.min << 14 | t.mip << 16 | t.aniso << 18 | t.arb << 21 | t.vmag << 24 |
                t.vmin << 26 | uint32_t(t.compLod) << 28 | uint32_t(t.regLod) << 29 | uint32_t(t.pred) << 31);
    p.push_back(uint32_t(t.regGrad) | (uint32_t(t.bias) & 0x7F) << 2 | t.dim << 14 | (uint32_t(t.ox) & 31) << 16 |
                (uint32_t(t.oy) & 31) << 21 | (uint32_t(t.oz) & 31) << 26);
}

// ---- Helpers ---------------------------------------------------------------------------------

bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

int compiled = 0;
// Compiles `glsl` (and, for a vertex shader, its rect-list variant with `#define XE_RECT_LIST`
// inserted as line 2, as the host does) and validates the SPIR-V.
bool Compiles(const std::string& glsl, bool pixel)
{
    for (int variant = 0; variant < (pixel ? 1 : 2); variant++)
    {
        std::string text = glsl;
        if (variant == 1)
            text.insert(text.find('\n') + 1, "#define XE_RECT_LIST\n");
        const std::string base = tempDir + "/shader" + std::to_string(compiled++);
        const std::string src = base + (pixel ? ".frag" : ".vert"), spv = base + ".spv";
        std::ofstream(src) << text;
        const std::string compile = "glslangValidator -V --target-env vulkan1.2 " + src + " -o " + spv + " > " + base + ".log 2>&1";
        const std::string validate = "spirv-val --target-env vulkan1.2 " + spv + " >> " + base + ".log 2>&1";
        if (std::system(compile.c_str()) != 0 || std::system(validate.c_str()) != 0)
        {
            std::fprintf(stderr, "compile/validate failed: see %s.log\n", base.c_str());
            return false;
        }
    }
    return true;
}

bool Translate(const std::vector<uint32_t>& p, bool pixel, ShaderTranslation& t, std::string* errorOut = nullptr)
{
    std::string error;
    const bool ok = TranslateShader(p.data(), p.size(), pixel, t, error);
    if (errorOut)
        *errorOut = error;
    return ok;
}

// Expects a refusal whose message contains `what`.
bool Refuses(const std::vector<uint32_t>& p, bool pixel, const char* what)
{
    ShaderTranslation t;
    std::string error;
    if (TranslateShader(p.data(), p.size(), pixel, t, error))
        return false;
    if (!Has(error, what))
        std::fprintf(stderr, "  refusal message \"%s\" lacks \"%s\"\n", error.c_str(), what);
    return Has(error, what);
}

// ---- Programs --------------------------------------------------------------------------------

// 1. VS "clear": exec{vfetch_full r1.xyz1 <- r0.x slot 0 fmt 57 stride 7 offset 0; vfetch_mini
// r0.xyzw fmt 38 offset 3}; alloc interpolators; MAX export0 = r0; alloc position; MAX export62
// = r1; exec_end with a no-op ALU (both masks 0). Slots 0-2 control flow, 3-4 fetch, 5-7 ALU.
// Sequence 0b0101: instructions 0 and 1 are fetches.
std::vector<uint32_t> ClearVs(VFetch full = {}, VFetch mini = {})
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(3, 2, 0x5), AllocCf(2));
    PackCf(p, ExecCf(5, 1, 0), AllocCf(1));
    PackCf(p, ExecCf(6, 1, 0), ExecCf(7, 1, 0, true));
    Put(p, full);
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
VFetch ClearFull()
{
    VFetch f;
    f.dest = 1, f.slot = 0, f.format = 57, f.stride = 7, f.offset = 0, f.dswz = DSwz("xyz1");
    return f;
}
VFetch ClearMini()
{
    VFetch f;
    f.dest = 0, f.format = 38, f.offset = 3, f.mini = true, f.dswz = DSwz("xyzw");
    return f;
}

// 2. PS "colour": alloc colours; exec_end MAX export0 = r0.
std::vector<uint32_t> ColourPs(Alu e = {})
{
    std::vector<uint32_t> p;
    PackCf(p, AllocCf(2), ExecCf(1, 1, 0, true));
    if (!e.exp)
        e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(0), e.s2 = T(0), e.s3 = T(0);
    Put(p, e);
    return p;
}

// 3. VS "quads": vfetch_full r1.__xy slot 95 fmt 37 stride 4; vfetch_mini r1.xy__ offset 2;
// DOT2ADDv r0.z/x/w/y = dot(r1.zw, cN.xy) + cN.z for N = 3, 2, 1, 0; ADDv export62.y =
// r0.w + c1.w with ADD_CONST_0 export62.x = c0.w + r0.y (src3 c0, swizzle yyzw: field 0 = 1 so
// b = y and the temp index (44 & 1) | sel << 1 | (swizzle & 0x3C) = 0; field 3 = 0 so a = w);
// ADDv export62.w = r0.z + c3.w with ADD_CONST_0 export62.z = c2.w + r0.x (swizzle xyzw);
// alloc interpolators; MULv r1.zw = r1.xxxy * c255.xxxy; MAX export0 = r1.
// Control flow: exec(3, 2) alloc(1) exec(5, 6) alloc(2) exec_end(11, 2) nop.
std::vector<uint32_t> QuadsVs(bool swapDp2AddSources = false, uint32_t addscSwizzle = Swz("yyzw"))
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(3, 2, 0x5), AllocCf(1));
    PackCf(p, ExecCf(5, 6, 0), AllocCf(2));
    PackCf(p, ExecCf(11, 2, 0, true), OpCf(0));
    VFetch full;
    full.dest = 1, full.slot = 95, full.format = 37, full.stride = 4, full.dswz = DSwz("__xy");
    Put(p, full);
    VFetch mini;
    mini.dest = 1, mini.format = 37, mini.offset = 2, mini.mini = true, mini.dswz = DSwz("xy__");
    Put(p, mini);
    const uint32_t rows[4][2] = {{3, 0x4}, {2, 0x1}, {1, 0x8}, {0, 0x2}};  // constant, r0 mask
    for (const auto& row : rows)
    {
        Alu d;
        d.vop = 17, d.vdest = 0, d.vmask = row[1], d.s1 = T(1, "zwww"), d.s2 = C(row[0], "xyyy"), d.s3 = C(row[0], "zzzz");
        if (swapDp2AddSources)
            std::swap(d.s1, d.s3);
        Put(p, d);
    }
    Alu a;
    a.vop = 0, a.sop = 44, a.exp = true, a.vdest = 62, a.vmask = 0x2, a.smask = 0x1;
    a.s1 = T(0, "wwww"), a.s2 = C(1, "wwww"), a.s3 = C(0), a.s3.swz = addscSwizzle;
    Put(p, a);
    Alu b = a;
    b.vmask = 0x8, b.smask = 0x4, b.s1 = T(0, "zzzz"), b.s2 = C(3, "wwww"), b.s3 = C(2, "xyzw");
    Put(p, b);
    Alu m;
    m.vop = 1, m.vdest = 1, m.vmask = 0xC, m.s1 = T(1, "xxxy"), m.s2 = C(255, "xxxy"), m.s3 = T(0);
    Put(p, m);
    Alu e;
    e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(1), e.s2 = T(1), e.s3 = T(0);
    Put(p, e);
    return p;
}

// 4. PS "textured": exec{tfetch r0 <- r0.xy}; alloc colours; exec_end three ALU: vector MAXv
// with mask 0, 0, w (r0.wwww) and SQRT_IEEE of |r0|.x, |r0|.y, |r0|.z into export0 x, y, z
// (src3 temp r0 with the abs bit 0x80; a = swizzle field 3).
std::vector<uint32_t> TexturedPs(TFetch t = {}, bool abs = true)
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(2, 1, 0x1), AllocCf(2));
    PackCf(p, ExecCf(3, 3, 0, true), OpCf(0));
    Put(p, t);
    const char* comps[3] = {"xxxx", "yyyy", "zzzz"};
    for (uint32_t i = 0; i < 3; i++)
    {
        Alu a;
        a.sop = 40, a.exp = true, a.vdest = 0, a.smask = 1u << i;
        a.s1 = T(0), a.s2 = T(0), a.s3 = T(abs ? 0x80 : 0, comps[i]);
        if (i == 2)
            a.vmask = 0x8, a.s1 = T(0, "wwww"), a.s2 = T(0, "wwww");
        Put(p, a);
    }
    return p;
}

// TexturedPs with setTextureLod (fetch opcode 24) from r0.z before the fetch, which uses the
// register LOD (computed LOD on or off).
std::vector<uint32_t> TexturedLodPs(bool computed)
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(2, 2, 0x5), AllocCf(2));
    PackCf(p, ExecCf(4, 3, 0, true), OpCf(0));
    TFetch set;
    set.srcSwz = 0x2;  // component 0 from z
    Put(p, set);
    p[p.size() - 3] = (p[p.size() - 3] & ~31u) | 24u;
    TFetch t;
    t.regLod = true, t.compLod = computed;
    Put(p, t);
    const char* comps[3] = {"xxxx", "yyyy", "zzzz"};
    for (uint32_t i = 0; i < 3; i++)
    {
        Alu a;
        a.sop = 40, a.exp = true, a.vdest = 0, a.smask = 1u << i;
        a.s1 = T(0), a.s2 = T(0), a.s3 = T(0x80, comps[i]);
        if (i == 2)
            a.vmask = 0x8, a.s1 = T(0, "wwww"), a.s2 = T(0, "wwww");
        Put(p, a);
    }
    return p;
}

// 5. VS "resolve": exec{vfetch_full r0.xy11 slot 0 fmt 37 stride 2}; alloc both; exec_end MAX
// export62 = r0.
std::vector<uint32_t> ResolveVs(VFetch f = {}, bool useDefault = true)
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(2, 1, 0x1), AllocCf(2));
    PackCf(p, AllocCf(1), ExecCf(3, 1, 0, true));
    if (useDefault)
        f.dest = 0, f.slot = 0, f.format = 37, f.stride = 2, f.dswz = DSwz("xy11");
    Put(p, f);
    Alu e;
    e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(0), e.s2 = T(0), e.s3 = T(0);
    Put(p, e);
    return p;
}

// 6. VS "points": constant exports only. export0: masks 0, scalar_dest_rel 1 -> constant 0 in
// xyzw; export62: vector and scalar mask w (constant 1), scalar_dest_rel 1 -> constant 0 in xyz.
std::vector<uint32_t> PointsVs()
{
    std::vector<uint32_t> p;
    PackCf(p, AllocCf(2), ExecCf(1, 2, 0, true));
    Alu z;
    z.exp = true, z.vdest = 0, z.sdestRel = true;
    Put(p, z);
    Alu w = z;
    w.vdest = 62, w.vmask = 0x8, w.smask = 0x8;
    Put(p, w);
    return p;
}

// 7. PS "video" (shaped like FM4's video-frame shader: three single-channel planes, a colour
// matrix, then one of two transfer curves chosen by a bool constant).
//   cf0 exec(4, 3, fetches): tfetch r1.x___ <- tf2, r1._x__ <- tf1, r1.__x_ <- tf0 (coords r0.xy)
//   cf1 alloc colours
//   cf2 exec(7, 2):  7: MAXs r1.w = max(c255.w, c255.w) (src3 c255.wwww, a = b = w)
//                    8: DOT4v r0.x = dot(c0.zxyw, r1.xzyw) + ADDs r4.x = r0.w + r0.x (src3 r0,
//                       swizzle xyzw: a = w, b = x)
//   cf3 cexec b128 == 1 (9, 2):  9: MULADDv r1.xyz = r0.xxxx * c253.wwww + c254.yyyy
//                               10: LOG_IEEE r2.x = log2(|r1|.x)
//   cf4 cexec b128 == 1 (11, 2): 11: SETGTEv r0.xyz = c253.zzzz >= r1.xyzw + EXP_IEEE r2.y =
//                                    2^r2.x (r2.x from cf3, same condition)
//                                12: CNDEv r3.xyz = r0 == 0 ? r2.yyyy : r1
//   cf5 cexec b128 == 0 (13, 1): 13: MULADDv r3.xyz = r0.xxxx * c255.yyyy + c255.xxxx
//   cf6 exec_end(14, 1):  14: MAXv export0.xyz = r3 + MAXs export0.w = c3.w >= c3.x ? c3.w : c3.x
//   cf7 nop. Control-flow slots 0-3, instruction slots 4-14.
struct VideoOptions
{
    uint32_t boolIndex = 128;
    bool thenCondition = true;   // cf3's and cf4's condition
    bool elseCondition = false;  // cf5's condition
    bool swapMadAddend = false;  // slot 9: src2 and src3 exchanged
    bool swapSge = false;        // slot 11: src1 and src2 exchanged
    bool swapCndeq = false;      // slot 12: src2 and src3 exchanged
    uint32_t addsSwizzle = Swz("xyzw");
};
std::vector<uint32_t> VideoPs(const VideoOptions& o = {})
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(4, 3, 0x15), AllocCf(2));
    PackCf(p, ExecCf(7, 2, 0), CondExecCf(9, 2, o.boolIndex, o.thenCondition));
    PackCf(p, CondExecCf(11, 2, o.boolIndex, o.thenCondition), CondExecCf(13, 1, o.boolIndex, o.elseCondition));
    PackCf(p, ExecCf(14, 1, 0, true), OpCf(0));
    const char* dswz[3] = {"x___", "_x__", "__x_"};
    for (uint32_t i = 0; i < 3; i++)
    {
        TFetch t;
        t.dest = 1, t.slot = 2 - i, t.dswz = DSwz(dswz[i]);
        Put(p, t);
    }
    Alu move;
    move.sop = 5, move.sdest = 1, move.smask = 0x8, move.s1 = T(0), move.s2 = T(0), move.s3 = C(255, "wwww");
    Put(p, move);
    Alu dp4;
    dp4.vop = 15, dp4.vdest = 0, dp4.vmask = 0x1, dp4.s1 = C(0, "zxyw"), dp4.s2 = T(1, "xzyw");
    dp4.sop = 0, dp4.sdest = 4, dp4.smask = 0x1, dp4.s3 = T(0), dp4.s3.swz = o.addsSwizzle;
    Put(p, dp4);
    Alu mad;
    mad.vop = 11, mad.vdest = 1, mad.vmask = 0x7, mad.s1 = T(0, "xxxx"), mad.s2 = C(253, "wwww"), mad.s3 = C(254, "yyyy");
    if (o.swapMadAddend)
        std::swap(mad.s2, mad.s3);
    Put(p, mad);
    Alu log;
    log.sop = 16, log.sdest = 2, log.smask = 0x1, log.s1 = T(0), log.s2 = T(0), log.s3 = T(0x81, "xxxx");
    Put(p, log);
    Alu sge;
    sge.vop = 6, sge.vdest = 0, sge.vmask = 0x7, sge.s1 = C(253, "zzzz"), sge.s2 = T(1);
    if (o.swapSge)
        std::swap(sge.s1, sge.s2);
    sge.sop = 14, sge.sdest = 2, sge.smask = 0x2, sge.s3 = T(2, "xxxx");
    Put(p, sge);
    Alu cnd;
    cnd.vop = 12, cnd.vdest = 3, cnd.vmask = 0x7, cnd.s1 = T(0), cnd.s2 = T(2, "yyyy"), cnd.s3 = T(1);
    if (o.swapCndeq)
        std::swap(cnd.s2, cnd.s3);
    Put(p, cnd);
    Alu other;
    other.vop = 11, other.vdest = 3, other.vmask = 0x7, other.s1 = T(0, "xxxx"), other.s2 = C(255, "yyyy"),
    other.s3 = C(255, "xxxx");
    Put(p, other);
    Alu e;
    e.exp = true, e.vdest = 0, e.vmask = 0x7, e.s1 = T(3), e.s2 = T(3), e.sop = 5, e.smask = 0x8, e.s3 = C(3);
    Put(p, e);
    return p;
}

// 8. VS "branches": alloc position; exec r1 = MAX(r0, r0) (r0 = index, so r0.x only: mask x);
// cexec b5 == 1 { MAXv r2.x = r1.x }; cexec b5 == `elseCondition` { MAXv r2.x = r0.x } (absent
// when `withElse` is false); exec_end MAXv export62.xyzw = r2.xxxx.
std::vector<uint32_t> BranchesVs(bool withElse, bool elseCondition = false)
{
    std::vector<uint32_t> p;
    PackCf(p, AllocCf(1), ExecCf(3, 1, 0));
    PackCf(p, CondExecCf(4, 1, 5, true), withElse ? CondExecCf(5, 1, 5, elseCondition) : OpCf(0));
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

// 9. PS "loop", the shape of an FM4 blur: exec{ADDv r1 = r0 + c0}; loop_start(skip 4, constant
// L); exec{MULADDv r0.xz = r0.zzzz * c1 + c2; tfetch r2 <- r0.xy slot 0 linear; ADDv r1 = r2 +
// r1}; loop_end(body 2, constant L, condition 1); alloc colours; exec_end MULv export0 =
// r<exportReg> * c3.zzzz. Slots 0-2 control flow, 3 the first exec, 4-6 the body, 7 the export.
struct LoopOpts
{
    uint32_t id = 31, endId = 31, skip = 4, back = 2, exportReg = 1, bodySeq = 0x4;
    bool repeat = false, startMode = false, endMode = false, predBreak = false, endCondition = true;
    Cf body = {}, afterBody = {};  // replace cf2 / cf4 when w1 is non-zero
    Alu last = {};                 // replaces the body's ADDv when its vmask is non-zero
};
std::vector<uint32_t> LoopPs(const LoopOpts& o = {})
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(3, 1, 0), LoopStartCf(o.skip, o.id, o.repeat, o.startMode));
    PackCf(p, o.body.w1 ? o.body : ExecCf(4, 3, o.bodySeq), LoopEndCf(o.back, o.endId, o.predBreak, o.endCondition, o.endMode));
    PackCf(p, o.afterBody.w1 ? o.afterBody : AllocCf(2), ExecCf(7, 1, 0, true));
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
    Put(p, o.last.vmask || o.last.exp ? o.last : s);
    Alu e;
    e.vop = 1, e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(o.exportReg), e.s2 = C(3, "zzzz"), e.s3 = T(0);
    Put(p, e);
    return p;
}

// 10. VS "loop": exec{MAXv r1.x = r0.xxxx; MAXv r2.x = r0.xxxx (writeBefore) or a no-op};
// loop_start(skip 4, constant 0); exec{MAXv r2.x = r<bodySrc>.xxxx}; loop_end(body 2, constant
// 0); alloc position; exec_end MAXv export62 = r2.xxxx.
std::vector<uint32_t> LoopVs(bool writeBefore, uint32_t bodySrc = 1)
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(3, 2, 0), LoopStartCf(4, 0));
    PackCf(p, ExecCf(5, 1, 0), LoopEndCf(2, 0));
    PackCf(p, AllocCf(1), ExecCf(6, 1, 0, true));
    Alu m;
    m.vdest = 1, m.vmask = 0x1, m.s1 = T(0, "xxxx"), m.s2 = T(0, "xxxx"), m.s3 = T(0);
    Put(p, m);
    Alu w = m;
    w.vdest = 2;
    Put(p, writeBefore ? w : Alu{});
    Alu b = w;
    b.s1 = T(bodySrc, "xxxx"), b.s2 = T(bodySrc, "xxxx");
    Put(p, b);
    Alu e;
    e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(2, "xxxx"), e.s2 = T(2, "xxxx"), e.s3 = T(0);
    Put(p, e);
    return p;
}

// 11. PS "previous scalar" (shaped like FM4's 6620 pixel shader: scalar ops with empty masks
// feeding MUL_PREVs, SUBs, RECIP_IEEE, DOT3v, MUL_CONST_0 and a bool-constant cond_jmp over an
// exec).
//   cf0 alloc colours
//   cf1 exec(3, 5):  3: DOT3v r1.x = dot3(r0.zyxw, c0) + MAXs (mask empty) of r0 (src3 swizzle
//                       xyzw: a = w, b = x): ps = r0.w >= r0.x ? r0.w : r0.x
//                    4: MUL_PREVs r2.x = r0.y * ps (src3 r0, swizzle xyzy: a = y)
//                    5: SUBs r2.y = c1.w - c1.x, scalar clamp (ps keeps the unclamped value)
//                    6: RETAIN_PREV r2.z = ps
//                    7: RECIP_IEEE r2.w = 1 / r2.y (src3 r2, swizzle xyzy)
//   cf2 cond_jmp to cf4 when bool 128 == 0 (skips cf3)
//   cf3 exec(8, 2):  8: MUL_CONST_0 r2.x = c3.w * r0.y (src3 c3, swizzle yyzw: field 0 = 1 so
//                       b = y and the temp index (42 & 1) | sel << 1 | (swizzle & 0x3C) = 0;
//                       field 3 = 0 so a = w)
//                    9: MAXv r4 = r0 + ADDs (mask empty) c2.w + c2.x: ps on the not-taken path
//   cf4 exec_end(10, 1): 10: MAXv export0.xyz = r<exportReg> + RETAIN_PREV export0.w = ps
//   cf5 nop. Control-flow slots 0-2, instruction slots 3-10.
struct PrevOptions
{
    Jmp jump;
    uint32_t exportReg = 2;
    uint32_t subsSwizzle = Swz("xyzw");
    uint32_t mulcOpcode = 42;
    bool mulcTemp = false;       // slot 8's src3 a temp (select 1): temp index 2
    bool firstRetain = false;    // slot 3's scalar op RETAIN_PREV (nothing sets ps before slot 4)
    uint32_t prevOpcode = 3;     // slot 4: MUL_PREVs (3), ADD_PREVs (1) or SUB_PREVs (26)
    uint32_t firstScalar = 5;    // slot 3's scalar op (MAXs; SIN 48, COS 49)
    Cf region = {};              // replaces cf3 when w1 is non-zero
    Cf afterJump = {};           // replaces cf4 when w1 is non-zero
};
std::vector<uint32_t> PrevPs(const PrevOptions& o = {})
{
    std::vector<uint32_t> p;
    PackCf(p, AllocCf(2), ExecCf(3, 5, 0));
    PackCf(p, JmpCf(o.jump), o.region.w1 ? o.region : ExecCf(8, 2, 0));
    PackCf(p, o.afterJump.w1 ? o.afterJump : ExecCf(10, 1, 0, true), OpCf(0));
    Alu dp3;
    dp3.vop = 16, dp3.vdest = 1, dp3.vmask = 0x1, dp3.s1 = T(0, "zyxw"), dp3.s2 = C(0);
    dp3.sop = o.firstRetain ? 50 : o.firstScalar, dp3.s3 = T(0);
    Put(p, dp3);
    Alu prev;
    prev.sop = o.prevOpcode, prev.sdest = 2, prev.smask = 0x1, prev.s1 = T(0), prev.s2 = T(0), prev.s3 = T(0, "xyzy");
    Put(p, prev);
    Alu sub;
    sub.sop = 25, sub.sdest = 2, sub.smask = 0x2, sub.sclamp = true, sub.s1 = T(0), sub.s2 = T(0), sub.s3 = C(1);
    sub.s3.swz = o.subsSwizzle;
    Put(p, sub);
    Alu retain;
    retain.sdest = 2, retain.smask = 0x4, retain.s1 = T(0), retain.s2 = T(0), retain.s3 = T(0);
    Put(p, retain);
    Alu rcp;
    rcp.sop = 19, rcp.sdest = 2, rcp.smask = 0x8, rcp.s1 = T(0), rcp.s2 = T(0), rcp.s3 = T(2, "xyzy");
    Put(p, rcp);
    Alu mulc;
    mulc.sop = o.mulcOpcode, mulc.sdest = 2, mulc.smask = 0x1, mulc.s1 = T(0), mulc.s2 = T(0);
    mulc.s3 = o.mulcTemp ? T(3, "yyzw") : C(3, "yyzw");
    Put(p, mulc);
    Alu copy;
    copy.vdest = 4, copy.vmask = 0xF, copy.s1 = T(0), copy.s2 = T(0), copy.sop = 0, copy.s3 = C(2);
    Put(p, copy);
    Alu e;
    e.exp = true, e.vdest = 0, e.vmask = 0x7, e.s1 = T(o.exportReg), e.s2 = T(o.exportReg), e.smask = 0x8, e.s3 = T(0);
    Put(p, e);
    return p;
}

// 12. VS "predicate" (the shape of an if / else-if / else on per-vertex data):
//   cf0 exec(5, 2):      5: MAXv r1.x = r0.x
//                        6: PRED_SETEs (no mask) a = r1.x: p0 = r1.x == 0
//   cf1 cond_jmp to cf6, predicated, taken when p0 == 0 (the region cf2-cf5 runs where p0 == 1)
//   cf2 exec(7, 2):      7: SETGTEv r1.y = r1.x >= c0.z
//                        8: PRED_SETNEs (no mask) a = r1.y: p0 = r1.y != 0
//   cf3 (p0) exec(9, 1): 9: (p0) MAXv r2.x = c1.x + MAXs r1.z = c1.w
//   cf4 (!p0) exec(10, 1): 10: (!p0) MAXv r2.x = c2.x + MAXs r1.z = c2.w
//   cf5 jmp to cf7 (unconditional: skips cf6, the else branch)
//   cf6 exec(11, 1):     11: MAXv r2.x = c3.x + MAXs r1.z = c3.w (predicate clean set)
//   cf7 alloc position; cf8 exec_end(12, 1): 12: ADDv export62 = r2.xxxx + r1.zzzz; cf9 nop.
// Control-flow slots 0-4, instruction slots 5-12. cf0 and cf2 set p0 and have the predicate-clean
// bit clear, the other execs set.
struct PredOptions
{
    Jmp jump = {6, 0, false, false, true};
    Jmp elseJump = {7, 0, false, true};
    uint32_t firstSet = 27, secondSet = 28, firstSetMask = 0;
    bool thenCondition = true, thenPred = true, thenPredCond = true, otherCondition = false, firstClean = false, readPs = false;
    Cf then = {}, otherwise = {};  // replace cf3 / cf4 when w1 is non-zero
    uint32_t thenSop = 5, thenScalarMask = 0x4;  // slot 9's scalar op and mask
    int elsePred = -1;                            // slot 11 predicated on this condition (0 / 1)
};
std::vector<uint32_t> PredVs(const PredOptions& o = {})
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(5, 2, 0, false, o.firstClean), JmpCf(o.jump));
    PackCf(p, ExecCf(7, 2, 0), o.then.w1 ? o.then : PredExecCf(9, 1, o.thenCondition));
    PackCf(p, o.otherwise.w1 ? o.otherwise : PredExecCf(10, 1, o.otherCondition), JmpCf(o.elseJump));
    PackCf(p, ExecCf(11, 1, 0, false, true), AllocCf(1));
    PackCf(p, ExecCf(12, 1, 0, true, true), OpCf(0));
    Alu m;
    m.vdest = 1, m.vmask = 0x1, m.s1 = T(0, "xxxx"), m.s2 = T(0, "xxxx"), m.s3 = T(0);
    Put(p, m);
    Alu set;
    set.sop = o.firstSet, set.sdest = 3, set.smask = o.firstSetMask, set.s1 = T(0), set.s2 = T(0), set.s3 = T(1, "xxxx");
    Put(p, set);
    Alu ge;
    ge.vop = 6, ge.vdest = 1, ge.vmask = 0x2, ge.s1 = T(1, "xxxx"), ge.s2 = C(0, "zzzz"), ge.s3 = T(0);
    Put(p, ge);
    set.sop = o.secondSet, set.smask = 0, set.s3 = T(1, "yyyy");
    Put(p, set);
    for (uint32_t c = 1; c <= 3; c++)
    {
        Alu b;
        b.vdest = 2, b.vmask = 0x1, b.s1 = C(c, "xxxx"), b.s2 = C(c, "xxxx");
        b.sop = c == 1 ? o.thenSop : 5, b.sdest = 1, b.smask = c == 1 ? o.thenScalarMask : 0x4, b.s3 = C(c, "wwww");
        b.pred = c == 1 ? o.thenPred : c == 2 || o.elsePred >= 0;
        b.predCond = c == 1 ? o.thenPredCond : c == 2 ? o.otherCondition : o.elsePred == 1;
        Put(p, b);
    }
    Alu e;
    e.vop = 0, e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(2, "xxxx"), e.s2 = T(1, "zzzz"), e.s3 = T(0);
    if (o.readPs)
        e.vmask = 0x7, e.smask = 0x8;  // RETAIN_PREV export62.w = ps
    Put(p, e);
    return p;
}

// ---- Tests -----------------------------------------------------------------------------------

void TestTextureFetchDecode()
{
    // word0: opcode 1 | fetch_valid_only (1 << 19 = 0x80000) | src swizzle x, y (0x4 << 26 =
    // 0x10000000) = 0x10080001. word1: dest xyzw 0x688 | mag 3 << 12 (0x3000) | min 3 << 14
    // (0xC000) | mip 3 << 16 (0x30000) | aniso 7 << 18 (0x1C0000) | vol mag 3 << 24 (0x3000000) |
    // vol min 3 << 26 (0xC000000) | computed LOD 1 << 28 = 0x1F1FF688. word2: dimension 2D
    // (1 << 14 = 0x4000) | register gradients (1) | sample location centre (2) | LOD bias -3
    // (7-bit 0x7D << 2 = 0x1F4) | offset x -1 (0x1F << 16 = 0x1F0000) | offset y 2 (2 << 21 =
    // 0x400000) = 0x5F41F7.
    const uint32_t w[3] = {0x10080001, 0x1F1FF688, 0x005F41F7};
    const TextureFetch t = DecodeTextureFetch(w);
    auto matches = [](const TextureFetch& t) {
        return t.srcReg == 0 && t.destReg == 0 && t.fetchValidOnly && t.constIndex == 0 && !t.unnormalized &&
               TextureSourceComponent(t, 0) == 0 && TextureSourceComponent(t, 1) == 1 && t.destSwizzle == 0x688 &&
               t.magFilter == 3 && t.minFilter == 3 && t.mipFilter == 3 && t.anisoFilter == 7 && t.arbitraryFilter == 0 &&
               t.volMagFilter == 3 && t.volMinFilter == 3 && t.useComputedLod && !t.useRegisterLod && !t.predicated &&
               t.useRegisterGradients && t.sampleLocation == 1 && t.lodBias == -3 && t.dimension == 1 &&
               t.offsetX == -1 && t.offsetY == 2 && t.offsetZ == 0;
    };
    CHECK(FetchOpcodeOf(w) == kTextureFetch && matches(t));
    // The encoder used by the program tests produces the same words.
    std::vector<uint32_t> p;
    TFetch e;
    e.regGrad = true, e.bias = -3, e.ox = -1, e.oy = 2;
    Put(p, e);
    std::vector<uint32_t> withLocation = {p[0], p[1], p[2] | 2};
    CHECK(withLocation == std::vector<uint32_t>(w, w + 3));
    // Controls: the bias read unsigned (bit 8 clear: 0x7D -> 0x3D reads +61), and constant index
    // 1 (bit 20).
    const uint32_t unsignedBias[3] = {w[0], w[1], w[2] & ~(1u << 8)};
    CONTROL(matches(DecodeTextureFetch(unsignedBias)));
    const uint32_t otherConst[3] = {w[0] | 1u << 20, w[1], w[2]};
    CONTROL(matches(DecodeTextureFetch(otherConst)));
}

void TestClearVs()
{
    ShaderTranslation t;
    std::string error;
    CHECK(Translate(ClearVs(ClearFull(), ClearMini()), false, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    const std::string& g = t.glsl;
    CHECK(g.rfind("#version 450\n", 0) == 0);
    CHECK(t.interpolatorCount == 1 && t.vertexFetchSlots == std::vector<uint32_t>{0} && t.textureSlots.empty());
    // r0.x preloaded with the index; slot 0: base word xe_vertex_base[0][0], fetch word 1 of
    // slot 0 = xe_fetch[0].y (size and endian); dword = index * 7 + offset + component.
    CHECK(Has(g, "vec4 r0 = vec4(float(xe_index), 0.0, 0.0, 0.0);"));
    CHECK(Has(g, "int xe_vidx0 = int(floor(r0.x));"));
    CHECK(Has(g, "uintBitsToFloat(xe_vertex_word(xe_vertex_base[0][0], xe_vidx0, 7u, 0, xe_fetch[0].y))"));
    CHECK(Has(g, "uintBitsToFloat(xe_vertex_word(xe_vertex_base[0][0], xe_vidx0, 7u, 2, xe_fetch[0].y)), 0.0)"));  // 3 components
    CHECK(Has(g, "r1.z = xe_fetch0.z;") && Has(g, "r1.w = 1.0;"));
    // The mini fetch reuses slot 0, the full fetch's index and stride, with offset 3.
    CHECK(Has(g, "xe_vertex_word(xe_vertex_base[0][0], xe_vidx0, 7u, 6, xe_fetch[0].y)") && Has(g, "r0.w = xe_fetch1.w;"));
    // Out-of-range fetches are flagged for the host, never read past the slot.
    CHECK(Has(g, "layout(std430, set = 0, binding = 5) buffer XeErrors { uint xe_error; };"));
    CHECK(Has(g, "if (!inside || at >= size)") && Has(g, "atomicOr(xe_error, 1u);"));
    // MAX(r, r) is a move.
    CHECK(Has(g, "precise vec4 xe_v0 = r0.xyzw;") && Has(g, "xe_interp_out[0].w = xe_v0.w;"));
    CHECK(Has(g, "precise vec4 xe_v1 = r1.xyzw;") && Has(g, "xe_position_out.x = xe_v1.x;"));
    CHECK(!Has(g, "xe_v2"));  // the no-op ALU writes nothing
    // Endian swap (8in16 = 1: bytes swapped in each half; 8in32 = 2: 8in16 then halves swapped;
    // 16in32 = 3: halves swapped). By hand: 0x11223344 -> 1: 0x22114433, 2: 0x44332211,
    // 3: 0x33441122, which the two steps below produce.
    CHECK(Has(g, "if (e == 1u || e == 2u)\n        v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);"));
    CHECK(Has(g, "if (e == 2u || e == 3u)\n        v = (v << 16) | (v >> 16);"));
    // Viewport fold (host contract): homogeneous, so it holds for w != 1.
    CHECK(Has(g, "gl_Position = vec4(p.xyz * xe_viewport_scale.xyz + xe_viewport_offset.xyz * p.w, p.w);"));
    CHECK(Has(g, "layout(location = 0) out vec4 xe_interp_0;"));
    CHECK(Has(g, "layout(std140, set = 0, binding = 2) uniform XeVsConstants"));
    CHECK(Compiles(g, false));

    // Rounded index: floor(x + 0.5).
    VFetch rounded = ClearFull();
    rounded.rounded = true;
    CHECK(Translate(ClearVs(rounded, ClearMini()), false, t) && Has(t.glsl, "int(floor(r0.x + 0.5))"));
}

void TestColourPs()
{
    ShaderTranslation t;
    CHECK(Translate(ColourPs(), true, t));
    const std::string& g = t.glsl;
    CHECK(t.interpolatorCount == 1);
    CHECK(Has(g, "layout(location = 0) in vec4 xe_interp_0;") && Has(g, "layout(location = 0) out vec4 xe_color0;"));
    CHECK(Has(g, "vec4 r0 = xe_interp_0;") && Has(g, "xe_color0.z = xe_v0.z;"));
    CHECK(Has(g, "layout(std140, set = 0, binding = 3) uniform XePsConstants { vec4 xe_c[256]; };"));
    CHECK(Compiles(g, true));

    // A PS reading r2 before writing it receives interpolators 0-2.
    Alu e;
    e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(2), e.s2 = T(2), e.s3 = T(0);
    CHECK(Translate(ColourPs(e), true, t) && t.interpolatorCount == 3 && Has(t.glsl, "vec4 r2 = xe_interp_2;"));
    CHECK(Compiles(t.glsl, true));

    // MAX of different operands keeps a >= b ? a : b, per component; clamp to [0, 1], NaN -> 0;
    // negate and the constant abs flag.
    Alu m;
    m.exp = true, m.vdest = 0, m.vmask = 0xF, m.vclamp = true, m.absConst = true, m.s1 = T(0), m.s2 = C(7, "wzyx"),
    m.s3 = T(0);
    m.s1.neg = true;
    CHECK(Translate(ColourPs(m), true, t));
    CHECK(Has(t.glsl, "precise vec4 xe_v0 = xe_sat(xe_max((-r0.xyzw), abs(xe_c[7]).wzyx));"));
    CHECK(Has(t.glsl, "vec4 xe_max(vec4 a, vec4 b) { return mix(b, a, greaterThanEqual(a, b)); }"));
    CHECK(Has(t.glsl, "vec4 xe_sat(vec4 x) { return mix(vec4(0.0), min(x, vec4(1.0)), greaterThanEqual(x, vec4(0.0))); }"));
    CHECK(Compiles(t.glsl, true));
}

void TestQuadsVs()
{
    ShaderTranslation t;
    std::string error;
    CHECK(Translate(QuadsVs(), false, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    const std::string& g = t.glsl;
    CHECK(t.interpolatorCount == 1 && t.vertexFetchSlots == std::vector<uint32_t>{95});
    // Slot 95: const index 31, select 2; base xe_vertex_base[23][3]; fetch word 2*95+1 = 191 =
    // xe_fetch[47].w (size and endian).
    CHECK(Has(g, "xe_vertex_word(xe_vertex_base[23][3], xe_vidx0, 4u, 0, xe_fetch[47].w)"));
    CHECK(Has(g, "r1.z = xe_fetch0.x;") && Has(g, "r1.w = xe_fetch0.y;") && Has(g, "r1.x = xe_fetch1.x;"));
    CHECK(Has(g, "xe_vertex_word(xe_vertex_base[23][3], xe_vidx0, 4u, 3, xe_fetch[47].w)"));
    // The slot's extent for windowed copies: stride 4 and the dword just past the furthest offset the
    // generated code reads (every xe_vertex_word offset + 1), so [0, high * 4 + end) covers every
    // fetch of indices <= high.
    {
        uint32_t furthest = 0;
        for (size_t at = g.find("xe_vidx0, 4u, "); at != std::string::npos; at = g.find("xe_vidx0, 4u, ", at + 1))
            furthest = std::max(furthest, uint32_t(std::stoul(g.substr(at + 14))) + 1);
        CHECK(t.vertexFetchStrides == std::vector<uint32_t>{4} && t.vertexFetchEnds == std::vector<uint32_t>{furthest} && furthest >= 4);
        CONTROL(t.vertexFetchEnds == std::vector<uint32_t>{furthest - 1});
    }
    // DOT2ADDv: src1.x * src2.x + src1.y * src2.y + src3.x on the swizzled operands.
    CHECK(Has(g, "precise vec4 xe_v0 = vec4(xe_dp2add(r1.zwww, xe_c[3].xyyy, xe_c[3].zzzz));") && Has(g, "r0.z = xe_v0.z;"));
    CHECK(Has(g, "vec4(xe_dp2add(r1.zwww, xe_c[0].xyyy, xe_c[0].zzzz));") && Has(g, "r0.y = xe_v3.y;"));
    CHECK(Has(g, "precise float s = xe_mul(a.x, b.x) + xe_mul(a.y, b.y);\n    precise float r = s + c.x;"));
    CHECK(Has(g, "return (a == 0.0 || b == 0.0) ? 0.0 : p;"));
    // ADDv and ADD_CONST_0 co-issued into the position.
    CHECK(Has(g, "precise vec4 xe_v4 = (r0.wwww + xe_c[1].wwww);") && Has(g, "xe_position_out.y = xe_v4.y;"));
    CHECK(Has(g, "precise float xe_s4 = (xe_c[0].w + r0.y);") && Has(g, "xe_position_out.x = xe_s4;"));
    CHECK(Has(g, "precise float xe_s5 = (xe_c[2].w + r0.x);") && Has(g, "xe_position_out.z = xe_s5;"));
    CHECK(Has(g, "xe_position_out.w = xe_v5.w;"));
    // MULv with the Direct3D 9 zero rule.
    CHECK(Has(g, "precise vec4 xe_v6 = xe_mul(r1.xxxy, xe_c[255].xxxy);") && Has(g, "r1.w = xe_v6.w;"));
    CHECK(Has(g, "xe_interp_out[0].x = xe_v7.x;"));
    CHECK(Compiles(g, false));

    // Rect-list main: ExpandRectList's order (len[0] edge 12, len[1] edge 20, len[2] edge 01,
    // strict comparisons), rotation, v3 = (v1 - v0) + v2, corners (v0, v1, v2, v1, v3, v2).
    CHECK(Has(g, "float len0 = xe_len2(p1, p2), len1 = xe_len2(p2, p0), len2 = xe_len2(p0, p1);"));
    CHECK(Has(g, "uint first = (len0 > len1 && len0 > len2) ? 0u : (len1 > len2 ? 1u : 2u);"));
    CHECK(Has(g, "if (first == 1u)\n    {\n        a = p1; b = p2; c = p0;"));
    CHECK(Has(g, "else if (first == 2u)\n    {\n        a = p2; b = p0; c = p1;"));
    CHECK(Has(g, "precise vec4 d = v1 - v0;\n    precise vec4 r = d + v2;"));
    CHECK(Has(g, "if (corner == 1u || corner == 3u)\n    {\n        p = b;"));
    CHECK(Has(g, "else if (corner == 2u || corner == 5u)\n    {\n        p = c;"));
    CHECK(Has(g, "else if (corner == 4u)\n    {\n        p = xe_mirror(a, b, c);"));
    CHECK(Has(g, "xe_vertex(xe_indices[3u * rect + 2u], p2, i2);"));
    CHECK(Has(g, "layout(std430, set = 0, binding = 4) readonly buffer XeIndices"));
    // Wrong-oracle control: the opposite rotation is not what is emitted.
    CONTROL(Has(g, "a = p1; b = p0; c = p2;"));
    // Hand-computed example of the selection rule: corners (0,0), (10,0), (0,5): len0 = |p1p2|^2
    // = 125, len1 = |p2p0|^2 = 25, len2 = |p0p1|^2 = 100 -> first = 0, so v3 = (10,0) - (0,0) +
    // (0,5) = (10,5), the missing corner. The emitted expressions above are exactly that rule.

    // Negative controls: (1) DOT2ADD's src1 and src3 exchanged; (2) ADD_CONST's swizzle field 0
    // cleared (b = x, not y).
    CHECK(Translate(QuadsVs(true), false, t));
    CONTROL(Has(t.glsl, "vec4(xe_dp2add(r1.zwww, xe_c[3].xyyy, xe_c[3].zzzz))"));
    CHECK(Translate(QuadsVs(false, Swz("xyzw")), false, t));
    CONTROL(Has(t.glsl, "precise float xe_s4 = (xe_c[0].w + r0.y);"));
}

// Two fetches (sequence 0x5) then TexturedPs's three ALU instructions: a downsample-like shape.
std::vector<uint32_t> TwoFetchPs(TFetch a, TFetch b)
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(2, 2, 0x5), AllocCf(2));
    PackCf(p, ExecCf(4, 3, 0, true), OpCf(0));
    Put(p, a);
    Put(p, b);
    const std::vector<uint32_t> one = TexturedPs();
    p.insert(p.end(), one.end() - 9, one.end());  // its three ALU instructions (3 words each)
    return p;
}

// FM4's 4-tap downsample fetches: explicit linear mag/min (word1 fields 1) and half-texel offsets
// (-2, 2 = one texel left, one down): the host gets the filters per slot, the coordinate moves by
// offset / 2 / size. Disagreeing filters for one slot are refused.
void TestFetchFiltersOffsets()
{
    TFetch f;
    f.mag = 1, f.min = 1, f.mip = 3, f.aniso = 7, f.ox = -2, f.oy = 2;
    ShaderTranslation t;
    std::string error;
    CHECK(Translate(TexturedPs(f), true, t, &error));
    CHECK(t.textureFilters.size() == 1 && t.textureFilters[0].mag == 1 && t.textureFilters[0].min == 1 && t.textureFilters[0].mip == 3);
    CHECK(Has(t.glsl, "texture(xe_texture_0, (vec2(r0.x, r0.y) + vec2(-2.0, 2.0) * 0.5 / vec2(textureSize(xe_texture_0, 0))));"));
    CHECK(Compiles(t.glsl, true));
    CONTROL(Has(t.glsl, "vec2(-2.0, 2.0) / vec2(textureSize"));  // whole-texel units would be wrong
    // The exponent bias: after the gamma block, every component, with the host's factor.
    CHECK(Has(t.glsl, "#endif\n#ifdef XE_TEXTURE_0_SCALE\n    xe_texel0 *= XE_TEXTURE_0_SCALE;\n#endif\n"));
    std::string scaled = t.glsl;
    scaled.insert(scaled.find('\n') + 1, "#define XE_TEXTURE_0_SCALE ldexp(1.0, 1)\n#define XE_TEXTURE_0_GAMMA\n");
    CHECK(Compiles(scaled, true));
    // Defaults: the fetch constant's filters (3), no offset expression.
    CHECK(Translate(TexturedPs(), true, t) && t.textureFilters[0].mag == 3 && t.textureFilters[0].min == 3 && !Has(t.glsl, "textureSize"));
    // use_computed_lod clear (and no register LOD): the explicit LOD 0, also in a pixel shader.
    TFetch explicitLod;
    explicitLod.compLod = false;
    CHECK(Translate(TexturedPs(explicitLod), true, t) && Has(t.glsl, "vec4 xe_texel0 = textureLod(xe_texture_0, vec2(r0.x, r0.y), 0.0);"));
    CHECK(Translate(TexturedPs(), true, t) && Has(t.glsl, "vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y));"));
    // Two fetches of slot 0: equal filters accepted, different refused.
    TFetch g = f;
    g.dest = 1, g.ox = 2, g.oy = -2;
    CHECK(Translate(TwoFetchPs(f, g), true, t, &error) && t.textureFilters.size() == 1);
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    g.mag = 0;
    CHECK(Refuses(TwoFetchPs(f, g), true, "set different filters"));
    // The instruction's mip filter is reported, and two fetches of one slot must agree on it.
    TFetch m = f;
    m.mip = 1;
    CHECK(Translate(TexturedPs(m), true, t) && t.textureFilters[0].mip == 1);
    TFetch m2 = f;
    m2.dest = 1, m2.mip = 0;
    CHECK(Refuses(TwoFetchPs(f, m2), true, "set different filters"));
}

// 3D fetches (instruction dimension 2, shaped like FM4's colour-grading LUT lookup): a sampler3D
// with U, V, W from the source swizzle's first three components; offsets in half texels on all
// three axes; the volume filters reported (2D fetches report 3) and refused where they cannot
// equal the sampler's mag/min; per-slot dimension reported and kept consistent.
void TestTexture3DFetch()
{
    // Decode. word0: opcode 1 | dest r2 (2 << 12 = 0x2000) | fetch_valid_only 0x80000 | const 9
    // (9 << 20 = 0x900000) | src swizzle x, y, z (0 | 1 << 2 | 2 << 4 = 0x24, << 26 = 0x90000000)
    // = 0x90982001. word1: dest xyzw 0x688 | mag 1 (0x1000) | min 1 (0x4000) | mip 3 (0x30000) |
    // aniso 7 (0x1C0000) | vol mag 1 (1 << 24) | vol min 1 (1 << 26) | computed LOD (1 << 28) =
    // 0x151F5688. word2: dimension 2 (2 << 14 = 0x8000) | offset z -3 (5-bit 0x1D << 26 =
    // 0x74000000) = 0x74008000.
    const uint32_t w[3] = {0x90982001, 0x151F5688, 0x74008000};
    const TextureFetch d = DecodeTextureFetch(w);
    CHECK(d.destReg == 2 && d.constIndex == 9 && d.dimension == kDimension3DOrStacked && d.offsetX == 0 && d.offsetY == 0 &&
          d.offsetZ == -3 && d.volMagFilter == 1 && d.volMinFilter == 1 && d.magFilter == 1 && d.minFilter == 1 &&
          TextureSourceComponent(d, 0) == 0 && TextureSourceComponent(d, 1) == 1 && TextureSourceComponent(d, 2) == 2);
    {
        std::vector<uint32_t> p;
        TFetch e;
        e.dest = 2, e.slot = 9, e.srcSwz = 0x24, e.mag = 1, e.min = 1, e.vmag = 1, e.vmin = 1, e.dim = 2, e.oz = -3;
        Put(p, e);
        CHECK(p == std::vector<uint32_t>(w, w + 3));
    }

    ShaderTranslation t;
    std::string error;
    TFetch f;
    f.slot = 7, f.srcSwz = 0x24, f.dim = 2;
    CHECK(Translate(TexturedPs(f), true, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    CHECK(Has(t.glsl, "layout(set = 0, binding = 15) uniform sampler3D xe_texture_7;\n"));
    CHECK(Has(t.glsl, "vec4 xe_texel0 = texture(xe_texture_7, vec3(r0.x, r0.y, r0.z));"));
    CHECK(t.textureSlots == std::vector<uint32_t>{7} && t.textureDimensions == std::vector<uint32_t>{2});
    CHECK(t.textureFilters.size() == 1 && t.textureFilters[0].mag == 3 && t.textureFilters[0].min == 3 &&
          t.textureFilters[0].volMag == 3 && t.textureFilters[0].volMin == 3);
    CHECK(Compiles(t.glsl, true));
    CONTROL(Has(t.glsl, "uniform sampler2D xe_texture_7;"));  // wrong oracle: a 2D declaration
    std::string gamma = t.glsl;
    gamma.insert(gamma.find('\n') + 1, "#define XE_TEXTURE_7_GAMMA\n#define XE_TEXTURE_7_SCALE ldexp(1.0, -1)\n");
    CHECK(Compiles(gamma, true));
    // The same fetch as 2D: two coordinates, sampler2D, dimension 1.
    TFetch flat = f;
    flat.dim = 1;
    CHECK(Translate(TexturedPs(flat), true, t) && Has(t.glsl, "uniform sampler2D xe_texture_7;") &&
          Has(t.glsl, "texture(xe_texture_7, vec2(r0.x, r0.y));") && t.textureDimensions == std::vector<uint32_t>{1});

    // Coordinates follow the swizzle (2 | 0 << 2 | 1 << 4 = 0x12: z, x, y); offsets -2, 2, 1 half
    // texels move each normalized coordinate by offset / 2 / size on its own axis.
    f.srcSwz = 0x12, f.ox = -2, f.oy = 2, f.oz = 1;
    CHECK(Translate(TexturedPs(f), true, t) &&
          Has(t.glsl, "texture(xe_texture_7, (vec3(r0.z, r0.x, r0.y) + vec3(-2.0, 2.0, 1.0) * 0.5 / vec3(textureSize(xe_texture_7, 0))));"));
    CHECK(Compiles(t.glsl, true));
    CONTROL(Has(t.glsl, "vec3(-2.0, 2.0, 1.0) / vec3(textureSize"));  // whole-texel units would be wrong
    // Only a Z offset still adds the offset term.
    f.srcSwz = 0x24, f.ox = 0, f.oy = 0, f.oz = -1;
    CHECK(Translate(TexturedPs(f), true, t) &&
          Has(t.glsl, "(vec3(r0.x, r0.y, r0.z) + vec3(0.0, 0.0, -1.0) * 0.5 / vec3(textureSize(xe_texture_7, 0)))"));
    f.oz = 0;

    // Volume filters: explicit and equal to mag/min, or either from the fetch constant: reported.
    f.mag = 1, f.min = 0, f.vmag = 1, f.vmin = 0;
    CHECK(Translate(TexturedPs(f), true, t) && t.textureFilters[0].mag == 1 && t.textureFilters[0].min == 0 &&
          t.textureFilters[0].volMag == 1 && t.textureFilters[0].volMin == 0);
    f.mag = 3, f.min = 3, f.vmag = 1, f.vmin = 0;
    CHECK(Translate(TexturedPs(f), true, t) && t.textureFilters[0].mag == 3 && t.textureFilters[0].min == 3 &&
          t.textureFilters[0].volMag == 1 && t.textureFilters[0].volMin == 0);
    f.mag = 1, f.min = 1, f.vmag = 3, f.vmin = 3;
    CHECK(Translate(TexturedPs(f), true, t) && t.textureFilters[0].volMag == 3 && t.textureFilters[0].volMin == 3);
    // Explicit and different: refused (a Vulkan sampler has no separate W filter); base map refused.
    f.mag = 1, f.min = 1, f.vmag = 0, f.vmin = 1;
    CHECK(Refuses(TexturedPs(f), true, "volume filters that differ from the mag/min filters"));
    f.vmag = 1, f.vmin = 0;
    CHECK(Refuses(TexturedPs(f), true, "volume filters that differ from the mag/min filters"));
    f.vmag = 2, f.vmin = 1;
    CHECK(Refuses(TexturedPs(f), true, "volume filter 2"));
    f.mag = 3, f.min = 3, f.vmag = 3, f.vmin = 3;
    // A 2D fetch ignores its volume filters: different explicit ones are accepted, reported as 3.
    flat.mag = 1, flat.vmag = 0, flat.vmin = 2;
    CHECK(Translate(TexturedPs(flat), true, t) && t.textureFilters[0].mag == 1 && t.textureFilters[0].volMag == 3 &&
          t.textureFilters[0].volMin == 3);

    // Two fetches of one slot: different dimensions refused (one declaration), equal accepted;
    // different volume filters refused. Slots 0 (2D) and 1 (3D): both declarations, dimensions
    // in slot order.
    TFetch a = f, b = f;
    a.slot = b.slot = 0, b.dest = 1;
    CHECK(Translate(TwoFetchPs(a, b), true, t) && t.textureDimensions == std::vector<uint32_t>{2});
    b.vmag = 0;
    CHECK(Refuses(TwoFetchPs(a, b), true, "set different filters"));
    b.vmag = 3, b.dim = 1, b.srcSwz = 0x4;
    CHECK(Refuses(TwoFetchPs(a, b), true, "use different dimensions"));
    a.dim = 1, a.srcSwz = 0x4, b.slot = 1, b.dim = 2, b.srcSwz = 0x24;
    CHECK(Translate(TwoFetchPs(a, b), true, t) && t.textureSlots == std::vector<uint32_t>({0, 1}) &&
          t.textureDimensions == std::vector<uint32_t>({1, 2}));
    CHECK(Has(t.glsl, "layout(set = 0, binding = 8) uniform sampler2D xe_texture_0;\nlayout(set = 0, binding = 9) uniform sampler3D xe_texture_1;\n"));
    CHECK(Compiles(t.glsl, true));

    {
        // Vertex shader: exec{tfetch r1 <- r0.x?? 3D}; alloc position; exec_end{MAXv export62 = r1}.
        // A VS preloads only r0.x: W from r0.y (swizzle x, x, y = 0x10) reads r0.y unwritten and
        // is refused; the same fetch as 2D ignores the third component and is accepted.
        auto vs = [](TFetch v) {
            std::vector<uint32_t> p;
            PackCf(p, ExecCf(2, 1, 0x1), AllocCf(1));
            PackCf(p, ExecCf(3, 1, 0, true), Cf{0, 0});
            v.dest = 1;
            Put(p, v);
            Alu e;
            e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(1), e.s2 = T(1), e.s3 = T(0);
            Put(p, e);
            return p;
        };
        TFetch v;
        v.dim = 2, v.srcSwz = 0;
        CHECK(Translate(vs(v), false, t) && Has(t.glsl, "layout(set = 0, binding = 8) uniform sampler3D xe_texture_0;\n") &&
              Has(t.glsl, "vec4 xe_texel0 = textureLod(xe_texture_0, vec3(r0.x, r0.x, r0.x), 0.0);"));
        CHECK(Compiles(t.glsl, false));
        v.srcSwz = 0x10;
        CHECK(Refuses(vs(v), false, "reads r0.y before writing it"));
        v.dim = 1;
        CHECK(Translate(vs(v), false, t) && Has(t.glsl, "textureLod(xe_texture_0, vec2(r0.x, r0.x), 0.0)"));
    }
}

void TestTexturedPs()
{
    ShaderTranslation t;
    std::string error;
    CHECK(Translate(TexturedPs(), true, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    const std::string& g = t.glsl;
    CHECK(t.interpolatorCount == 1 && t.textureSlots == std::vector<uint32_t>{0});
    CHECK(Has(g, "layout(set = 0, binding = 8) uniform sampler2D xe_texture_0;"));
    CHECK(Has(g, "vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y));") && Has(g, "r0.w = xe_texel0.w;"));
    CHECK(Has(g, "precise float xe_s0 = sqrt(abs(r0).x);") && Has(g, "xe_color0.x = xe_s0;"));
    CHECK(Has(g, "precise float xe_s1 = sqrt(abs(r0).y);") && Has(g, "xe_color0.y = xe_s1;"));
    CHECK(Has(g, "precise float xe_s2 = sqrt(abs(r0).z);") && Has(g, "xe_color0.z = xe_s2;"));
    CHECK(Has(g, "precise vec4 xe_v2 = r0.wwww;") && Has(g, "xe_color0.w = xe_v2.w;"));
    CHECK(Compiles(g, true));

    // Texture slot 5 -> binding 13; coordinates from r0.z, r0.x (src swizzle 2 | 0 << 2).
    TFetch other;
    other.slot = 5, other.srcSwz = 0x2;
    CHECK(Translate(TexturedPs(other), true, t));
    CHECK(Has(t.glsl, "binding = 13) uniform sampler2D xe_texture_5;") && Has(t.glsl, "vec2(r0.z, r0.x)"));
    CHECK(t.textureSlots == std::vector<uint32_t>{5} && Compiles(t.glsl, true));

    // Negative control: the abs bit cleared.
    CHECK(Translate(TexturedPs({}, false), true, t));
    CONTROL(Has(t.glsl, "sqrt(abs(r0).x)"));
}

void TestResolveAndPointsVs()
{
    ShaderTranslation t;
    CHECK(Translate(ResolveVs(), false, t));
    CHECK(t.interpolatorCount == 0 && t.vertexFetchSlots == std::vector<uint32_t>{0});
    CHECK(Has(t.glsl, "xe_vertex_word(xe_vertex_base[0][0], xe_vidx0, 2u, 0, xe_fetch[0].y)") && Has(t.glsl, "r0.z = 1.0;") && Has(t.glsl, "r0.w = 1.0;"));
    CHECK(!Has(t.glsl, "out vec4 xe_interp_0"));
    CHECK(Compiles(t.glsl, false));

    CHECK(Translate(PointsVs(), false, t));
    const std::string& g = t.glsl;
    CHECK(t.interpolatorCount == 1);
    CHECK(Has(g, "xe_interp_out[0].x = 0.0;") && Has(g, "xe_interp_out[0].w = 0.0;"));
    CHECK(Has(g, "xe_position_out.z = 0.0;") && Has(g, "xe_position_out.w = 1.0;"));
    CHECK(Compiles(g, false));
}

void TestVideoPs()
{
    ShaderTranslation t;
    std::string error;
    CHECK(Translate(VideoPs(), true, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    const std::string& g = t.glsl;
    // Both branches write r3, so only r0 is an interpolator; slots 0-2; bool 128.
    CHECK(t.interpolatorCount == 1 && t.textureSlots == (std::vector<uint32_t>{0, 1, 2}));
    CHECK(t.boolConstants == std::vector<uint32_t>{128});
    // Bool constants follow the 256 float constants in the PS constant block.
    CHECK(Has(g, "layout(std140, set = 0, binding = 3) uniform XePsConstants { vec4 xe_c[256]; uvec4 xe_bool[2]; };"));
    // Fetches: each result's x into one component of r1.
    // Fetches: each result's x into one component of r1, PWL-linearised (x, y, z) when the host
    // defines the slot's gamma macro.
    CHECK(Has(g, "vec4 xe_texel0 = texture(xe_texture_2, vec2(r0.x, r0.y));\n#endif\n#ifdef XE_TEXTURE_2_GAMMA\n"
                 "    xe_texel0.xyz = xe_pwl_gamma_to_linear(xe_texel0.xyz);\n#endif\n#ifdef XE_TEXTURE_2_SCALE\n    xe_texel0 *= XE_TEXTURE_2_SCALE;\n#endif\n    r1.x = xe_texel0.x;"));
    CHECK(Has(g, "vec4 xe_texel1 = texture(xe_texture_1, vec2(r0.x, r0.y));\n#endif\n#ifdef XE_TEXTURE_1_GAMMA\n"
                 "    xe_texel1.xyz = xe_pwl_gamma_to_linear(xe_texel1.xyz);\n#endif\n#ifdef XE_TEXTURE_1_SCALE\n    xe_texel1 *= XE_TEXTURE_1_SCALE;\n#endif\n    r1.y = xe_texel1.x;"));
    CHECK(Has(g, "vec4 xe_texel2 = texture(xe_texture_0, vec2(r0.x, r0.y));\n#endif\n#ifdef XE_TEXTURE_0_GAMMA\n"
                 "    xe_texel2.xyz = xe_pwl_gamma_to_linear(xe_texel2.xyz);\n#endif\n#ifdef XE_TEXTURE_0_SCALE\n    xe_texel2 *= XE_TEXTURE_0_SCALE;\n#endif\n    r1.z = xe_texel2.x;"));
    // Colour 0 encoded to PWL gamma (xyz, not w) after everything else, when the host asks.
    CHECK(Has(g, "    xe_color0.w = xe_s7;\n#ifdef XE_ALPHA_TEST\n    if (!xe_alpha_pass(xe_color0.w, XE_ALPHA_REF))\n        discard;\n#endif\n#ifdef XE_COLOR0_GAMMA\n    xe_color0.xyz = xe_linear_to_pwl_gamma(xe_color0.xyz);\n#endif\n#ifdef XE_COLOR0_7E3"));
    // MAXs(a, a) is a move; DOT4v over the swizzled operands, sums ((x + y) + z) + w, products by
    // the D3D9 rule; ADDs a + b = r0.w + r0.x, both read before either op writes.
    CHECK(Has(g, "precise float xe_s0 = xe_c[255].w;\n    r1.w = xe_s0;"));
    CHECK(Has(g, "precise vec4 xe_v1 = vec4(xe_dp4(xe_c[0].zxyw, r1.xzyw));\n    precise float xe_s1 = (r0.w + r0.x);\n"
                 "    r0.x = xe_v1.x;\n    r4.x = xe_s1;"));
    CHECK(Has(g, "precise float s = xe_mul(a.x, b.x) + xe_mul(a.y, b.y);\n    precise float t = s + xe_mul(a.z, b.z);\n"
                 "    precise float r = t + xe_mul(a.w, b.w);"));
    // Bool 128: dword 4 = xe_bool[1].x, bit 0; cf3 and cf4 on 1, cf5 on 0.
    const std::string on = "    if (((xe_bool[1].x >> 0) & 1u) == 1u)\n    {\n";
    const std::string off = "    if (((xe_bool[1].x >> 0) & 1u) == 0u)\n    {\n";
    // MULADDv a * b + c (the product by the D3D9 rule, then the sum); LOG_IEEE of |r1|.x.
    CHECK(Has(g, on + "        precise vec4 xe_v2 = (xe_mul(r0.xxxx, xe_c[253].wwww) + xe_c[254].yyyy);\n"
                      "        r1.x = xe_v2.x;\n        r1.y = xe_v2.y;\n        r1.z = xe_v2.z;\n"
                      "        precise float xe_s3 = xe_log2(abs(r1).x);\n        r2.x = xe_s3;\n    }\n"));
    // SETGTEv src1 >= src2 co-issued with EXP_IEEE of r2.x; CNDEv src1 == 0 ? src2 : src3.
    CHECK(Has(g, on + "        precise vec4 xe_v4 = xe_sge(xe_c[253].zzzz, r1.xyzw);\n        precise float xe_s4 = xe_exp2(r2.x);\n"));
    CHECK(Has(g, "        precise vec4 xe_v5 = xe_cndeq(r0.xyzw, r2.yyyy, r1.xyzw);\n        r3.x = xe_v5.x;"));
    CHECK(Has(g, off + "        precise vec4 xe_v6 = (xe_mul(r0.xxxx, xe_c[255].yyyy) + xe_c[255].xxxx);\n"));
    // Export: MAXv(r3, r3) into xyz, MAXs(c3.w, c3.x) into w.
    CHECK(Has(g, "precise vec4 xe_v7 = r3.xyzw;\n    precise float xe_s7 = (xe_c[3].w >= xe_c[3].x ? xe_c[3].w : xe_c[3].x);"));
    CHECK(Has(g, "xe_color0.z = xe_v7.z;") && Has(g, "xe_color0.w = xe_s7;"));
    // Helpers: per-component ordered comparisons; EXP_IEEE / LOG_IEEE exact cases: 2^0 = 1,
    // log2 1 = 0, log2 0 = -Inf (0xFF800000: sign, all-ones exponent), log2 of a negative = NaN
    // (0x7FC00000), 2^-Inf = 0.
    CHECK(Has(g, "vec4 xe_sge(vec4 a, vec4 b) { return mix(vec4(0.0), vec4(1.0), greaterThanEqual(a, b)); }"));
    CHECK(Has(g, "vec4 xe_cndeq(vec4 a, vec4 b, vec4 c) { return mix(c, b, equal(a, vec4(0.0))); }"));
    CHECK(Has(g, "    if (x == 0.0)\n        return 1.0;\n    if (isnan(x) || isinf(x))\n        return x > 0.0 || isnan(x) ? x : 0.0;\n    return exp2(x);"));
    CHECK(Has(g, "    if (x == 1.0)\n        return 0.0;\n    if (x == 0.0)\n        return uintBitsToFloat(0xFF800000u);\n"
                 "    if (x < 0.0)\n        return uintBitsToFloat(0x7FC00000u);\n    if (isnan(x) || isinf(x))\n        return x;\n    return log2(x);"));
    CHECK(Compiles(g, true));
    // With every gamma macro defined, as the host inserts them after line 1.
    {
        std::string defined = g;
        defined.insert(defined.find('\n') + 1, "#define XE_TEXTURE_0_GAMMA\n#define XE_TEXTURE_1_GAMMA\n"
                                                "#define XE_TEXTURE_2_GAMMA\n#define XE_COLOR0_GAMMA\n");
        CHECK(Compiles(defined, true));
    }

    // Bool 33: dword 1 = xe_bool[0].y, bit 1; bool 255: dword 7 = xe_bool[1].w, bit 31.
    VideoOptions o;
    o.boolIndex = 33;
    CHECK(Translate(VideoPs(o), true, t) && Has(t.glsl, "if (((xe_bool[0].y >> 1) & 1u) == 1u)") && t.boolConstants == std::vector<uint32_t>{33});
    CONTROL(Has(t.glsl, "xe_bool[1].x"));
    o.boolIndex = 255;
    CHECK(Translate(VideoPs(o), true, t) && Has(t.glsl, "if (((xe_bool[1].w >> 31) & 1u) == 0u)") && Compiles(t.glsl, true));

    // Negative controls: (1) cf5 on the same condition as cf3/cf4: no branch writes r3 when the
    // bool is 0, so r3 is read as interpolator 3 there (4 interpolators, not 1); (2) MULADDv's
    // src2 and src3 exchanged; (3) SETGTEv's operands exchanged; (4) CNDEv's src2 and src3
    // exchanged; (5) ADDs src3 swizzle wyzx (a = x, b = w).
    o = {}, o.elseCondition = true;
    CHECK(Translate(VideoPs(o), true, t));
    CONTROL(t.interpolatorCount == 1);
    CHECK(t.interpolatorCount == 4 && Has(t.glsl, "vec4 r3 = xe_interp_3;"));
    // The same with the unwritten path on bool value 1 (every condition 0).
    o = {}, o.thenCondition = false;
    CHECK(Translate(VideoPs(o), true, t) && t.interpolatorCount == 4 && Compiles(t.glsl, true));
    o = {}, o.swapMadAddend = true;
    CHECK(Translate(VideoPs(o), true, t));
    CONTROL(Has(t.glsl, "(xe_mul(r0.xxxx, xe_c[253].wwww) + xe_c[254].yyyy)"));
    o = {}, o.swapSge = true;
    CHECK(Translate(VideoPs(o), true, t));
    CONTROL(Has(t.glsl, "xe_sge(xe_c[253].zzzz, r1.xyzw)"));
    o = {}, o.swapCndeq = true;
    CHECK(Translate(VideoPs(o), true, t));
    CONTROL(Has(t.glsl, "xe_cndeq(r0.xyzw, r2.yyyy, r1.xyzw)"));
    o = {}, o.addsSwizzle = Swz("wyzx");
    CHECK(Translate(VideoPs(o), true, t));
    CONTROL(Has(t.glsl, "(r0.w + r0.x)"));
    CHECK(Has(t.glsl, "(r0.x + r0.w)"));

    // Shaders without conditional execs keep the plain constant block.
    CHECK(Translate(ColourPs(), true, t) && t.boolConstants.empty() && !Has(t.glsl, "xe_bool"));
}

// The PWL curves as the emitted helpers compute them (float arithmetic, Xenia's form): each
// segment's threshold, scale and offset are the ones the GLSL text checked below spells out.
// `slope1` is the 64-95 segment's scale numerator (2; a negative control passes 4).
float PwlToLinear(float g, float slope1 = 2.0f)
{
    g = g >= 0.0f ? (g < 1.0f ? g : 1.0f) : 0.0f;
    float scale = 1.0f / 1024.0f, offset = 0.0f;
    if (g >= 192.0f / 255.0f)
        scale = 8.0f / 1024.0f, offset = -1024.0f;
    else if (g >= 96.0f / 255.0f)
        scale = 4.0f / 1024.0f, offset = -256.0f;
    else if (g >= 64.0f / 255.0f)
        scale = slope1 / 1024.0f, offset = -64.0f;
    const float l = g * (255.0f * 1024.0f) * scale + offset;
    const float m = l + std::trunc(l * scale);
    return m * (1.0f / 1023.0f);
}
float LinearToPwl(float l)
{
    l = l >= 0.0f ? (l < 1.0f ? l : 1.0f) : 0.0f;
    float scale = 1023.0f, offset = 0.0f;
    if (l >= 512.0f / 1023.0f)
        scale = 1023.0f / 8.0f, offset = 128.0f / 255.0f;
    else if (l >= 128.0f / 1023.0f)
        scale = 1023.0f / 4.0f, offset = 64.0f / 255.0f;
    else if (l >= 64.0f / 1023.0f)
        scale = 1023.0f / 2.0f, offset = 32.0f / 255.0f;
    return std::trunc(l * scale) * (1.0f / 255.0f) + offset;
}

void TestPwlGamma()
{
    ShaderTranslation t;
    CHECK(Translate(ColourPs(), true, t));
    const std::string& g = t.glsl;
    // The helpers' thresholds, scales and offsets (segment table in shader_glsl.cpp).
    CHECK(Has(g, "bvec3 p1 = greaterThanEqual(g, vec3(64.0 / 255.0));\n    bvec3 p2 = greaterThanEqual(g, vec3(96.0 / 255.0));\n"
                 "    bvec3 p3 = greaterThanEqual(g, vec3(192.0 / 255.0));"));
    CHECK(Has(g, "vec3 scale = mix(mix(vec3(1.0 / 1024.0), vec3(2.0 / 1024.0), p1), mix(vec3(4.0 / 1024.0), vec3(8.0 / 1024.0), p3), p2);"));
    CHECK(Has(g, "vec3 offset = mix(mix(vec3(0.0), vec3(-64.0), p1), mix(vec3(-256.0), vec3(-1024.0), p3), p2);"));
    CHECK(Has(g, "precise vec3 l = g * (255.0 * 1024.0) * scale + offset;\n    precise vec3 m = l + trunc(l * scale);\n"
                 "    precise vec3 r = m * (1.0 / 1023.0);"));
    CHECK(Has(g, "bvec3 p1 = greaterThanEqual(l, vec3(64.0 / 1023.0));\n    bvec3 p2 = greaterThanEqual(l, vec3(128.0 / 1023.0));\n"
                 "    bvec3 p3 = greaterThanEqual(l, vec3(512.0 / 1023.0));"));
    CHECK(Has(g, "vec3 scale = mix(mix(vec3(1023.0), vec3(1023.0 / 2.0), p1), mix(vec3(1023.0 / 4.0), vec3(1023.0 / 8.0), p3), p2);"));
    CHECK(Has(g, "vec3 offset = mix(mix(vec3(0.0), vec3(32.0 / 255.0), p1), mix(vec3(64.0 / 255.0), vec3(128.0 / 255.0), p3), p2);"));
    CHECK(Has(g, "precise vec3 k = trunc(l * scale) * (1.0 / 255.0) + offset;"));
    CHECK(Has(g, "vec3 xe_saturate3(vec3 x) { return mix(vec3(0.0), min(x, vec3(1.0)), greaterThanEqual(x, vec3(0.0))); }"));
    // No texture: no texture macro; the colour macro is always available.
    CHECK(!Has(g, "XE_TEXTURE_") && Has(g, "#ifdef XE_COLOR0_GAMMA"));

    // Hand values (8-bit gamma k -> 10-bit linear n) from the segment definitions:
    // 0 -> 0, 63 -> 63, 64 -> 2*64-64 = 64, 95 -> 126, 96 -> 4*96-256 = 128, 128 -> 256 + 1 = 257,
    // 191 -> 508 + 1 = 509, 192 -> 512 + 4 = 516, 208 -> 640 + 5 = 645, 224 -> 768 + 6 = 774,
    // 255 -> 1016 + 7 = 1023. Inverse at the middle of each 10-bit step (n + 0.5) / 1023 gives
    // k back (truncating divisions).
    const uint32_t table[][2] = {{0, 0},     {63, 63},   {64, 64},   {95, 126},  {96, 128},  {128, 257},
                                 {191, 509}, {192, 516}, {208, 645}, {224, 774}, {255, 1023}};
    auto matches = [&](float slope1) {
        bool ok = true;
        for (const auto& e : table)
        {
            ok &= std::lround(PwlToLinear(float(e[0]) / 255.0f, slope1) * 1023.0f) == long(e[1]);
            ok &= std::lround(LinearToPwl((float(e[1]) + 0.5f) / 1023.0f) * 255.0f) == long(e[0]);
        }
        return ok;
    };
    CHECK(matches(2.0f));
    // Saturation (NaN -> 0) on both sides.
    CHECK(PwlToLinear(-1.0f) == 0.0f && PwlToLinear(2.0f) == 1.0f && LinearToPwl(std::nanf("")) == 0.0f && LinearToPwl(5.0f) == 1.0f);
    // Negative controls: a wrong slope for the 64-95 segment; the emitted text with that slope.
    CONTROL(matches(4.0f));
    CONTROL(Has(g, "vec3(1.0 / 1024.0), vec3(4.0 / 1024.0), p1)"));
}

void TestBranchesVs()
{
    // if / else on bool 5: r2.x is written on both paths.
    ShaderTranslation t;
    std::string error;
    CHECK(Translate(BranchesVs(true), false, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    CHECK(t.boolConstants == std::vector<uint32_t>{5});
    CHECK(Has(t.glsl, "layout(std140, set = 0, binding = 2) uniform XeVsConstants { vec4 xe_c[256]; uvec4 xe_bool[2]; };"));
    CHECK(Has(t.glsl, "if (((xe_bool[0].x >> 5) & 1u) == 1u)\n    {\n        precise vec4 xe_v1 = r1.xxxx;\n        r2.x = xe_v1.x;\n    }"));
    CHECK(Has(t.glsl, "if (((xe_bool[0].x >> 5) & 1u) == 0u)\n    {\n        precise vec4 xe_v2 = r0.xxxx;\n        r2.x = xe_v2.x;\n    }"));
    CHECK(Compiles(t.glsl, false));
    // Negative controls: no else, or an "else" on the same condition: r2.x is unwritten when
    // bool 5 is 0, which a VS refuses.
    CHECK(Refuses(BranchesVs(false), false, "reads r2.x before writing it"));
    CHECK(Refuses(BranchesVs(true, true), false, "reads r2.x before writing it"));
    CONTROL(Translate(BranchesVs(false), false, t));
}

// The compare and select families: each opcode becomes its helper with src1, src2 (, src3) in
// order, and each helper's GLSL is exactly its ISA definition (Xenia ucode.h): MINv a < b ? a : b;
// SETEv/SETGTv/SETNEv a ==/>/!= b ? 1 : 0; CNDGEv/CNDGTv a >=/> 0 ? b : c.
void TestCompareSelect()
{
    struct Case { uint32_t op; const char* call; };
    const Case cases[] = {{3, "xe_min(r0.xyzw, r1.xyzw)"}, {4, "xe_seq(r0.xyzw, r1.xyzw)"}, {5, "xe_sgt(r0.xyzw, r1.xyzw)"},
                          {7, "xe_sne(r0.xyzw, r1.xyzw)"}, {13, "xe_cndge(r0.xyzw, r1.xyzw, r2.xyzw)"},
                          {14, "xe_cndgt(r0.xyzw, r1.xyzw, r2.xyzw)"}, {8, "(r0.xyzw - floor(r0.xyzw))"},
                          {9, "trunc(r0.xyzw)"}, {10, "floor(r0.xyzw)"}};
    for (const Case& c : cases)
    {
        Alu a;
        a.exp = true, a.vmask = 0xF, a.s1 = T(0), a.s2 = T(1), a.s3 = T(2), a.vop = c.op;
        ShaderTranslation t;
        std::string error;
        const bool ok = Translate(ColourPs(a), true, t, &error);
        CHECK(ok && Has(t.glsl, c.call));
        if (!ok)
            std::fprintf(stderr, "  opcode %u: %s\n", c.op, error.c_str());
        else
            CHECK(Compiles(t.glsl, true));
    }
    Alu a;
    a.exp = true, a.vmask = 0xF, a.s1 = T(0), a.s2 = T(1), a.s3 = T(2), a.vop = 14;
    ShaderTranslation t;
    CHECK(Translate(ColourPs(a), true, t));
    const std::string& g = t.glsl;
    CHECK(Has(g, "vec4 xe_min(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }"));
    CHECK(Has(g, "vec4 xe_seq(vec4 a, vec4 b) { return mix(vec4(0.0), vec4(1.0), equal(a, b)); }"));
    CHECK(Has(g, "vec4 xe_sgt(vec4 a, vec4 b) { return mix(vec4(0.0), vec4(1.0), greaterThan(a, b)); }"));
    CHECK(Has(g, "vec4 xe_sne(vec4 a, vec4 b) { return mix(vec4(0.0), vec4(1.0), notEqual(a, b)); }"));
    CHECK(Has(g, "vec4 xe_cndge(vec4 a, vec4 b, vec4 c) { return mix(c, b, greaterThanEqual(a, vec4(0.0))); }"));
    CHECK(Has(g, "vec4 xe_cndgt(vec4 a, vec4 b, vec4 c) { return mix(c, b, greaterThan(a, vec4(0.0))); }"));
    // Scalar compares on src3.a (component pos 3) against 0, and MINs a < b.
    struct SCase { uint32_t op; const char* expr; };
    const SCase scases[] = {{22, "xe_rsq(r1.w)"}, {6, "(r1.w < r1.x ? r1.w : r1.x)"}, {7, "(r1.w == 0.0 ? 1.0 : 0.0)"}, {8, "(r1.w > 0.0 ? 1.0 : 0.0)"},
                            {9, "(r1.w >= 0.0 ? 1.0 : 0.0)"}, {10, "(r1.w != 0.0 ? 1.0 : 0.0)"}};
    for (const SCase& c : scases)
    {
        Alu s;
        s.exp = true, s.vmask = 0x7, s.smask = 0x8, s.s1 = T(0), s.s2 = T(0), s.s3 = T(1), s.vop = 2, s.sop = c.op;
        ShaderTranslation st;
        std::string error;
        const bool ok = Translate(ColourPs(s), true, st, &error);
        CHECK(ok && Has(st.glsl, std::string("precise float xe_s0 = ") + c.expr + ";"));
        if (!ok)
            std::fprintf(stderr, "  scalar %u: %s\n", c.op, error.c_str());
        else
            CHECK(Compiles(st.glsl, true));
    }
    {
        Alu q;
        q.exp = true, q.vmask = 0x7, q.smask = 0x8, q.s1 = T(0), q.s2 = T(0), q.s3 = T(1), q.vop = 2, q.sop = 22;
        ShaderTranslation st;
        CHECK(Translate(ColourPs(q), true, st) && Has(st.glsl, "    if (x == 1.0)\n        return 1.0;\n    if (x == 0.0)\n") &&
              Has(st.glsl, "    if (isinf(x) && x > 0.0)\n        return 0.0;\n    if (isnan(x) || x < 0.0)\n") &&
              Has(st.glsl, "    precise float s = sqrt(x);\n    precise float r = 1.0 / s;\n"));
        CONTROL(Has(st.glsl, "float xe_rsq(float x)\n{\n    precise float s = sqrt(x);"));  // no special cases would be wrong
    }
    // Negative controls: CNDGT written as >= and MIN as <= are wrong definitions.
    CONTROL(Has(g, "vec4 xe_cndgt(vec4 a, vec4 b, vec4 c) { return mix(c, b, greaterThanEqual(a, vec4(0.0))); }"));
    CONTROL(Has(g, "vec4 xe_min(vec4 a, vec4 b) { return mix(b, a, lessThanEqual(a, b)); }"));
}

// Colour 0 on a k_2_10_10_10_FLOAT target (XE_COLOR0_7E3): RGB clamped to [0, 31.875] and alpha
// to [0, 1], NaN and -0 to 0 (the greaterThan guard is false for them).
void TestColor7e3Clamp()
{
    ShaderTranslation t;
    CHECK(Translate(ColourPs(), true, t));
    CHECK(Has(t.glsl, "#ifdef XE_COLOR0_7E3\n    xe_color0 = mix(vec4(0.0), min(xe_color0, vec4(31.875, 31.875, 31.875, 1.0)), "
                      "greaterThan(xe_color0, vec4(0.0)));\n#endif\n#ifdef XE_COLOR0_SCALE\n    xe_color0 *= XE_COLOR0_SCALE;\n#endif\n}\n"));
    std::string defined = t.glsl;
    defined.insert(defined.find('\n') + 1, "#define XE_COLOR0_7E3\n");
    CHECK(Compiles(defined, true));
    // The 16_16_16_16 output scale (render.cpp defines it): compiles when defined, last in main.
    std::string scaled = t.glsl;
    scaled.insert(scaled.find('\n') + 1, "#define XE_COLOR0_SCALE (1.0 / 32.0)\n");
    CHECK(Compiles(scaled, true));
    CONTROL(Has(t.glsl, "greaterThanEqual(xe_color0, vec4(0.0))"));  // would let -0 through
    // Alpha test: before the output conversions, with the host's function and reference.
    CHECK(Has(t.glsl, "#ifdef XE_ALPHA_TEST\n    if (!xe_alpha_pass(xe_color0.w, XE_ALPHA_REF))\n        discard;\n#endif\n#ifdef XE_COLOR0_GAMMA"));
    CHECK(Has(t.glsl, "    case 4: return a > r;\n") && Has(t.glsl, "    case 1: return a < r;\n"));
    std::string tested = t.glsl;
    tested.insert(tested.find('\n') + 1, "#define XE_ALPHA_TEST 4\n#define XE_ALPHA_REF uintBitsToFloat(0x3F000000u)\n");
    CHECK(Compiles(tested, true));
    CONTROL(Has(t.glsl, "    case 4: return a >= r;\n"));
}

// Loops (ISA: loop_start skips the body when the constant's count, bits 0-7, is 0; loop_end
// decrements it and repeats while it is not 0, so the body runs exactly `count` times).
void TestLoops()
{
    ShaderTranslation t;
    std::string error;
    CHECK(Translate(LoopPs(), true, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    const std::string g = t.glsl;
    CHECK(t.loopConstants == std::vector<uint32_t>{31} && t.boolConstants.empty());
    CHECK(t.interpolatorCount == 1 && t.textureSlots == std::vector<uint32_t>{0});
    CHECK(t.textureFilters.size() == 1 && t.textureFilters[0].mag == 1 && t.textureFilters[0].min == 1);
    // Loop constants follow the bools (declared though unused) in the constant block.
    CHECK(Has(g, "layout(std140, set = 0, binding = 3) uniform XePsConstants { vec4 xe_c[256]; uvec4 xe_bool[2]; uvec4 xe_loop[8]; };"));
    // Loop constant 31: dword 31 = xe_loop[7].w, count = bits 0-7; the body `count` times.
    const std::string loop =
        "    precise vec4 xe_v0 = (r0.xyzw + xe_c[0].xyzw);\n"
        "    r1.x = xe_v0.x;\n    r1.y = xe_v0.y;\n    r1.z = xe_v0.z;\n    r1.w = xe_v0.w;\n"
        "    uint xe_loop_count0 = xe_loop[7].w & 0xFFu;\n"
        "    for (uint xe_i0 = 0u; xe_i0 < xe_loop_count0; xe_i0++)\n"
        "    {\n"
        "        precise vec4 xe_v1 = (xe_mul(r0.zzzz, xe_c[1].xyzw) + xe_c[2].xyzw);\n"
        "        r0.x = xe_v1.x;\n        r0.z = xe_v1.z;\n"
        "#ifdef XE_TEXTURE_0_LOD_BIAS\n        vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y), 0.0 + XE_TEXTURE_0_LOD_BIAS);\n#else\n"
        "        vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y));\n"
        "#endif\n"
        "#ifdef XE_TEXTURE_0_GAMMA\n        xe_texel0.xyz = xe_pwl_gamma_to_linear(xe_texel0.xyz);\n#endif\n"
        "#ifdef XE_TEXTURE_0_SCALE\n        xe_texel0 *= XE_TEXTURE_0_SCALE;\n#endif\n"
        "        r2.x = xe_texel0.x;\n        r2.y = xe_texel0.y;\n        r2.z = xe_texel0.z;\n        r2.w = xe_texel0.w;\n"
        "        precise vec4 xe_v2 = (r2.xyzw + r1.xyzw);\n"
        "        r1.x = xe_v2.x;\n        r1.y = xe_v2.y;\n        r1.z = xe_v2.z;\n        r1.w = xe_v2.w;\n"
        "    }\n"
        "    precise vec4 xe_v3 = xe_mul(r1.xyzw, xe_c[3].zzzz);\n"
        "    xe_color0.x = xe_v3.x;\n";
    CHECK(Has(g, loop));
    // The fetch constant's LOD bias (host macro): the computed LOD's bias; compiles when defined.
    CHECK(Has(g, "#ifdef XE_TEXTURE_0_LOD_BIAS\n        vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y), 0.0 + XE_TEXTURE_0_LOD_BIAS);\n#else\n"));
    {
        std::string biased = g;
        biased.insert(biased.find('\n') + 1, "#define XE_TEXTURE_0_LOD_BIAS (-24.0 / 32.0)\n");
        CHECK(Compiles(biased, true));
        CONTROL(Has(g, "textureLod(xe_texture_0, vec2(r0.x, r0.y), 0.0 + XE_TEXTURE_0_LOD_BIAS)"));
    }
    CHECK(Has(g, "    vec4 r0 = xe_interp_0;\n    vec4 r1 = vec4(0.0);\n    vec4 r2 = vec4(0.0);\n"));
    CHECK(Compiles(g, true));
    // Negative controls (wrong oracles): the count from bits 8-15 (aL start), the wrong dword,
    // one iteration too many.
    CONTROL(Has(g, "(xe_loop[7].w >> 8) & 0xFFu"));
    CONTROL(Has(g, "xe_loop[7].z & 0xFFu"));
    CONTROL(Has(g, "xe_i0 <= xe_loop_count0"));

    // Loop constant 5: dword 5 = xe_loop[1].y.
    LoopOpts o;
    o.id = o.endId = 5;
    CHECK(Translate(LoopPs(o), true, t) && Has(t.glsl, "uint xe_loop_count0 = xe_loop[1].y & 0xFFu;") && t.loopConstants == std::vector<uint32_t>{5});
    // loop_end's condition (word1 bit 10) only qualifies a predicated break: ignored without one.
    ShaderTranslation t0;
    o = {}, o.endCondition = false;
    CHECK(Translate(LoopPs(o), true, t0) && t0.glsl == g);

    // Zero-trip: r2 is first written in the body. Read after the loop it is undefined when the
    // count is 0: a VS refuses it, a PS reads the interpolator preload (r0-r2), as after a
    // conditional exec.
    CHECK(Translate(LoopVs(true), false, t) && Compiles(t.glsl, false) && t.loopConstants == std::vector<uint32_t>{0});
    CHECK(Has(t.glsl, "uvec4 xe_bool[2]; uvec4 xe_loop[8]; };") && Has(t.glsl, "uint xe_loop_count0 = xe_loop[0].x & 0xFFu;"));
    CHECK(Refuses(LoopVs(false), false, "reads r2.x before writing it"));
    CONTROL(Translate(LoopVs(false), false, t));
    // Iteration 1 sees the pre-loop registers: a body read of a register only the body writes.
    CHECK(Refuses(LoopVs(true, 3), false, "reads r3.x before writing it"));
    o = {}, o.exportReg = 2;
    CHECK(Translate(LoopPs(o), true, t) && t.interpolatorCount == 3 && Has(t.glsl, "vec4 r2 = xe_interp_2;") && Compiles(t.glsl, true));

    // Refused: nested loops, a stray loop_end, mismatched skip / body addresses or constants,
    // is_repeat, the address mode bit, a predicated break, an exec_end, conditional exec, export
    // or vertex fetch in the body, an aL-relative source in the body.
    o = {}, o.body = LoopStartCf(4, 31);  // an inner loop_start whose loop_end would lie past the outer one
    CHECK(Refuses(LoopPs(o), true, "does not close the loop"));
    {
        // Nested loops (FM4 race shaders): cf0 exec{ADDv r1 = r0 + c0}, cf1 loop_start(skip 6, constant 0),
        // cf2 loop_start(skip 5, constant 1), cf3 exec{ADDv r1 = r1 + c0}, cf4 loop_end(back 3, constant 1),
        // cf5 loop_end(back 2, constant 0), cf6 alloc colour, cf7 exec_end{export0 = r1 * c3}. Expected: the
        // inner for (constant 1 = xe_loop[0].y) inside the outer (constant 0 = xe_loop[0].x).
        std::vector<uint32_t> p;
        PackCf(p, ExecCf(4, 1, 0), LoopStartCf(6, 0));
        PackCf(p, LoopStartCf(5, 1), ExecCf(5, 1, 0));
        PackCf(p, LoopEndCf(3, 1), LoopEndCf(2, 0));
        PackCf(p, AllocCf(2), ExecCf(6, 1, 0, true));
        Alu a;
        a.vop = 0, a.vdest = 1, a.vmask = 0xF, a.s1 = T(0), a.s2 = C(0), a.s3 = T(0);
        Put(p, a);
        Alu b = a;
        b.s1 = T(1);
        Put(p, b);
        Alu e;
        e.vop = 1, e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(1), e.s2 = C(3), e.s3 = T(0);
        Put(p, e);
        ShaderTranslation nt;
        std::string err;
        CHECK(Translate(p, true, nt, &err) && nt.loopConstants == (std::vector<uint32_t>{0, 1}));
        if (!err.empty())
            std::fprintf(stderr, "  %s\n", err.c_str());
        const size_t outer = nt.glsl.find("for (uint xe_i0 = 0u; xe_i0 < xe_loop_count0; xe_i0++)");
        const size_t inner = nt.glsl.find("    for (uint xe_i1 = 0u; xe_i1 < xe_loop_count1; xe_i1++)");
        CHECK(outer != std::string::npos && inner != std::string::npos && outer < inner &&
              Has(nt.glsl, "uint xe_loop_count0 = xe_loop[0].x & 0xFFu;") && Has(nt.glsl, "uint xe_loop_count1 = xe_loop[0].y & 0xFFu;") &&
              Compiles(nt.glsl, true));
        CONTROL(inner != std::string::npos && outer > inner);  // the inner loop is not emitted first
    }
    o = {}, o.afterBody = LoopEndCf(2, 31);
    CHECK(Refuses(LoopPs(o), true, "no matching loop_start"));
    o = {}, o.skip = 5;
    CHECK(Refuses(LoopPs(o), true, "does not follow a loop_end"));
    o = {}, o.skip = 3;
    CHECK(Refuses(LoopPs(o), true, "does not follow a loop_end"));
    o = {}, o.back = 3;  // mutation: loop_end back to itself + 0 (instruction 3)
    CHECK(Refuses(LoopPs(o), true, "does not close the loop"));
    CONTROL(Translate(LoopPs(o), true, t));
    o = {}, o.endId = 30;
    CHECK(Refuses(LoopPs(o), true, "does not close the loop"));
    o = {}, o.endMode = true;
    CHECK(Refuses(LoopPs(o), true, "does not close the loop"));
    o = {}, o.startMode = true;
    CHECK(Refuses(LoopPs(o), true, "address mode"));
    o = {}, o.repeat = true;
    CHECK(Refuses(LoopPs(o), true, "is_repeat"));
    o = {}, o.predBreak = true;
    CHECK(Refuses(LoopPs(o), true, "predicated break"));
    o = {}, o.body = ExecCf(4, 3, 0x4, true);
    CHECK(Refuses(LoopPs(o), true, "exec_end (control-flow opcode 2) inside a loop"));
    o = {}, o.body = AllocCf(2);
    CHECK(Refuses(LoopPs(o), true, "alloc (control-flow opcode 12) inside a loop"));
    o = {}, o.body = CondExecCf(4, 3, 0, true, 0x4);
    CHECK(Refuses(LoopPs(o), true, "inside a loop is not supported"));
    o = {}, o.last.exp = true, o.last.vdest = 0, o.last.vmask = 0xF, o.last.s1 = T(1), o.last.s2 = T(1), o.last.s3 = T(1);
    CHECK(Refuses(LoopPs(o), true, "export inside a loop"));
    o = {}, o.last.vop = 0, o.last.vdest = 1, o.last.vmask = 0xF, o.last.s1 = T(2 | 0x40), o.last.s2 = T(1), o.last.s3 = T(0);
    CHECK(Refuses(LoopPs(o), true, "relatively addressed temp (aL)"));
    {
        // Per bool assignment: r2.x is written only when bool 0 is 1, then read in the body, so
        // the walk of the body on the bool-0 = 0 pass refuses it (the Emit walk alone, a union
        // over paths, would not). With an else writing r2.x too, it is accepted. CF: exec(4)
        // MAXv r1.x = r0.x; cexec(5) bool 0 MAXv r2.x = r0.x; [cexec(5) bool 0 == 0];
        // loop_start(skip 6, constant 2); exec(6) MAXv r3.x = r2.x; loop_end(body 4); alloc
        // position; exec_end(7) export62 = r1.xxxx.
        auto program = [](bool withElse) {
            std::vector<uint32_t> v;
            PackCf(v, ExecCf(4, 1, 0), CondExecCf(5, 1, 0, true));
            PackCf(v, withElse ? CondExecCf(5, 1, 0, false) : OpCf(0), LoopStartCf(6, 2));
            PackCf(v, ExecCf(6, 1, 0), LoopEndCf(4, 2));
            PackCf(v, AllocCf(1), ExecCf(7, 1, 0, true));
            Alu m;
            m.vdest = 1, m.vmask = 0x1, m.s1 = T(0, "xxxx"), m.s2 = T(0, "xxxx"), m.s3 = T(0);
            Put(v, m);
            m.vdest = 2;
            Put(v, m);
            m.vdest = 3, m.s1 = T(2, "xxxx"), m.s2 = T(2, "xxxx");
            Put(v, m);
            Alu e;
            e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(1, "xxxx"), e.s2 = T(1, "xxxx"), e.s3 = T(0);
            Put(v, e);
            return v;
        };
        CHECK(Refuses(program(false), false, "reads r2.x before writing it"));
        CHECK(Translate(program(true), false, t) && t.loopConstants == std::vector<uint32_t>{2} && t.boolConstants == std::vector<uint32_t>{0});
        CHECK(Has(t.glsl, "uint xe_loop_count0 = xe_loop[0].z & 0xFFu;") && Compiles(t.glsl, false));
        CONTROL(Translate(program(false), false, t));
    }
    {
        // Twelve bool constants and a loop: more than 12 pass dimensions. Eleven bools (the twelfth
        // cexec reuses bool 0): accepted. FM4 Recomp: the limit was 6. CF: cf0 alloc colours, cf1-12
        // cexecs of slot 9 (bools 0-10, then 11 or 0), cf13 loop_start(skip 16), cf14 exec(9), cf15
        // loop_end(body 14), cf16 exec_end(10), cf17 nop; slot 9 a no-op ALU, slot 10 export0 = r0.
        auto program = [](uint32_t bools) {
            std::vector<uint32_t> v;
            PackCf(v, AllocCf(2), CondExecCf(9, 1, 0, true));
            for (uint32_t b = 1; b < 11; b += 2)
                PackCf(v, CondExecCf(9, 1, b, true), CondExecCf(9, 1, b + 1, true));
            PackCf(v, CondExecCf(9, 1, bools == 12 ? 11 : 0, true), LoopStartCf(16, 0));
            PackCf(v, ExecCf(9, 1, 0), LoopEndCf(14, 0));
            PackCf(v, ExecCf(10, 1, 0, true), OpCf(0));
            Put(v, Alu{});
            Alu e;
            e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(0), e.s2 = T(0), e.s3 = T(0);
            Put(v, e);
            return v;
        };
        CHECK(Refuses(program(12), true, "more than 12 conditional exec bool constants, loops and predicate sets"));
        CHECK(Translate(program(11), true, t) && t.boolConstants.size() == 11 && t.loopConstants == std::vector<uint32_t>{0});
    }
    {
        // ResolveVs with its vertex fetch exec wrapped in a loop.
        std::vector<uint32_t> v;
        PackCf(v, LoopStartCf(3, 0), ExecCf(3, 1, 0x1));
        PackCf(v, LoopEndCf(1, 0), AllocCf(2));
        PackCf(v, AllocCf(1), ExecCf(4, 1, 0, true));
        const std::vector<uint32_t> r = ResolveVs();
        v.insert(v.end(), r.begin() + 6, r.end());
        CHECK(Refuses(v, false, "vertex fetch inside a loop"));
    }
}

void TestPreviousScalarAndJump()
{
    ShaderTranslation t;
    std::string error;
    CHECK(Translate(PrevPs(), true, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    const std::string g = t.glsl;
    // Only r0 is read before written; the jump's bool joins the bool constants.
    CHECK(t.interpolatorCount == 1 && t.boolConstants == std::vector<uint32_t>{128} && t.textureSlots.empty());
    CHECK(Has(g, "layout(std140, set = 0, binding = 3) uniform XePsConstants { vec4 xe_c[256]; uvec4 xe_bool[2]; };"));
    CHECK(Has(g, "    vec4 r4 = vec4(0.0);\n    float xe_ps = 0.0;\n"));
    const std::string body =
        // DOT3v over the swizzled operands; MAXs with an empty mask still runs and sets ps.
        "    precise vec4 xe_v0 = vec4(xe_dp3(r0.zyxw, xe_c[0].xyzw));\n"
        "    precise float xe_s0 = (r0.w >= r0.x ? r0.w : r0.x);\n"
        "    xe_ps = xe_s0;\n"
        "    r1.x = xe_v0.x;\n"
        // MUL_PREVs: a * ps by the D3D9 rule, then ps = the product.
        "    precise float xe_s1 = xe_mul(r0.y, xe_ps);\n"
        "    xe_ps = xe_s1;\n"
        "    r2.x = xe_s1;\n"
        // SUBs a - b = c1.w - c1.x; ps takes it unclamped, r2.y clamped.
        "    precise float xe_s2 = (xe_c[1].w - xe_c[1].x);\n"
        "    xe_ps = xe_s2;\n"
        "    r2.y = xe_sat(xe_s2);\n"
        // RETAIN_PREV copies ps and leaves it.
        "    precise float xe_s3 = xe_ps;\n"
        "    r2.z = xe_s3;\n"
        "    precise float xe_s4 = xe_rcp(r2.y);\n"
        "    xe_ps = xe_s4;\n"
        "    r2.w = xe_s4;\n"
        // cond_jmp taken when bool 128 == 0: the region runs when it is 1.
        "    if (((xe_bool[1].x >> 0) & 1u) == 1u)\n"
        "    {\n"
        "        precise float xe_s5 = xe_mul(xe_c[3].w, r0.y);\n"
        "        xe_ps = xe_s5;\n"
        "        r2.x = xe_s5;\n"
        "        precise vec4 xe_v6 = r0.xyzw;\n"
        "        precise float xe_s6 = (xe_c[2].w + xe_c[2].x);\n"
        "        xe_ps = xe_s6;\n"
        "        r4.x = xe_v6.x;\n        r4.y = xe_v6.y;\n        r4.z = xe_v6.z;\n        r4.w = xe_v6.w;\n"
        "    }\n"
        "    precise vec4 xe_v7 = r2.xyzw;\n"
        "    precise float xe_s7 = xe_ps;\n"
        "    xe_color0.x = xe_v7.x;\n    xe_color0.y = xe_v7.y;\n    xe_color0.z = xe_v7.z;\n    xe_color0.w = xe_s7;\n";
    CHECK(Has(g, body));
    // DOT3v: ((x + y) + z), no w; RECIP_IEEE: 1 / 1 = 1, 1 / +-0 = +-Inf (sign | 0x7F800000),
    // 1 / +-Inf = +-0 (the sign alone), NaN passes, else the host's division.
    CHECK(Has(g, "float xe_dp3(vec4 a, vec4 b)\n{\n    precise float s = xe_mul(a.x, b.x) + xe_mul(a.y, b.y);\n"
                 "    precise float r = s + xe_mul(a.z, b.z);\n    return r;\n}\n"));
    CHECK(Has(g, "float xe_rcp(float x)\n{\n    if (x == 1.0)\n        return 1.0;\n    if (x == 0.0)\n"
                 "        return uintBitsToFloat((floatBitsToUint(x) & (1u << 31)) | 0x7F800000u);\n"
                 "    if (isinf(x))\n        return uintBitsToFloat(floatBitsToUint(x) & (1u << 31));\n"
                 "    if (isnan(x))\n        return x;\n    precise float r = 1.0 / x;\n    return r;\n}\n"));
    CHECK(Compiles(g, true));
    // Wrong oracles: ps updated with the clamped value; the region on the jump's own condition;
    // DOT4 for DOT3.
    CONTROL(Has(g, "xe_ps = xe_sat("));
    CONTROL(Has(g, "if (((xe_bool[1].x >> 0) & 1u) == 0u)"));
    CONTROL(Has(g, "xe_dp4(r0.zyxw"));
    // Programs that read no ps are unchanged: no xe_ps, no DOT3 / RECIP helpers.
    CHECK(Translate(ColourPs(), true, t) && !Has(t.glsl, "xe_ps") && !Has(t.glsl, "xe_dp3") && !Has(t.glsl, "xe_rcp"));

    PrevOptions o;
    // Jump polarity: taken when bool 128 == 1, so the region runs when it is 0.
    o.jump.condition = true;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "    if (((xe_bool[1].x >> 0) & 1u) == 0u)\n    {\n        precise float xe_s5"));
    CONTROL(Has(t.glsl, "if (((xe_bool[1].x >> 0) & 1u) == 1u)"));
    // Bool 37: dword 1 = xe_bool[0].y, bit 5.
    o = {}, o.jump.boolIndex = 37;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "if (((xe_bool[0].y >> 5) & 1u) == 1u)") && t.boolConstants == std::vector<uint32_t>{37});
    // SUBs operand order: swizzle wyzx gives a = x, b = w.
    o = {}, o.subsSwizzle = Swz("wyzx");
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "(xe_c[1].x - xe_c[1].w)"));
    CONTROL(Has(t.glsl, "(xe_c[1].w - xe_c[1].x)"));
    // MUL_CONST_1 (43): temp index bit 0 from the opcode (r1); a temp source 3 sets bit 1 (r2).
    o = {}, o.mulcOpcode = 43;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "xe_mul(xe_c[3].w, r1.y)"));
    o = {}, o.mulcTemp = true;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "xe_mul(xe_c[3].w, r2.y)"));
    CONTROL(Has(t.glsl, "xe_mul(xe_c[3].w, r0.y)"));
    // ADD_CONST_0 in the same place is a sum.
    o = {}, o.mulcOpcode = 44;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "precise float xe_s5 = (xe_c[3].w + r0.y);"));
    // SUB_CONST_0 (46) is the constant minus the temp (FM4's car shader); SUB_CONST_1
    // (47) the same with bit 0 of the temp index from the opcode.
    o = {}, o.mulcOpcode = 46;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "precise float xe_s5 = (xe_c[3].w - r0.y);") && Compiles(t.glsl, true));
    CONTROL(Has(t.glsl, "(xe_c[3].w + r0.y)") || Has(t.glsl, "(r0.y - xe_c[3].w)"));
    o = {}, o.mulcOpcode = 47;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "precise float xe_s5 = (xe_c[3].w - r1.y);"));
    // SIN / COS of src3's a component (swizzle xyzw: w), radians; the result feeds ps.
    o = {}, o.firstScalar = 48;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "precise float xe_s0 = sin(r0.w);\n    xe_ps = xe_s0;\n") && Compiles(t.glsl, true));
    CONTROL(Has(t.glsl, "cos(r0.w)") || Has(t.glsl, "sin(r0.x)"));
    o = {}, o.firstScalar = 49;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "precise float xe_s0 = cos(r0.w);\n"));
    // r4, written only when the jump is not taken, read after it: undefined on the taken path,
    // so a pixel shader preloads r0-r4.
    o = {}, o.exportReg = 4;
    CHECK(Translate(PrevPs(o), true, t) && t.interpolatorCount == 5 && Has(t.glsl, "vec4 r4 = xe_interp_4;") && Compiles(t.glsl, true));

    // ADD_PREVs / SUB_PREVs in slot 4 (FM4's race-loading pixel shader): a + ps and
    // a - ps with a = r0.y (src3 swizzle xyzy, component 3 + 1), ps = MAXs's r0.w >= r0.x ? ...
    // (Xenia ucode.h via ReCarbon: "src0.a + ps", "src0.a - ps"; hand-written).
    o = {}, o.prevOpcode = 1;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "    precise float xe_s1 = (r0.y + xe_ps);\n    xe_ps = xe_s1;\n    r2.x = xe_s1;\n") &&
          Compiles(t.glsl, true));
    CONTROL(Has(t.glsl, "(r0.y - xe_ps)") || Has(t.glsl, "(xe_ps + r0.y)") || Has(t.glsl, "xe_mul(r0.y, xe_ps)"));
    o = {}, o.prevOpcode = 26;
    CHECK(Translate(PrevPs(o), true, t) && Has(t.glsl, "    precise float xe_s1 = (r0.y - xe_ps);\n    xe_ps = xe_s1;\n") && Compiles(t.glsl, true));
    CONTROL(Has(t.glsl, "(xe_ps - r0.y)") || Has(t.glsl, "(r0.y + xe_ps)"));
    o = {}, o.prevOpcode = 1, o.firstRetain = true;
    CHECK(Refuses(PrevPs(o), true, "ADD_PREVs reads the previous scalar result before any scalar op sets it"));
    o = {}, o.prevOpcode = 26, o.firstRetain = true;
    CHECK(Refuses(PrevPs(o), true, "SUB_PREVs reads the previous scalar result before any scalar op sets it"));
    // ps rule: with slot 3's scalar op RETAIN_PREV, no scalar op runs before MUL_PREVs.
    o = {}, o.firstRetain = true;
    CHECK(Refuses(PrevPs(o), true, "MUL_PREVs reads the previous scalar result before any scalar op sets it"));
    CONTROL(Translate(PrevPs(o), true, t));
    {
        // ps set only in a jump's region, read after it: unset on the taken path. CF: alloc; exec(3)
        // RETAIN_PREV (no write); cond_jmp to 4; exec(4) ADDs (mask empty); exec_end(5) export0 =
        // r0.xyz + RETAIN_PREV w. With the jump replaced by a nop, accepted.
        auto program = [](bool jump) {
            std::vector<uint32_t> v;
            PackCf(v, AllocCf(2), ExecCf(3, 1, 0));
            Jmp j;
            PackCf(v, jump ? JmpCf(j) : OpCf(0), ExecCf(4, 1, 0));
            PackCf(v, ExecCf(5, 1, 0, true), OpCf(0));
            Put(v, Alu{});
            Alu add;
            add.sop = 0, add.s1 = T(0), add.s2 = T(0), add.s3 = C(2);
            Put(v, add);
            Alu e;
            e.exp = true, e.vdest = 0, e.vmask = 0x7, e.s1 = T(0), e.s2 = T(0), e.smask = 0x8, e.s3 = T(0);
            Put(v, e);
            return v;
        };
        CHECK(Refuses(program(true), true, "RETAIN_PREV writes the previous scalar result before any scalar op sets it"));
        CONTROL(Translate(program(true), true, t));
        CHECK(Translate(program(false), true, t) && Has(t.glsl, "precise float xe_s1 = (xe_c[2].w + xe_c[2].x);\n    xe_ps = xe_s1;\n")
              && Compiles(t.glsl, true));
    }
    {
        // ps through a loop: exec(4) MAXs (mask empty, before the loop when `before`) ;
        // loop_start(skip 4, constant 0); exec(5) ADDs (mask empty); loop_end(body 2); alloc;
        // exec_end(6) export0 = r0.xyz + RETAIN_PREV w. Without the pre-loop op, ps is unset on
        // the zero-trip path.
        auto program = [](bool before) {
            std::vector<uint32_t> v;
            PackCf(v, ExecCf(4, 1, 0), LoopStartCf(4, 0));
            PackCf(v, ExecCf(5, 1, 0), LoopEndCf(2, 0));
            PackCf(v, AllocCf(2), ExecCf(6, 1, 0, true));
            PackCf(v, OpCf(0), OpCf(0));
            Alu m;
            m.sop = before ? 5 : 50, m.s1 = T(0), m.s2 = T(0), m.s3 = T(0, "wwwz");
            Put(v, m);
            Alu add;
            add.sop = 0, add.s1 = T(0), add.s2 = T(0), add.s3 = C(2);
            Put(v, add);
            Alu e;
            e.exp = true, e.vdest = 0, e.vmask = 0x7, e.s1 = T(0), e.s2 = T(0), e.smask = 0x8, e.s3 = T(0);
            Put(v, e);
            return v;
        };
        CHECK(Refuses(program(false), true, "RETAIN_PREV writes the previous scalar result before any scalar op sets it"));
        CHECK(Translate(program(true), true, t, &error));
        CHECK(Has(t.glsl, "    precise float xe_s0 = (r0.z >= r0.w ? r0.z : r0.w);\n    xe_ps = xe_s0;\n"
                          "    uint xe_loop_count0 = xe_loop[0].x & 0xFFu;\n"
                          "    for (uint xe_i0 = 0u; xe_i0 < xe_loop_count0; xe_i0++)\n    {\n"
                          "        precise float xe_s1 = (xe_c[2].w + xe_c[2].x);\n        xe_ps = xe_s1;\n    }\n"
                          "    precise vec4 xe_v2 = r0.xyzw;\n    precise float xe_s2 = xe_ps;\n"));
        CHECK(Compiles(t.glsl, true));
    }
    {
        // A scalar op with an empty mask reads its operands only when the program reads ps: a VS
        // MAXs of the unwritten r5 is dead without a ps reader, refused with one. CF: alloc
        // position; exec_end(2, 2): MAXs (empty) of r5; MAXv export62 = r0.xxxx + `sop` export
        // w (RETAIN_PREV reads ps; MAXs of r0.x does not).
        auto program = [](uint32_t sop) {
            std::vector<uint32_t> v;
            PackCf(v, AllocCf(1), ExecCf(1, 2, 0, true));
            Alu m;
            m.sop = 5, m.s1 = T(0), m.s2 = T(0), m.s3 = T(5);
            Put(v, m);
            Alu e;
            e.exp = true, e.vdest = 62, e.vmask = 0x7, e.s1 = T(0, "xxxx"), e.s2 = T(0, "xxxx"), e.smask = 0x8, e.sop = sop,
            e.s3 = T(0, "xxxx");
            Put(v, e);
            return v;
        };
        CHECK(Translate(program(5), false, t) && !Has(t.glsl, "r5") && Compiles(t.glsl, false));
        CHECK(Refuses(program(50), false, "reads r5.w before writing it"));
    }
    {
        // DOT3v reads x, y, z only: a VS with r1.xyz written translates; DOT4v of the same refuses
        // r1.w. CF: alloc position; exec_end(2, 2): MAXv r1.xyz = r0.xxxx; DOTnv export62 = r1.r1.
        auto program = [](uint32_t vop) {
            std::vector<uint32_t> v;
            PackCf(v, AllocCf(1), ExecCf(1, 2, 0, true));
            Alu m;
            m.vdest = 1, m.vmask = 0x7, m.s1 = T(0, "xxxx"), m.s2 = T(0, "xxxx"), m.s3 = T(0);
            Put(v, m);
            Alu e;
            e.vop = vop, e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(1), e.s2 = T(1), e.s3 = T(0);
            Put(v, e);
            return v;
        };
        CHECK(Translate(program(16), false, t) && Has(t.glsl, "precise vec4 xe_v1 = vec4(xe_dp3(r1.xyzw, r1.xyzw));") && Compiles(t.glsl, false));
        CHECK(Refuses(program(15), false, "reads r1.w before writing it"));
    }

    {
        // A jump and a cexec on the same bool: the jump's region (bool 5 == 1) writes r2.x, the
        // cexec on bool 5 == `cexecCondition` reads it. Pairing the paths right accepts 1 and
        // refuses 0 (the read then runs on the pass where the jump skipped the write). CF: alloc
        // position; exec(3) MAXv r1.x = r0.x; cond_jmp to 4 on bool 5 == 0; exec(4) MAXv r2.x =
        // r1.x; cexec(5) bool 5 MAXv r1.x = r2.x; exec_end(6) export62 = r1.xxxx.
        auto program = [](bool cexecCondition) {
            std::vector<uint32_t> v;
            PackCf(v, AllocCf(1), ExecCf(3, 1, 0));
            Jmp j;
            j.boolIndex = 5;
            PackCf(v, JmpCf(j), ExecCf(4, 1, 0));
            PackCf(v, CondExecCf(5, 1, 5, cexecCondition), ExecCf(6, 1, 0, true));
            Alu m;
            m.vdest = 1, m.vmask = 0x1, m.s1 = T(0, "xxxx"), m.s2 = T(0, "xxxx"), m.s3 = T(0);
            Put(v, m);
            m.vdest = 2, m.s1 = T(1, "xxxx"), m.s2 = T(1, "xxxx");
            Put(v, m);
            m.vdest = 1, m.s1 = T(2, "xxxx"), m.s2 = T(2, "xxxx");
            Put(v, m);
            Alu e;
            e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(1, "xxxx"), e.s2 = T(1, "xxxx"), e.s3 = T(0);
            Put(v, e);
            return v;
        };
        CHECK(Translate(program(true), false, t) && t.boolConstants == std::vector<uint32_t>{5} && Compiles(t.glsl, false));
        CHECK(Has(t.glsl, "    if (((xe_bool[0].x >> 5) & 1u) == 1u)\n    {\n        precise vec4 xe_v1 = r1.xxxx;\n"));
        CHECK(Refuses(program(false), false, "reads r2.x before writing it"));
        CONTROL(Translate(program(false), false, t));
    }

    // Jump refusals: unconditional, predicated, address mode, direction, backward / self; a
    // nested jump, exec_end, cexec, loop or alloc in the region; an export or texture fetch in
    // it; a jump inside a loop; a region running past the program.
    o = {}, o.jump.unconditional = true;
    CHECK(Refuses(PrevPs(o), true, "unconditional jump"));
    o = {}, o.jump.predicated = true;
    CHECK(Refuses(PrevPs(o), true, "predicated jump"));
    o = {}, o.jump.addressMode = true;
    CHECK(Refuses(PrevPs(o), true, "address mode"));
    o = {}, o.jump.direction = true;
    CHECK(Refuses(PrevPs(o), true, "direction"));
    o = {}, o.jump.target = 1;
    CHECK(Refuses(PrevPs(o), true, "backward jump to control-flow instruction 1"));
    o = {}, o.jump.target = 2;
    CHECK(Refuses(PrevPs(o), true, "backward jump to control-flow instruction 2"));
    CONTROL(Translate(PrevPs(o), true, t));
    o = {}, o.region = JmpCf(Jmp{});
    CHECK(Refuses(PrevPs(o), true, "jump (control-flow opcode 11) inside a jump's skipped region"));
    // FM4 Recomp: an exec_end is accepted as a region's last instruction (TestUniformBranches); one
    // before the last (cf3 of the region cf3-cf4) is refused.
    o = {}, o.jump.target = 5, o.region = ExecCf(8, 2, 0, true);
    CHECK(Refuses(PrevPs(o), true, "exec_end (control-flow opcode 2) inside a jump's skipped region"));
    {
        // FM4 Recomp: a bool-constant cexec (bool 5) nested in the bool jump's region (bool 128, run
        // where its bit is 1): `if (bool 128) { if (bool 5) { ... } }`.
        o = {}, o.region = CondExecCf(8, 2, 5, true);
        std::string why;
        CHECK(Translate(PrevPs(o), true, t, &why) && t.boolConstants == (std::vector<uint32_t>{5, 128}) && Compiles(t.glsl, true));
        if (!why.empty())
            std::fprintf(stderr, "  %s\n", why.c_str());
        CHECK(Has(t.glsl, "    if (((xe_bool[1].x >> 0) & 1u) == 1u)\n    {\n        if (((xe_bool[0].x >> 5) & 1u) == 1u)\n        {\n"));
        CONTROL(Has(t.glsl, "    if (((xe_bool[0].x >> 5) & 1u) == 1u)\n    {\n"));  // not at the top level
        // An export after the nested cexec, still inside the region (cf3 cexec, cf4 the export exec;
        // the region ends at cf5): refused, the cexec leaves the region conditional.
        o.jump.target = 5, o.afterJump = ExecCf(10, 1, 0);
        CHECK(Refuses(PrevPs(o), true, "export inside a jump's skipped region"));
    }
    o = {}, o.region = AllocCf(2);
    CHECK(Refuses(PrevPs(o), true, "inside a jump's skipped region"));
    o = {}, o.region = LoopStartCf(5, 0);
    CHECK(Refuses(PrevPs(o), true, "loop (control-flow opcode 7) inside a jump's skipped region"));
    o = {}, o.jump.target = 5, o.afterJump = ExecCf(10, 1, 0);  // the export exec in the region
    CHECK(Refuses(PrevPs(o), true, "export inside a jump's skipped region"));
    o = {}, o.jump.target = 6, o.afterJump = ExecCf(10, 0, 0);  // the region runs into the instruction slots
    CHECK(Refuses(PrevPs(o), true, "without exec_end"));
    {
        std::vector<uint32_t> q;
        PackCf(q, AllocCf(2), JmpCf(Jmp{3}));
        PackCf(q, ExecCf(2, 1, 0x1), ExecCf(3, 1, 0, true));
        Put(q, TFetch{});
        Alu e;
        e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(0), e.s2 = T(0), e.s3 = T(0);
        Put(q, e);
        // FM4 Recomp: a texture fetch in a bool-constant jump's region samples in place (uniform
        // control flow): see TestUniformBranches.
        CHECK(Translate(q, true, t) && Has(t.glsl, "        vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y));\n"));
        LoopOpts lo;
        lo.body = JmpCf(Jmp{3});
        CHECK(Refuses(LoopPs(lo), true, "jump (control-flow opcode 11) inside a loop"));
    }
}

// The predicate p0 (PRED_SETEs / PRED_SETNEs, predicated ALU, predicated exec, predicated and
// unconditional cond_jmp) and MULs; expectations from the definitions quoted in shader_glsl.cpp
// (Xenia ucode.h): PRED_SETEs p0 = a == 0, result p0 ? 0 : 1; PRED_SETNEs p0 = a != 0; a
// predicated instruction or exec runs where p0 equals its condition; a predicated jump is taken
// where p0 equals its condition; MULs a * b by the D3D9 rule.
void TestPredicate()
{
    ShaderTranslation t;
    std::string error;
    CHECK(Translate(PredVs(), false, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    const std::string g = t.glsl;
    // No bool constants: the predicate is not a constant, and needs no binding.
    CHECK(t.boolConstants.empty() && t.loopConstants.empty() && t.interpolatorCount == 0);
    CHECK(Has(g, "uniform XeVsConstants { vec4 xe_c[256]; };"));
    CHECK(Has(g, "    vec4 r2 = vec4(0.0);\n    bool xe_p0 = false;\n"));
    const std::string body =
        "    precise vec4 xe_v0 = r0.xxxx;\n"
        "    r1.x = xe_v0.x;\n"
        // PRED_SETEs with an empty mask still sets p0; no result is kept.
        "    xe_p0 = (r1.x == 0.0);\n"
        // The jump is taken where p0 == 0: the region runs where p0 is set.
        "    if (xe_p0)\n"
        "    {\n"
        "        precise vec4 xe_v2 = xe_sge(r1.xxxx, xe_c[0].zzzz);\n"
        "        r1.y = xe_v2.y;\n"
        "        xe_p0 = (r1.y != 0.0);\n"
        // (p0) exec and (!p0) exec: both halves of each instruction under the exec's test.
        "        if (xe_p0)\n"
        "        {\n"
        "            precise vec4 xe_v4 = xe_c[1].xxxx;\n"
        "            precise float xe_s4 = xe_c[1].w;\n"
        "            r2.x = xe_v4.x;\n"
        "            r1.z = xe_s4;\n"
        "        }\n"
        "        if (!xe_p0)\n"
        "        {\n"
        "            precise vec4 xe_v5 = xe_c[2].xxxx;\n"
        "            precise float xe_s5 = xe_c[2].w;\n"
        "            r2.x = xe_v5.x;\n"
        "            r1.z = xe_s5;\n"
        "        }\n"
        "    }\n"
        // The unconditional jump at the region's end skips cf6: cf6 runs where the first was taken.
        "    else\n"
        "    {\n"
        "        precise vec4 xe_v6 = xe_c[3].xxxx;\n"
        "        precise float xe_s6 = xe_c[3].w;\n"
        "        r2.x = xe_v6.x;\n"
        "        r1.z = xe_s6;\n"
        "    }\n"
        "    precise vec4 xe_v7 = (r2.xxxx + r1.zzzz);\n"
        "    xe_position_out.x = xe_v7.x;\n";
    CHECK(Has(g, body));
    CHECK(Compiles(g, false));
    // Wrong oracles: the region on the jump's own condition; PRED_SETEs as != ; the else branch
    // run unconditionally after the region.
    CONTROL(Has(g, "    if (!xe_p0)\n    {\n        precise vec4 xe_v2"));
    CONTROL(Has(g, "xe_p0 = (r1.x != 0.0);"));
    CONTROL(Has(g, "    }\n    precise vec4 xe_v6"));

    {
        // PRED_SETGTs (29) / PRED_SETGTEs (30) as the two sets: p0 = a > 0 / a >= 0 (Xenia ucode.h).
        PredOptions gt;
        gt.firstSet = 29, gt.secondSet = 30;
        CHECK(Translate(PredVs(gt), false, t) && Has(t.glsl, "    xe_p0 = (r1.x > 0.0);\n") && Has(t.glsl, "        xe_p0 = (r1.y >= 0.0);\n") &&
              Compiles(t.glsl, false));
        CONTROL(Has(t.glsl, "xe_p0 = (r1.x >= 0.0);") || Has(t.glsl, "xe_p0 = (r1.y > 0.0);"));
    }
    PredOptions o;
    // Polarity: a jump taken where p0 == 1 runs its region where p0 == 0; the (p0) exec as
    // (!p0) exec with its instruction predicated on 0 tests !p0.
    o.jump.condition = true;
    CHECK(Translate(PredVs(o), false, t) && Has(t.glsl, "    if (!xe_p0)\n    {\n        precise vec4 xe_v2") && Compiles(t.glsl, false));
    CONTROL(Has(t.glsl, "    if (xe_p0)\n    {\n        precise vec4 xe_v2"));
    o = {}, o.thenCondition = false, o.thenPredCond = false, o.otherCondition = true;
    CHECK(Translate(PredVs(o), false, t) && Has(t.glsl, "        if (!xe_p0)\n        {\n            precise vec4 xe_v4") &&
          Has(t.glsl, "        if (xe_p0)\n        {\n            precise vec4 xe_v5"));
    CONTROL(Has(t.glsl, "        if (xe_p0)\n        {\n            precise vec4 xe_v4"));
    // Both execs on the same condition: r2.x is unwritten where the second set gives p0 == 1.
    o.otherCondition = false;
    CHECK(Refuses(PredVs(o), false, "reads r2.x before writing it"));
    // The op mapping: 28 first is !=, 27 second is ==; PRED_SETEs's result is 0 where p0 is set,
    // else 1 (here written to r3.x).
    o = {}, o.firstSet = 28, o.secondSet = 27;
    CHECK(Translate(PredVs(o), false, t) && Has(t.glsl, "xe_p0 = (r1.x != 0.0);") && Has(t.glsl, "xe_p0 = (r1.y == 0.0);"));
    CONTROL(Has(t.glsl, "xe_p0 = (r1.x == 0.0);"));
    o = {}, o.firstSetMask = 0x1;
    CHECK(Translate(PredVs(o), false, t) &&
          Has(t.glsl, "    xe_p0 = (r1.x == 0.0);\n    precise float xe_s1 = (xe_p0 ? 0.0 : 1.0);\n    r3.x = xe_s1;\n") &&
          Compiles(t.glsl, false));
    CONTROL(Has(t.glsl, "(xe_p0 ? 1.0 : 0.0)"));
    // ps: a predicate set updates ps (unclamped), a predicated-off op does not (its update is
    // under the test). With RETAIN_PREV exporting ps, the (p0) op MAXs updates ps in its block.
    o = {}, o.readPs = true;
    CHECK(Translate(PredVs(o), false, t, &error) && Compiles(t.glsl, false));
    CHECK(Has(t.glsl, "    xe_p0 = (r1.x == 0.0);\n    precise float xe_s1 = (xe_p0 ? 0.0 : 1.0);\n    xe_ps = xe_s1;\n"));
    CHECK(Has(t.glsl, "        if (xe_p0)\n        {\n            precise vec4 xe_v4 = xe_c[1].xxxx;\n"
                      "            precise float xe_s4 = xe_c[1].w;\n            xe_ps = xe_s4;\n"));

    // A predicated write that may not happen: with the (!p0) exec gone, r2.x and r1.z are
    // unwritten where the second set clears p0, so the VS read after is refused.
    o = {}, o.otherwise = ExecCf(10, 0, 0);  // an exec of no instructions (a nop's w1 is 0)
    CHECK(Refuses(PredVs(o), false, "reads r2.x before writing it"));
    CONTROL(Translate(PredVs(o), false, t));
    // r1.z not written by the (p0) exec: unwritten where the region ran with the second p0 set
    // (the else branch, which writes it, is jumped over).
    o = {}, o.thenScalarMask = 0;
    CHECK(Refuses(PredVs(o), false, "reads r1.z before writing it"));
    // With slot 8 not a predicate set, cf3 / cf4 test the first p0, which is 1 in the region:
    // only the (p0) exec runs there, so without its r1.z write r1.z is unwritten.
    o = {}, o.secondSet = 5;
    CHECK(Translate(PredVs(o), false, t) && Compiles(t.glsl, false));
    o.thenScalarMask = 0;
    CHECK(Refuses(PredVs(o), false, "reads r1.z before writing it"));
    CONTROL(Translate(PredVs(o), false, t));
    // The else branch runs where the jump was taken, so where p0 == 0: a (!p0) instruction there
    // runs, a (p0) one does not (r2.x then unwritten).
    o = {}, o.elsePred = 0;
    CHECK(Translate(PredVs(o), false, t) && Has(t.glsl, "    else\n    {\n        if (!xe_p0)\n        {\n            precise vec4 xe_v6") &&
          Compiles(t.glsl, false));
    o.elsePred = 1;
    CHECK(Refuses(PredVs(o), false, "reads r2.x before writing it"));
    CONTROL(Translate(PredVs(o), false, t));
    {
        // Two if / else shapes in sequence. PS: cf0 alloc colours; cf1 exec(6) PRED_SETEs
        // a = r0.x; cf2 predicated jump to cf5 (p0 == 0); cf3 exec(7) MAXv r1.x = c0.x; cf4 jmp
        // to cf6; cf5 exec(8) MAXv r1.x = c1.x; cf6 predicated jump to cf9; cf7 exec(9) MAXv
        // r2.x = c2.x; cf8 jmp to cf10; cf9 exec(10) MAXv r2.x = c3.x; cf10 exec_end(11)
        // ADDv export0 = r1.xxxx + r2.xxxx; cf11 nop.
        std::vector<uint32_t> v;
        const Jmp pj = {5, 0, false, false, true}, uj = {6, 0, false, true};
        Jmp pj2 = pj, uj2 = uj;
        pj2.target = 9, uj2.target = 10;
        PackCf(v, AllocCf(2), ExecCf(6, 1, 0));
        PackCf(v, JmpCf(pj), ExecCf(7, 1, 0));
        PackCf(v, JmpCf(uj), ExecCf(8, 1, 0));
        PackCf(v, JmpCf(pj2), ExecCf(9, 1, 0));
        PackCf(v, JmpCf(uj2), ExecCf(10, 1, 0));
        PackCf(v, ExecCf(11, 1, 0, true), OpCf(0));
        Alu set;
        set.sop = 27, set.s1 = T(0), set.s2 = T(0), set.s3 = T(0, "xxxx");
        Put(v, set);
        for (uint32_t c = 0; c < 4; c++)
        {
            Alu m;
            m.vdest = 1 + c / 2, m.vmask = 0x1, m.s1 = C(c, "xxxx"), m.s2 = C(c, "xxxx"), m.s3 = T(0);
            Put(v, m);
        }
        Alu e;
        e.vop = 0, e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(1, "xxxx"), e.s2 = T(2, "xxxx"), e.s3 = T(0);
        Put(v, e);
        CHECK(Translate(v, true, t, &error) && t.interpolatorCount == 1 && Compiles(t.glsl, true));
        CHECK(Has(t.glsl, "    }\n    if (xe_p0)\n    {\n        precise vec4 xe_v3 = xe_c[2].xxxx;\n        r2.x = xe_v3.x;\n    }\n"
                          "    else\n    {\n        precise vec4 xe_v4 = xe_c[3].xxxx;\n        r2.x = xe_v4.x;\n    }\n"));
    }
    {
        // p0 and a bool constant are independent: VS alloc position; exec(3) PRED_SETEs a = r0.x;
        // cexec(4) on bool 5 == 1: (p0) MAXv r1.x = r0.x; cexec(5) on bool 5 == 0: (!p0) MAXv
        // r1.x = r0.x; exec_end(6) export62 = r1.xxxx. Unwritten where bool 5 != p0: refused.
        // With slot 3 also writing r1.x (MAXv r1.x = r0.x co-issued), accepted.
        auto program = [](bool writeBefore) {
            std::vector<uint32_t> v;
            PackCf(v, AllocCf(1), ExecCf(3, 1, 0));
            PackCf(v, CondExecCf(4, 1, 5, true), CondExecCf(5, 1, 5, false));
            PackCf(v, ExecCf(6, 1, 0, true), OpCf(0));
            Alu set;
            set.sop = 27, set.s1 = T(0, "xxxx"), set.s2 = T(0, "xxxx"), set.s3 = T(0, "xxxx");
            set.vdest = 1, set.vmask = writeBefore ? 0x1 : 0;
            Put(v, set);
            Alu b;
            b.pred = true, b.predCond = true, b.vdest = 1, b.vmask = 0x1, b.s1 = T(0, "xxxx"), b.s2 = T(0, "xxxx"), b.s3 = T(0);
            Put(v, b);
            b.predCond = false;
            Put(v, b);
            Alu e;
            e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(1, "xxxx"), e.s2 = T(1, "xxxx"), e.s3 = T(0);
            Put(v, e);
            return v;
        };
        CHECK(Refuses(program(false), false, "reads r1.x before writing it"));
        CONTROL(Translate(program(false), false, t));
        CHECK(Translate(program(true), false, t) && t.boolConstants == std::vector<uint32_t>{5} &&
              Has(t.glsl, "    if (((xe_bool[0].x >> 5) & 1u) == 1u)\n    {\n        if (xe_p0)\n        {\n") && Compiles(t.glsl, false));
    }
    {
        // Predicated ALU in a plain exec: alloc position; exec(2, 3): MAXv r1.x = r0.x (or a
        // no-op), PRED_SETNEs a = r0.x, (p0) MAXv r1.x = c0.x; exec_end(5): export62 = r1.xxxx.
        auto program = [](bool writeFirst) {
            std::vector<uint32_t> v;
            PackCf(v, AllocCf(1), ExecCf(2, 3, 0));
            PackCf(v, ExecCf(5, 1, 0, true), OpCf(0));
            Alu m;
            m.vdest = 1, m.vmask = writeFirst ? 0x1 : 0, m.s1 = T(0, "xxxx"), m.s2 = T(0, "xxxx"), m.s3 = T(0);
            Put(v, m);
            Alu set;
            set.sop = 28, set.s1 = T(0), set.s2 = T(0), set.s3 = T(0, "xxxx");
            Put(v, set);
            Alu b;
            b.pred = true, b.predCond = true, b.vdest = 1, b.vmask = 0x1, b.s1 = C(0, "xxxx"), b.s2 = C(0, "xxxx"), b.s3 = T(0);
            Put(v, b);
            Alu e;
            e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(1, "xxxx"), e.s2 = T(1, "xxxx"), e.s3 = T(0);
            Put(v, e);
            return v;
        };
        CHECK(Translate(program(true), false, t) && Compiles(t.glsl, false));
        CHECK(Has(t.glsl, "    xe_p0 = (r0.x != 0.0);\n    if (xe_p0)\n    {\n        precise vec4 xe_v2 = xe_c[0].xxxx;\n"
                          "        r1.x = xe_v2.x;\n    }\n    precise vec4 xe_v3 = r1.xxxx;\n"));
        CHECK(Refuses(program(false), false, "reads r1.x before writing it"));
        CONTROL(Translate(program(false), false, t));
    }
    {
        // p0 set only inside a bool jump's region, read after it: unset where the jump is taken.
        // PS: alloc colours; cond_jmp to cf3 on bool 5 == 1; exec(2) PRED_SETEs a = r0.x;
        // exec_end(3, 2): (p0) MAXv r0.x = c0.x, export0 = r0. With the jump a nop: accepted.
        auto program = [](bool jump) {
            std::vector<uint32_t> v;
            Jmp j;
            j.target = 3, j.boolIndex = 5, j.condition = true;
            PackCf(v, AllocCf(2), jump ? JmpCf(j) : OpCf(0));
            PackCf(v, ExecCf(2, 1, 0), ExecCf(3, 2, 0, true));
            Alu set;
            set.sop = 27, set.s1 = T(0), set.s2 = T(0), set.s3 = T(0, "xxxx");
            Put(v, set);
            Alu b;
            b.pred = true, b.predCond = true, b.vdest = 0, b.vmask = 0x1, b.s1 = C(0, "xxxx"), b.s2 = C(0, "xxxx"), b.s3 = T(0);
            Put(v, b);
            Alu e;
            e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(0), e.s2 = T(0), e.s3 = T(0);
            Put(v, e);
            return v;
        };
        CHECK(Refuses(program(true), true, "predicated ALU instruction reads the predicate before any predicate-setting op sets it"));
        CONTROL(Translate(program(true), true, t));
        CHECK(Translate(program(false), true, t) && Compiles(t.glsl, true));
    }

    // Refusals: an unpredicated instruction or one predicated on the other condition in a
    // predicated exec; a predicate set in a predicated exec, in an exec whose predicate-clean bit
    // is set, in a bool cexec (clean by definition) or in a loop; a predicated export; the
    // predicated exec_end; an unconditional jump with a predicate bit or not at a region's end;
    // a predicated jump with a bool constant field or before any predicate set.
    o = {}, o.thenPred = false;
    CHECK(Refuses(PredVs(o), false, "unpredicated ALU instruction inside a predicated exec"));
    o = {}, o.thenPredCond = false;
    CHECK(Refuses(PredVs(o), false, "opposite condition of its predicated exec"));
    o = {}, o.thenSop = 28;
    CHECK(Refuses(PredVs(o), false, "predicate set inside a predicated exec"));
    o = {}, o.firstClean = true;
    CHECK(Refuses(PredVs(o), false, "predicate-clean hint"));
    o = {}, o.then = CondExecCf(8, 1, 0, true);  // cf2's instructions (slot 8 sets p0) in a bool cexec
    // FM4 Recomp: a cexec inside a jump's region is accepted; the set in it is refused by the cexec's
    // implied predicate-clean hint (it was refused for the region before).
    CHECK(Refuses(PredVs(o), false, "predicate-clean hint"));
    {
        std::vector<uint32_t> q;
        PackCf(q, AllocCf(2), CondExecCf(2, 1, 0, true));
        PackCf(q, ExecCf(3, 1, 0, true), OpCf(0));
        Alu set;
        set.sop = 27, set.s1 = T(0), set.s2 = T(0), set.s3 = T(0, "xxxx");
        Put(q, set);
        Alu e;
        e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(0), e.s2 = T(0), e.s3 = T(0);
        Put(q, e);
        CHECK(Refuses(q, true, "predicate-clean hint"));
        q.clear();  // the same as a plain exec: accepted
        PackCf(q, AllocCf(2), ExecCf(2, 1, 0));
        PackCf(q, ExecCf(3, 1, 0, true), OpCf(0));
        Put(q, set);
        Put(q, e);
        CHECK(Translate(q, true, t) && Has(t.glsl, "xe_p0 = (r0.x == 0.0);") && Compiles(t.glsl, true));
        // Thirteen predicate sets in two execs (6 + 7): more pass dimensions than allowed (12; FM4
        // Recomp: was 6); twelve (6 + 6): accepted.
        for (int n : {7, 6})
        {
            q.clear();
            PackCf(q, AllocCf(2), ExecCf(2, 6, 0));
            PackCf(q, ExecCf(8, n, 0), ExecCf(8 + n, 1, 0, true));
            for (int i = 0; i < 6 + n; i++)
                Put(q, set);
            Put(q, e);
            if (n == 7)
                CHECK(Refuses(q, true, "more than 12 bool constants, loops and predicate sets"));
            else
                CHECK(Translate(q, true, t) && Compiles(t.glsl, true));
        }
        // A predicated export (after a predicate set).
        q.clear();
        PackCf(q, AllocCf(2), ExecCf(1, 2, 0, true));
        Put(q, set);
        e.pred = true;
        Put(q, e);
        CHECK(Refuses(q, true, "predicated export"));
    }
    {
        LoopOpts lo;
        lo.last.vop = 0, lo.last.vdest = 1, lo.last.vmask = 0xF, lo.last.s1 = T(2), lo.last.s2 = T(1);
        lo.last.sop = 27, lo.last.s3 = T(0);
        CHECK(Refuses(LoopPs(lo), true, "predicate set inside a loop"));
    }
    o = {}, o.then = PredExecCf(9, 1, true), o.then.w1 |= 1u << 11;
    CHECK(Refuses(PredVs(o), false, "predicated exec with word 1 bit 11 (address mode)"));
    o = {}, o.otherwise = Cf{PredExecCf(10, 1, false).w0, 6u << 12};
    CHECK(Refuses(PredVs(o), false, "predicated exec (control-flow opcode 6) inside a jump's skipped region"));
    o = {}, o.elseJump.predicated = true;
    CHECK(Refuses(PredVs(o), false, "unconditional jump with predicate"));
    o = {}, o.elseJump.boolIndex = 3;
    CHECK(Refuses(PredVs(o), false, "unconditional jump with predicate, bool constant or condition fields"));
    o = {}, o.jump.boolIndex = 3;
    CHECK(Refuses(PredVs(o), false, "predicated jump with a bool constant field"));
    o = {}, o.jump.predicated = false, o.jump.unconditional = true;  // not inside a region
    CHECK(Refuses(PredVs(o), false, "unconditional jump other than at the end of a conditional jump's region"));
    o = {}, o.firstSet = 5;  // cf1 then reads p0 before any set
    CHECK(Refuses(PredVs(o), false, "predicated jump reads the predicate before any predicate-setting op sets it"));
    CONTROL(Refuses(PredVs(), false, ""));

    // MULs a * b by the D3D9 rule, a = w and b = x for swizzle xyzw; wyzx swaps them.
    Alu mul;
    mul.exp = true, mul.vdest = 0, mul.vmask = 0x7, mul.s1 = T(0), mul.s2 = T(0), mul.sop = 2, mul.smask = 0x8, mul.s3 = T(0);
    CHECK(Translate(ColourPs(mul), true, t) && Has(t.glsl, "    precise float xe_s0 = xe_mul(r0.w, r0.x);\n") &&
          Has(t.glsl, "    xe_color0.w = xe_s0;\n") && Compiles(t.glsl, true));
    CONTROL(Has(t.glsl, "(r0.w * r0.x)"));
    mul.s3 = T(0, "wyzx");
    CHECK(Translate(ColourPs(mul), true, t) && Has(t.glsl, "xe_mul(r0.x, r0.w)"));
    CONTROL(Has(t.glsl, "xe_mul(r0.w, r0.x)"));
}

// A VS exporting only interpolator 0's x (and the position): accepted, the other components keep
// the zero every interpolator output starts from (as Xenia).
void TestPartialInterpolator()
{
    std::vector<uint32_t> vs;
    PackCf(vs, ExecCf(2, 2, 0), AllocCf(1));
    PackCf(vs, AllocCf(2), ExecCf(4, 1, 0, true));
    Alu p;
    p.exp = true, p.vdest = 62, p.vmask = 0xF, p.s1 = T(0, "xxxx"), p.s2 = T(0, "xxxx"), p.s3 = T(0);
    Put(vs, p);
    Alu nop;
    Put(vs, nop);
    Alu e;
    e.exp = true, e.vdest = 0, e.vmask = 0x1, e.s1 = T(0, "xxxx"), e.s2 = T(0, "xxxx"), e.s3 = T(0);
    Put(vs, e);
    ShaderTranslation t;
    std::string error;
    const bool ok = Translate(vs, false, t, &error);
    CHECK(ok && t.interpolatorCount == 1 && Has(t.glsl, "xe_interp_out[i] = vec4(0.0);") && Has(t.glsl, "xe_interp_out[0].x = "));
    if (!ok)
        std::fprintf(stderr, "  %s\n", error.c_str());
    else
        CHECK(Compiles(t.glsl, false));
    CONTROL(ok && Has(t.glsl, "xe_interp_out[0].y = "));
}

// ---- Vertex formats --------------------------------------------------------------------------
// Host models of the unpacking defined in shader_glsl.cpp (written from those definitions, not
// from the emitted text), checked against values computed by hand from the format definitions.

// The fetch constant's endian swap: 1 swaps the bytes of each 16-bit half, 2 reverses all four
// bytes, 3 exchanges the halves.
uint32_t EndianSwap(uint32_t v, uint32_t e)
{
    if (e == 1 || e == 2)
        v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2 || e == 3)
        v = (v << 16) | (v >> 16);
    return v;
}
float FromBits(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
uint32_t ToBits(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
// IEEE 754 binary16 by its definition (exponents 0-30): (-1)^s * 2^(e - 15) * (1 + m / 1024),
// e = 0: (-1)^s * m * 2^-24.
float HalfIeee(uint32_t h)
{
    const uint32_t e = (h >> 10) & 31, m = h & 1023;
    const double v = e ? std::ldexp(1.0 + m / 1024.0, int(e) - 15) : std::ldexp(double(m), -24);
    return float(h & 0x8000 ? -v : v);
}
// xe_half's integer construction (the emitted helper), exponent 31 excluded.
float HalfBits(uint32_t h)
{
    const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF, s = (h & 0x8000) << 16;
    if (e == 0)
        return FromBits(m == 0 ? s : s | (ToBits(float(m)) - (24u << 23)));
    return FromBits(s | ((e + 112) << 23) | (m << 13));
}
// A packed field: extracted (sign-extended when signed), converted, normalized by the float32
// reciprocal of 2^n - 1 (unsigned) or 2^(n-1) - 1 (signed, then max with -1).
float Field(uint32_t w, uint32_t offset, uint32_t width, bool sgn, bool normalized)
{
    const uint64_t raw = (uint64_t(w) >> offset) & ((uint64_t(1) << width) - 1);
    const int64_t v = sgn && (raw >> (width - 1)) ? int64_t(raw) - (int64_t(1) << width) : int64_t(raw);
    float r = float(v);
    if (!normalized)
        return r;
    const double d = sgn ? std::ldexp(1.0, int(width) - 1) - 1.0 : std::ldexp(1.0, int(width)) - 1.0;
    r = r * float(1.0 / d);
    return sgn ? std::max(r, -1.0f) : r;
}

std::string FetchLine(const VFetch& f)
{
    ShaderTranslation t;
    std::string error;
    if (!Translate(ResolveVs(f, false), false, t, &error))
    {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return "";
    }
    CHECK(Compiles(t.glsl, false));
    return t.glsl;
}

void TestVertexFormats()
{
    // Half floats: the integer construction equals the IEEE definition for every exponent 0-30
    // value (bit-exact, so -0 too), and hand values.
    bool all = true;
    for (uint32_t h = 0; h < 0x10000; h++)
        if (((h >> 10) & 31) != 31)
            all &= ToBits(HalfBits(h)) == ToBits(HalfIeee(h));
    CHECK(all);
    CHECK(HalfBits(0x3C00) == 1.0f && HalfBits(0xC000) == -2.0f && HalfBits(0x0001) == std::ldexp(1.0f, -24));
    CHECK(HalfBits(0x03FF) == 1023.0f * std::ldexp(1.0f, -24) && HalfBits(0x0400) == std::ldexp(1.0f, -14));
    CHECK(HalfBits(0x7BFF) == 65504.0f && HalfBits(0x3555) == 1365.0f / 4096.0f && HalfBits(0x3614) == 0.3798828125f);
    CHECK(ToBits(HalfBits(0x8000)) == 0x80000000u && ToBits(HalfBits(0x0000)) == 0 && HalfBits(0x8001) == -std::ldexp(1.0f, -24));
    // Wrong oracle: a bias of 14 instead of 15.
    CONTROL(FromBits(((0x3C00u >> 10) + 113u) << 23) == 1.0f);

    // Element order and endian: guest bytes C0 00 3C 00 read as a host (little-endian) dword.
    // 8in32 (endian 2) gives 0xC0003C00: x = low half 0x3C00 = 1.0, y = 0xC000 = -2.0. 8in16
    // (endian 1) gives 0x3C00C000, so the halves trade places.
    const uint32_t host = 0x003C00C0u, swapped = EndianSwap(host, 2);
    CHECK(swapped == 0xC0003C00u && HalfBits(swapped & 0xFFFF) == 1.0f && HalfBits(swapped >> 16) == -2.0f);
    CHECK(EndianSwap(host, 1) == 0x3C00C000u && HalfBits(EndianSwap(host, 1) & 0xFFFF) == -2.0f);
    CONTROL(HalfBits(swapped >> 16) == 1.0f);  // wrong half order: x from the high half

    // 16_16_FLOAT (31), FM4's shape: signed / integer flags set, offsets 0 and 1 of a 2-dword
    // element at slot 95.
    VFetch f;
    f.dest = 0, f.slot = 95, f.format = 31, f.stride = 2, f.dswz = DSwz("xy01"), f.sgn = true, f.integer = true;
    std::string g = FetchLine(f);
    CHECK(Has(g, "    uint xe_fetch0_0 = xe_vertex_word(xe_vertex_base[23][3], xe_vidx0, 2u, 0, xe_fetch[47].w);\n"
                 "    precise vec4 xe_fetch0 = vec4(xe_half(xe_fetch0_0 & 0xFFFFu), xe_half((xe_fetch0_0 >> 16) & 0xFFFFu), 0.0, 0.0);\n"
                 "    r0.x = xe_fetch0.x;\n    r0.y = xe_fetch0.y;\n    r0.z = 0.0;\n    r0.w = 1.0;\n"));
    CHECK(Has(g, "float xe_half(uint h)\n{\n    uint e = (h >> 10) & 0x1Fu, m = h & 0x3FFu, s = (h & 0x8000u) << 16;\n"
                 "    if (e == 31u)\n    {\n        atomicOr(xe_error, 2u);\n        return 0.0;\n    }\n"
                 "    if (e == 0u)\n        return uintBitsToFloat(m == 0u ? s : s | (floatBitsToUint(float(m)) - (24u << 23)));\n"
                 "    return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13));\n}\n"));
    CONTROL(Has(g, "vec4(xe_half((xe_fetch0_0 >> 16) & 0xFFFFu), xe_half(xe_fetch0_0 & 0xFFFFu)"));
    // The flags do not apply to float formats: the same text with them cleared.
    f.sgn = false, f.integer = false;
    CHECK(FetchLine(f) == g);
    // 16_16_16_16_FLOAT (32): two dwords, z and w from the second.
    f.slot = 0, f.format = 32, f.stride = 4, f.dswz = DSwz("xyzw");
    g = FetchLine(f);
    CHECK(Has(g, "uint xe_fetch0_1 = xe_vertex_word(xe_vertex_base[0][0], xe_vidx0, 4u, 1, xe_fetch[0].y);"));
    CHECK(Has(g, "vec4(xe_half(xe_fetch0_0 & 0xFFFFu), xe_half((xe_fetch0_0 >> 16) & 0xFFFFu), "
                 "xe_half(xe_fetch0_1 & 0xFFFFu), xe_half((xe_fetch0_1 >> 16) & 0xFFFFu));"));
    // Exponent adjust 3: * 2^3 (0x41000000) after the decode.
    f.format = 31, f.expAdjust = 3, f.dswz = DSwz("xy01");
    CHECK(Has(FetchLine(f), "vec4(xe_half(xe_fetch0_0 & 0xFFFFu) * uintBitsToFloat(0x41000000u), "
                            "xe_half((xe_fetch0_0 >> 16) & 0xFFFFu) * uintBitsToFloat(0x41000000u), 0.0, 0.0);"));

    // 32-bit float formats keep their text without an exponent adjust; with -2 (* 0x3E800000 =
    // 0.25) they read the dwords first.
    f.expAdjust = 0, f.format = 36, f.dswz = DSwz("x001");
    g = FetchLine(f);
    CHECK(Has(g, "vec4 xe_fetch0 = vec4(uintBitsToFloat(xe_vertex_word(xe_vertex_base[0][0], xe_vidx0, 4u, 0, xe_fetch[0].y)), 0.0, 0.0, 0.0);"));
    CHECK(!Has(g, "xe_half"));
    f.format = 37, f.expAdjust = -2, f.dswz = DSwz("xy01");
    CHECK(Has(FetchLine(f), "vec4(uintBitsToFloat(xe_fetch0_0) * uintBitsToFloat(0x3E800000u), "
                            "uintBitsToFloat(xe_fetch0_1) * uintBitsToFloat(0x3E800000u), 0.0, 0.0);"));
    f.expAdjust = 0;

    // Reciprocal constants: 1/255 = 0x3B808081, 1/127 = 0x3C010204, 1/1023 = 0x3A802008, 1/2047
    // = 0x3A001002, 1/511 = 0x3B004020, 1/32767 = 0x38000100, 1/65535 = 0x37800080, 1/3 =
    // 0x3EAAAAAB, 1/(2^31 - 1) -> 2^-31 = 0x30000000, 1/(2^32 - 1) -> 2^-32 = 0x2F800000 (each the
    // float32 nearest the quotient: mantissa of 2^k / d rounded to nearest).
    CHECK(ToBits(float(1.0 / 255)) == 0x3B808081u && ToBits(float(1.0 / 127)) == 0x3C010204u && ToBits(float(1.0 / 3)) == 0x3EAAAAABu);

    // 8_8_8_8 (6) unsigned normalized: x bits 0-7 ... w bits 24-31.
    f.format = 6, f.sgn = false, f.integer = false, f.dswz = DSwz("xyzw");
    g = FetchLine(f);
    CHECK(Has(g, "precise vec4 xe_fetch0 = vec4(float(bitfieldExtract(xe_fetch0_0, 0, 8)) * uintBitsToFloat(0x3B808081u), "
                 "float(bitfieldExtract(xe_fetch0_0, 8, 8)) * uintBitsToFloat(0x3B808081u), "
                 "float(bitfieldExtract(xe_fetch0_0, 16, 8)) * uintBitsToFloat(0x3B808081u), "
                 "float(bitfieldExtract(xe_fetch0_0, 24, 8)) * uintBitsToFloat(0x3B808081u));"));
    CHECK(!Has(g, "xe_half") && !Has(g, "xe_fetch0_1"));
    // 0xFF80_7F00: x = 0, y = 127, z = 128, w = 255 -> 0, ~0.498, ~0.502, 1.0 (within 1 ULP of the
    // quotient; 0 and 255 exact).
    const uint32_t c = 0xFF807F00u;
    CHECK(Field(c, 0, 8, false, true) == 0.0f && Field(c, 24, 8, false, true) == 1.0f);
    CHECK(std::fabs(Field(c, 8, 8, false, true) - 127.0f / 255.0f) <= std::ldexp(1.0f, -24));
    CHECK(std::fabs(Field(c, 16, 8, false, true) - 128.0f / 255.0f) <= std::ldexp(1.0f, -24));
    // The documented 1-ULP difference: 3 * (1/255) is not the correctly rounded 3/255.
    CHECK(Field(3, 0, 8, false, true) != float(3.0 / 255.0) && std::fabs(Field(3, 0, 8, false, true) - float(3.0 / 255.0)) <= std::ldexp(1.0f, -30));
    // Signed normalized (mode 0): 0x7F -> 1, 0x81 (-127) -> -1, 0x80 (-128) -> -1 (clamped), 0 -> 0.
    f.sgn = true;
    CHECK(Has(FetchLine(f), "max(float(bitfieldExtract(int(xe_fetch0_0), 0, 8)) * uintBitsToFloat(0x3C010204u), -1.0), "
                            "max(float(bitfieldExtract(int(xe_fetch0_0), 8, 8)) * uintBitsToFloat(0x3C010204u), -1.0), "
                            "max(float(bitfieldExtract(int(xe_fetch0_0), 16, 8)) * uintBitsToFloat(0x3C010204u), -1.0), "
                            "max(float(bitfieldExtract(int(xe_fetch0_0), 24, 8)) * uintBitsToFloat(0x3C010204u), -1.0));"));
    CHECK(Field(0x7F, 0, 8, true, true) == 1.0f && Field(0x81, 0, 8, true, true) == -1.0f && Field(0x80, 0, 8, true, true) == -1.0f &&
          Field(0, 0, 8, true, true) == 0.0f);
    // Wrong conventions: no clamp (-128/127 < -1); the no-zero mapping (2v + 1) / 255 (0x81 -> -253/255).
    CONTROL(-128.0f * float(1.0 / 127) == -1.0f);
    CONTROL((2.0f * -127.0f + 1.0f) / 255.0f == -1.0f);
    // Integer: the field's value. Signed 0x80 -> -128, unsigned 0xFF -> 255.
    f.integer = true;
    CHECK(Has(FetchLine(f), "vec4(float(bitfieldExtract(int(xe_fetch0_0), 0, 8)), float(bitfieldExtract(int(xe_fetch0_0), 8, 8)), "
                            "float(bitfieldExtract(int(xe_fetch0_0), 16, 8)), float(bitfieldExtract(int(xe_fetch0_0), 24, 8)));"));
    CHECK(Field(0x80, 0, 8, true, false) == -128.0f && Field(0xFF, 0, 8, false, false) == 255.0f);
    // Mode 1 matters only for signed normalized: accepted for integer fields.
    f.rf = true;
    CHECK(!FetchLine(f).empty());
    f.rf = false;

    // 2_10_10_10 (7) signed normalized: x 0-9, y 10-19, z 20-29 (1/511), w 30-31 (1/1); w = 2
    // (-2) and 3 (-1) -> -1, 1 -> 1; x = 0x200 (-512) and 0x201 (-511) -> -1, 0x1FF -> 1.
    f.format = 7, f.integer = false;
    CHECK(Has(FetchLine(f), "vec4(max(float(bitfieldExtract(int(xe_fetch0_0), 0, 10)) * uintBitsToFloat(0x3B004020u), -1.0), "
                            "max(float(bitfieldExtract(int(xe_fetch0_0), 10, 10)) * uintBitsToFloat(0x3B004020u), -1.0), "
                            "max(float(bitfieldExtract(int(xe_fetch0_0), 20, 10)) * uintBitsToFloat(0x3B004020u), -1.0), "
                            "max(float(bitfieldExtract(int(xe_fetch0_0), 30, 2)) * uintBitsToFloat(0x3F800000u), -1.0));"));
    CHECK(Field(2u << 30, 30, 2, true, true) == -1.0f && Field(3u << 30, 30, 2, true, true) == -1.0f && Field(1u << 30, 30, 2, true, true) == 1.0f);
    CHECK(Field(0x200, 0, 10, true, true) == -1.0f && Field(0x201, 0, 10, true, true) == -1.0f && Field(0x1FF, 0, 10, true, true) == 1.0f);
    // Unsigned: x 0x3FF -> 1, w 3 -> 1, w 1 -> 1/3.
    f.sgn = false;
    CHECK(Has(FetchLine(f), "float(bitfieldExtract(xe_fetch0_0, 30, 2)) * uintBitsToFloat(0x3EAAAAABu));"));
    CHECK(Field(0x3FF, 0, 10, false, true) == 1.0f && Field(3u << 30, 30, 2, false, true) == 1.0f &&
          Field(1u << 30, 30, 2, false, true) == float(1.0 / 3));

    // 10_11_11 (16): x 11 bits at 0, y 11 at 11, z 10 at 22; 11_11_10 (17): x 10 at 0, y 11 at
    // 10, z 11 at 21. Unsigned integer, hand word 1 | 2 << 11 | 3 << 22 -> (1, 2, 3).
    f.format = 16, f.integer = true, f.dswz = DSwz("xyz1");
    CHECK(Has(FetchLine(f), "vec4(float(bitfieldExtract(xe_fetch0_0, 0, 11)), float(bitfieldExtract(xe_fetch0_0, 11, 11)), "
                            "float(bitfieldExtract(xe_fetch0_0, 22, 10)), 0.0);"));
    const uint32_t p = 1u | 2u << 11 | 3u << 22;
    CHECK(Field(p, 0, 11, false, false) == 1.0f && Field(p, 11, 11, false, false) == 2.0f && Field(p, 22, 10, false, false) == 3.0f);
    // Wrong layout: widths read off the name from x (10 at 0, 11 at 10, 11 at 21) -> (1, 4, 6).
    CONTROL(Field(p, 10, 11, false, false) == 2.0f || Field(p, 21, 11, false, false) == 3.0f);
    f.format = 17;
    CHECK(Has(FetchLine(f), "vec4(float(bitfieldExtract(xe_fetch0_0, 0, 10)), float(bitfieldExtract(xe_fetch0_0, 10, 11)), "
                            "float(bitfieldExtract(xe_fetch0_0, 21, 11)), 0.0);"));
    // Normalized: 10 bits 1/1023, 11 bits 1/2047.
    f.format = 16, f.integer = false;
    CHECK(Has(FetchLine(f), "vec4(float(bitfieldExtract(xe_fetch0_0, 0, 11)) * uintBitsToFloat(0x3A001002u), "
                            "float(bitfieldExtract(xe_fetch0_0, 11, 11)) * uintBitsToFloat(0x3A001002u), "
                            "float(bitfieldExtract(xe_fetch0_0, 22, 10)) * uintBitsToFloat(0x3A802008u), 0.0);"));

    // 16_16 (25) and 16_16_16_16 (26) signed normalized (1/32767): 0x8001 and 0x8000 -> -1,
    // 0x7FFF -> 1; unsigned 0xFFFF -> 1 (1/65535).
    f.format = 25, f.sgn = true, f.dswz = DSwz("xy01");
    CHECK(Has(FetchLine(f), "vec4(max(float(bitfieldExtract(int(xe_fetch0_0), 0, 16)) * uintBitsToFloat(0x38000100u), -1.0), "
                            "max(float(bitfieldExtract(int(xe_fetch0_0), 16, 16)) * uintBitsToFloat(0x38000100u), -1.0), 0.0, 0.0);"));
    CHECK(Field(0x8001, 0, 16, true, true) == -1.0f && Field(0x8000, 0, 16, true, true) == -1.0f && Field(0x7FFF0000u, 16, 16, true, true) == 1.0f);
    f.format = 26, f.sgn = false, f.dswz = DSwz("xyzw");
    g = FetchLine(f);
    CHECK(Has(g, "vec4(float(bitfieldExtract(xe_fetch0_0, 0, 16)) * uintBitsToFloat(0x37800080u), "
                 "float(bitfieldExtract(xe_fetch0_0, 16, 16)) * uintBitsToFloat(0x37800080u), "
                 "float(bitfieldExtract(xe_fetch0_1, 0, 16)) * uintBitsToFloat(0x37800080u), "
                 "float(bitfieldExtract(xe_fetch0_1, 16, 16)) * uintBitsToFloat(0x37800080u));"));
    CHECK(Has(g, "uint xe_fetch0_1 = xe_vertex_word(xe_vertex_base[0][0], xe_vidx0, 4u, 1, xe_fetch[0].y);"));
    CHECK(Field(0xFFFF, 0, 16, false, true) == 1.0f);

    // 32 (33), 32_32 (34), 32_32_32_32 (35): one dword per component. Unsigned normalized *
    // 2^-32 (0xFFFFFFFF -> 1.0 after rounding to 2^32), signed * 2^-31 (0x80000000 -> -1,
    // 0x7FFFFFFF -> 1), integer as is.
    f.format = 34, f.dswz = DSwz("xy01");
    CHECK(Has(FetchLine(f), "vec4(float(xe_fetch0_0) * uintBitsToFloat(0x2F800000u), float(xe_fetch0_1) * uintBitsToFloat(0x2F800000u), 0.0, 0.0);"));
    CHECK(Field(0xFFFFFFFFu, 0, 32, false, true) == 1.0f && Field(0x80000000u, 0, 32, true, true) == -1.0f &&
          Field(0x7FFFFFFFu, 0, 32, true, true) == 1.0f);
    f.format = 33, f.sgn = true, f.integer = true, f.dswz = DSwz("x001");
    CHECK(Has(FetchLine(f), "precise vec4 xe_fetch0 = vec4(float(int(xe_fetch0_0)), 0.0, 0.0, 0.0);"));
    CHECK(Field(0xFFFFFFFEu, 0, 32, true, false) == -2.0f);
    f.format = 35, f.sgn = true, f.integer = false, f.dswz = DSwz("xyzw");
    g = FetchLine(f);
    CHECK(Has(g, "uint xe_fetch0_3 = xe_vertex_word(xe_vertex_base[0][0], xe_vidx0, 4u, 3, xe_fetch[0].y);"));
    CHECK(Has(g, "max(float(int(xe_fetch0_3)) * uintBitsToFloat(0x30000000u), -1.0));"));
    // A packed format's missing component is refused like a float format's.
    f.format = 25, f.dswz = DSwz("xyz1");
    CHECK(Refuses(ResolveVs(f, false), false, "format lacks"));
}

// FM4 Recomp addition. A VS whose full and mini vertex fetches straddle a predicated exec:
//   cf0 exec(3, 2):  3: vfetch_full r1 <- r0.x slot 0 fmt 38 stride 4
//                    4: PRED_SETEs (no mask) a = r1.x
//   cf1 (p0) exec(5, n): shape 0: 5: vfetch_full r2 <- r0.x slot 1 fmt 38 stride 4
//                        shape 1: 5: that full fetch, 6: vfetch_mini r3 (offset 0)
//                        shape 2: 5: vfetch_mini r3 (offset 0)
//   cf2 exec(6, 1) (shape 0 only): 6: vfetch_mini r3; cf3 alloc position;
//   cf4 exec_end(7, 1): 7: MAXv export62 = r1. Control-flow slots 0-2, instruction slots 3-7.
// A mini fetch reuses the address of the last full fetch that ran ([X] ucode.h
// VertexFetchInstruction, spirv_shader_translator_fetch.cc var_main_vfetch_address_).
std::vector<uint32_t> MiniScopeVs(int shape)
{
    std::vector<uint32_t> p;
    PackCf(p, ExecCf(3, 2, 0x1), PredExecCf(5, shape == 1 ? 2 : 1, true, true, shape == 1 ? 0x5 : 0x1));
    PackCf(p, shape == 0 ? ExecCf(6, 1, 0x1) : OpCf(0), AllocCf(1));
    PackCf(p, ExecCf(7, 1, 0, true), OpCf(0));
    VFetch a;
    a.dest = 1, a.slot = 0, a.format = 38, a.stride = 4, a.dswz = DSwz("xyzw");
    Put(p, a);
    Alu set;
    set.sop = 27, set.s1 = T(0), set.s2 = T(0), set.s3 = T(1, "xxxx");
    Put(p, set);
    VFetch b = a;
    b.dest = 2, b.slot = 1;
    VFetch mini;
    mini.dest = 3, mini.format = 38, mini.mini = true, mini.dswz = DSwz("xyzw");
    Put(p, shape == 2 ? mini : b);
    if (shape == 2)
        Put(p, Alu{});  // slot 6: unused
    else
        Put(p, mini);   // slot 6: the mini fetch
    Alu e;
    e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(1), e.s2 = T(1), e.s3 = T(0);
    Put(p, e);
    return p;
}

void TestMiniFetchScope()
{
    // Shape 0: the mini fetch after the predicated exec would reuse the full fetch inside it,
    // which ran only where p0 held, by naming its block-scoped index: refused (before the fix it
    // was accepted and the GLSL failed to compile).
    ShaderTranslation t;
    std::string error;
    const bool accepted = Translate(MiniScopeVs(0), false, t, &error);
    CHECK(!accepted && Has(error, "mini vertex fetch whose full fetch is in the predicated exec at control-flow instruction 1"));
    if (accepted)
        Compiles(t.glsl, false);  // shows the undeclared index in its log
    // Shape 1: full and mini fetch in the same predicated exec: accepted, the mini fetch uses the
    // inner full fetch's index (fetch 1: slot 1, base [0][1], word 1 xe_fetch[0].w).
    CHECK(Translate(MiniScopeVs(1), false, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    CHECK(Has(t.glsl, "        int xe_vidx1 = int(floor(r0.x));") &&
          Has(t.glsl, "uintBitsToFloat(xe_vertex_word(xe_vertex_base[0][1], xe_vidx1, 4u, 3, xe_fetch[0].w))") &&
          t.vertexFetchSlots == std::vector<uint32_t>({0, 1}));
    CHECK(Compiles(t.glsl, false));
    // Shape 2: a mini fetch inside the predicated exec reusing the full fetch before it: accepted,
    // with fetch 0's index (slot 0) inside the exec's `if`.
    CHECK(Translate(MiniScopeVs(2), false, t, &error));
    if (!error.empty())
        std::fprintf(stderr, "  %s\n", error.c_str());
    const size_t open = t.glsl.find("    if (xe_p0)\n    {\n"), last = t.glsl.rfind("xe_vertex_word(xe_vertex_base[0][0], xe_vidx0, 4u, 3, xe_fetch[0].y)");
    CHECK(open != std::string::npos && last != std::string::npos && open < last && t.vertexFetchSlots == std::vector<uint32_t>{0});
    CHECK(Compiles(t.glsl, false));
    // Control: the shape-1 oracle does not hold for shape 2 (the mini fetch there uses fetch 0).
    CONTROL(Has(t.glsl, "xe_vidx1"));
}

// FM4 Recomp: texture fetches and getTextureGradients in bool-constant control flow (uniform: the
// bool constants come from the uniform block, so implicit derivatives stay defined), refused in
// p0-dependent control flow; a bool jump over a region ending in exec_end (an early end of the
// program on one path); the pixel kill ops KILLEv..KILLNEv (24-27); RECIP_CLAMP (17) and MAXAs (23).
// Expected text from the semantics in shader_glsl.cpp (Xenia ucode.h) and the binding contract:
// bool constant b is bit b % 32 of xe_bool[b / 128][(b / 32) % 4]; a jump's region runs where the
// jump is not taken.
void TestUniformBranches()
{
    Alu set;  // PRED_SETEs, no mask: p0 = r0.x == 0
    set.sop = 27, set.s1 = T(0), set.s2 = T(0), set.s3 = T(0, "xxxx");
    Alu e;  // MAXv export0 = r0
    e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(0), e.s2 = T(0), e.s3 = T(0);
    const std::string sampled = "vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y));\n";
    {
        // cf0 alloc colours, cf1 cexec(2, 1) on bool 3 == 1 holding a fetch r0 = texture 0 at r0.xy
        // (slot 2), cf2 exec_end(3) export0 = r0 (slot 3). The fetch samples inside the bool `if`.
        auto program = [&](bool condition) {
            std::vector<uint32_t> q;
            PackCf(q, AllocCf(2), CondExecCf(2, 1, 3, condition, 0x1));
            PackCf(q, ExecCf(3, 1, 0, true), OpCf(0));
            Put(q, TFetch{});
            Put(q, e);
            return q;
        };
        ShaderTranslation t;
        std::string why;
        CHECK(Translate(program(true), true, t, &why) && t.boolConstants == std::vector<uint32_t>{3} &&
              t.textureSlots == std::vector<uint32_t>{0} && Compiles(t.glsl, true));
        if (!why.empty())
            std::fprintf(stderr, "  %s\n", why.c_str());
        const std::string inBool = "    if (((xe_bool[0].x >> 3) & 1u) == 1u)\n    {\n#ifdef XE_TEXTURE_0_LOD_BIAS\n"
                                   "        vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y), 0.0 + XE_TEXTURE_0_LOD_BIAS);\n#else\n        " +
                                   sampled + "#endif\n";
        CHECK(Has(t.glsl, inBool));
        ShaderTranslation f;
        CHECK(Translate(program(false), true, f));
        CONTROL(Has(f.glsl, inBool));  // the condition is part of what is checked

        // getTextureGradients (fetch opcode 18) in the same cexec: coarse derivatives inside it.
        std::vector<uint32_t> g = program(true);
        g[6] = (g[6] & ~31u) | 18;
        CHECK(Translate(g, true, t) &&
              Has(t.glsl, "        vec4 xe_grad0 = vec4(dFdxCoarse(r0.x), dFdyCoarse(r0.x), dFdxCoarse(r0.y), dFdyCoarse(r0.y));\n") &&
              Compiles(t.glsl, true));
    }
    {
        // cf0 alloc colours, cf1 exec(3) PRED_SETEs (slot 3), cf2 cond_jmp to cf4, cf3 exec(4) or a
        // predicated exec (p0 == 1) of a fetch r1 = texture 0 at r0.xy (slot 4), cf4 exec_end(5)
        // export0 = r1 (slot 5), cf5 nop.
        auto program = [&](const Jmp& j, bool predExec, uint32_t fetchOpcode = 1) {
            std::vector<uint32_t> q;
            PackCf(q, AllocCf(2), ExecCf(3, 1, 0));
            PackCf(q, JmpCf(j), predExec ? PredExecCf(4, 1, true, true, 0x1) : ExecCf(4, 1, 0x1));
            PackCf(q, ExecCf(5, 1, 0, true), OpCf(0));
            Put(q, set);
            TFetch f;
            f.dest = 1;
            Put(q, f);
            q[12] = (q[12] & ~31u) | fetchOpcode;
            Alu x = e;
            x.s1 = T(1), x.s2 = T(1);
            Put(q, x);
            return q;
        };
        Jmp onBool;  // taken where bool 128 == 0: the region runs where it is 1
        onBool.target = 4;
        Jmp onP0 = onBool;
        onP0.predicated = true, onP0.boolIndex = 0;  // taken where p0 == 0
        ShaderTranslation t;
        std::string why;
        // A predicated exec in the bool region: sampled in the bool `if`, outside the p0 `if`, and
        // written under p0.
        CHECK(Translate(program(onBool, true), true, t, &why) && t.boolConstants == std::vector<uint32_t>{128} && Compiles(t.glsl, true));
        if (!why.empty())
            std::fprintf(stderr, "  %s\n", why.c_str());
        CHECK(Has(t.glsl, "    if (((xe_bool[1].x >> 0) & 1u) == 1u)\n    {\n        if (xe_p0)\n        {\n        }\n#ifdef XE_TEXTURE_0_LOD_BIAS\n"));
        CHECK(Has(t.glsl, "#else\n        " + sampled + "#endif\n        if (xe_p0)\n        {\n") && Has(t.glsl, "            r1.x = xe_texel0.x;\n"));
        CONTROL(Has(t.glsl, "            " + sampled));  // not sampled under p0
        // In a predicated jump's region (p0-dependent): refused, a plain exec or a predicated one, and
        // getTextureGradients too.
        CHECK(Refuses(program(onP0, false), true, "texture fetch inside a predicated jump's skipped region"));
        CHECK(Refuses(program(onP0, true), true, "texture fetch inside a predicated exec"));
        CHECK(Refuses(program(onP0, false, 18), true, "getTextureGradients inside a predicated jump's skipped region"));
        CONTROL(Refuses(program(onBool, false), true, "texture fetch inside"));  // the bool region is accepted
    }
    {
        // A terminal region: cf0 cond_jmp to cf3 taken where bool 7 == 1; cf1 alloc colours; cf2
        // exec_end(3, 2): KILLGTv (no mask) c0 > c1 (slot 3), MAXv export0 = c2 (slot 4); cf3 alloc
        // colours; cf4 exec_end(5) export0 = r0 (slot 5); cf5 nop. Where bool 7 == 0 the program
        // kills (if any c0 > c1), exports c2 and ends; elsewhere it exports r0. Both reach the one
        // epilogue: the discard of a killed pixel, then the alpha test.
        Jmp j;
        j.target = 3, j.boolIndex = 7, j.condition = true;
        Alu kill;
        kill.vop = 25, kill.s1 = C(0), kill.s2 = C(1), kill.s3 = T(0);
        Alu early = e;
        early.s1 = C(2), early.s2 = C(2);
        auto program = [&](Cf region, Alu first) {
            std::vector<uint32_t> q;
            PackCf(q, JmpCf(j), AllocCf(2));
            PackCf(q, region, AllocCf(2));
            PackCf(q, ExecCf(5, 1, 0, true), OpCf(0));
            Put(q, first);
            Put(q, early);
            Put(q, e);
            return q;
        };
        ShaderTranslation t;
        std::string why;
        CHECK(Translate(program(ExecCf(3, 2, 0, true), kill), true, t, &why) && t.boolConstants == std::vector<uint32_t>{7} &&
              t.interpolatorCount == 1 && Compiles(t.glsl, true));
        if (!why.empty())
            std::fprintf(stderr, "  %s\n", why.c_str());
        CHECK(Has(t.glsl, "    bool xe_kill = false;\n"));
        CHECK(Has(t.glsl, "    if (((xe_bool[0].x >> 7) & 1u) == 0u)\n    {\n"
                          "        bool xe_killed0 = any(notEqual(xe_sgt(xe_c[0].xyzw, xe_c[1].xyzw), vec4(0.0)));\n"
                          "        xe_kill = xe_kill || xe_killed0;\n"
                          "        precise vec4 xe_v1 = xe_c[2].xyzw;\n        xe_color0.x = xe_v1.x;\n"));
        CHECK(Has(t.glsl, "        xe_color0.w = xe_v1.w;\n    }\n    else\n    {\n        precise vec4 xe_v2 = r0.xyzw;\n"));
        CHECK(Has(t.glsl, "        xe_color0.w = xe_v2.w;\n    }\n    if (xe_kill)\n        discard;\n#ifdef XE_ALPHA_TEST\n"));
        CONTROL(Has(t.glsl, "    if (((xe_bool[0].x >> 7) & 1u) == 1u)\n"));  // the region runs where the jump is not taken
        // Refusals: the region (its exec_end holding only the kill) exports nothing; a vertex shader;
        // a predicated jump over the exec_end; an export in a cexec inside the region.
        CHECK(Refuses(program(ExecCf(3, 1, 0, true), kill), true, "pixel shader does not write all of colour 0"));
        CHECK(Refuses(program(ExecCf(3, 2, 0, true), kill), false, "jump over an exec_end in a vertex shader"));
        {
            // cf0 exec(3) PRED_SETEs; cf1 cond_jmp to cf4 on p0; cf2 alloc; cf3 exec_end(4) export0 = c2;
            // cf4 alloc; cf5 exec_end(5) export0 = r0.
            std::vector<uint32_t> q;
            Jmp p;
            p.target = 4, p.predicated = true, p.boolIndex = 0, p.condition = true;
            PackCf(q, ExecCf(3, 1, 0), JmpCf(p));
            PackCf(q, AllocCf(2), ExecCf(4, 1, 0, true));
            PackCf(q, AllocCf(2), ExecCf(5, 1, 0, true));
            Put(q, set);
            Put(q, early);
            Put(q, e);
            CHECK(Refuses(q, true, "predicated jump over an exec_end"));
        }
        {
            // cf0 cond_jmp to cf4 (bool 7); cf1 alloc; cf2 cexec(3, 2) on bool 1 (kill, export c2);
            // cf3 exec_end(5) export0 = r0; cf4 alloc; cf5 exec_end(5).
            std::vector<uint32_t> q;
            Jmp k = j;
            k.target = 4;
            PackCf(q, JmpCf(k), AllocCf(2));
            PackCf(q, CondExecCf(3, 2, 1, true), ExecCf(5, 1, 0, true));
            PackCf(q, AllocCf(2), ExecCf(5, 1, 0, true));
            Put(q, kill);
            Put(q, early);
            Put(q, e);
            CHECK(Refuses(q, true, "export inside a conditional exec"));
        }
    }
    {
        // The kill ops on r0 against c0, exported (the result: 1.0 where killed, else 0.0): 24 ==,
        // 25 >, 26 >= (xe_seq / xe_sgt / xe_sge, ordered) and 27 != (xe_sne, true for NaN).
        const char* kNames[] = {"xe_seq", "xe_sgt", "xe_sge", "xe_sne"};
        for (uint32_t op = 24; op <= 27; op++)
        {
            Alu k = e;
            k.vop = op, k.s2 = C(0);
            ShaderTranslation t;
            CHECK(Translate(ColourPs(k), true, t) &&
                  Has(t.glsl, std::string("    bool xe_killed0 = any(notEqual(") + kNames[op - 24] + "(r0.xyzw, xe_c[0].xyzw), vec4(0.0)));\n") &&
                  Has(t.glsl, "    precise vec4 xe_v0 = vec4(xe_killed0 ? 1.0 : 0.0);\n") &&
                  Has(t.glsl, "    if (xe_kill)\n        discard;\n") && Compiles(t.glsl, true));
            if (op == 25)
                CONTROL(Has(t.glsl, "xe_sge(r0.xyzw"));
            CHECK(Refuses(ColourPs(k), false, "pixel kill in a vertex shader"));
        }
        ShaderTranslation t;
        CHECK(Translate(ColourPs(), true, t));
        CONTROL(Has(t.glsl, "xe_kill"));  // declared and tested only when a kill op is used
    }
    {
        // RECIP_CLAMP (17): r0.w (src3 xyzw: a = w) through xe_rcpc, which clamps xe_rcp's +-Inf to
        // +-FLT_MAX (0x7F7FFFFF: exponent 254, mantissa all ones; sign bit for the negative).
        Alu r = e;
        r.vmask = 0x7, r.smask = 0x8, r.sop = 17;
        ShaderTranslation t;
        CHECK(Translate(ColourPs(r), true, t) && Has(t.glsl, "    precise float xe_s0 = xe_rcpc(r0.w);\n") &&
              Has(t.glsl, "float xe_rcpc(float x)\n{\n    float r = xe_rcp(x);\n    if (isinf(r))\n"
                          "        return uintBitsToFloat(r < 0.0 ? 0xFF7FFFFFu : 0x7F7FFFFFu);\n    return r;\n}\n") &&
              t.glsl.find("float xe_rcp(float x)") < t.glsl.find("float xe_rcpc(float x)") && Compiles(t.glsl, true));
        CONTROL(Has(t.glsl, "0x7F800000u : 0x7F7FFFFFu"));
        // MAXAs (23): MAXs of a = r0.w, b = r0.x; its a0 write has no reader (a0-relative constants
        // stay refused).
        r.sop = 23;
        CHECK(Translate(ColourPs(r), true, t) && Has(t.glsl, "    precise float xe_s0 = (r0.w >= r0.x ? r0.w : r0.x);\n") &&
              Compiles(t.glsl, true));
        CONTROL(Has(t.glsl, "(r0.x >= r0.w"));
        Alu rel = r;
        rel.s1 = C(4), rel.c0rel = true;
        CHECK(Refuses(ColourPs(rel), true, "relatively addressed constant"));
    }
}

void TestRefusals()
{
    // Vector opcode 30 (undefined), scalar PRED_SET_INVs (31); CUBEv (18) translates.
    Alu a;
    a.exp = true, a.vmask = 0xF, a.s1 = T(0), a.s2 = T(0), a.s3 = T(0);
    a.vop = 30;
    CHECK(Refuses(ColourPs(a), true, "vector ALU opcode 30"));
    // CUBEv (18): Xenia's kCube on src1 read as .z_xy: major axis Z -> (-y, z < 0 ? -x : x, 2z, 5/4),
    // Y -> (y < 0 ? -z : z, x, 2y, 3/2), X -> (-y, x < 0 ? z : -z, 2x, 1/0).
    a.vop = 18;
    {
        ShaderTranslation ct;
        CHECK(Translate(ColourPs(a), true, ct) && Has(ct.glsl, "xe_cube(") && Has(ct.glsl, "    float x = s.z, y = s.w, z = s.x;\n") &&
              Has(ct.glsl, "return vec4(-y, z < 0.0 ? -x : x, 2.0 * z, z < 0.0 ? 5.0 : 4.0);") &&
              Has(ct.glsl, "return vec4(y < 0.0 ? -z : z, x, 2.0 * y, y < 0.0 ? 3.0 : 2.0);") &&
              Has(ct.glsl, "return vec4(-y, x < 0.0 ? z : -z, 2.0 * x, x < 0.0 ? 1.0 : 0.0);") && Compiles(ct.glsl, true));
        CONTROL(Has(ct.glsl, "float x = s.x, y = s.y, z = s.z;"));
    }
    a.vop = 2, a.sop = 31;
    CHECK(Refuses(ColourPs(a), true, "scalar ALU opcode 31"));
    // Neighbours of the supported ops: MAX4v (19), DSTv (28), the predicate setters SETPEQv (20)
    // and the other scalar predicate ops (31-34; 27-30 are translated), LOG_CLAMP (15), RECIP_FF (18; RECIP_CLAMP
    // (17) is translated, FM4 Recomp: TestUniformBranches), and
    // MUL_PREVs's sibling MUL_PREV2s (4) (ADD_PREVs and SUB_PREVs are translated: PrevPs above).
    for (uint32_t op : {19u, 28u, 20u})
    {
        Alu v = a;
        v.vop = op, v.sop = 50;
        CHECK(Refuses(ColourPs(v), true, ("vector ALU opcode " + std::to_string(op)).c_str()));
    }
    for (uint32_t op : {4u, 15u, 18u, 31u, 32u, 33u, 34u})
    {
        Alu v = a;
        v.vmask = 0x7, v.smask = 0x8, v.sop = op;
        CHECK(Refuses(ColourPs(v), true, ("scalar ALU opcode " + std::to_string(op)).c_str()));
    }
    {
        // FRACs (11), TRUNCs (12), FLOORs (13) on the scalar operand (Xenia ucode.h): a - floor(a), trunc(a), floor(a).
        for (uint32_t op : {11u, 12u, 13u})
        {
            Alu v = a;
            v.vmask = 0x7, v.smask = 0x8, v.sop = op;
            ShaderTranslation st;
            const char* want = op == 11 ? " - floor(" : op == 12 ? "trunc(" : "floor(";
            CHECK(Translate(ColourPs(v), true, st) && Has(st.glsl, want) && Compiles(st.glsl, true));
        }
        Alu v = a;
        v.vmask = 0x7, v.smask = 0x8, v.sop = 11;
        ShaderTranslation st;
        CHECK(Translate(ColourPs(v), true, st));
        CHECK(Has(st.glsl, "precise float xe_s0 = (") && Has(st.glsl, " - floor("));  // the line the control reads
        CONTROL(Has(st.glsl, "precise float xe_s0 = trunc("));  // FRACs is not TRUNCs
    }
    // MUL_PREV2s (4) stays refused (SUB_CONST_0/1, SIN and COS are translated: PrevPs above).
    a.sop = 4;
    CHECK(Refuses(ColourPs(a), true, "scalar ALU opcode 4"));
    // RETAIN_PREV that writes, with no scalar op before it: ps is unset.
    a.sop = 50, a.vmask = 0x7, a.smask = 0x8;
    CHECK(Refuses(ColourPs(a), true, "RETAIN_PREV writes the previous scalar result before any scalar op sets it"));
    a.vmask = 0xF, a.smask = 0;
    // Predication, relative constants and temps, relative destination.
    Alu p = a;
    p.pred = true;
    CHECK(Refuses(ColourPs(p), true, "predicated ALU instruction reads the predicate before any predicate-setting op sets it"));
    p = a, p.s1 = C(4), p.c0rel = true;
    CHECK(Refuses(ColourPs(p), true, "relatively addressed constant"));
    p = a, p.s1 = T(0x40), p.s2 = T(0x40);
    CHECK(Refuses(ColourPs(p), true, "relatively addressed temp"));
    // Unsupported export targets: PS colour 1, PS depth (61), VS point size (63), memory (32).
    p = a, p.vdest = 1;
    CHECK(Refuses(ColourPs(p), true, "export to register 1"));
    p.vdest = 61;
    CHECK(Refuses(ColourPs(p), true, "export to register 61"));
    p.vdest = 32;
    CHECK(Refuses(ColourPs(p), true, "export to register 32"));
    {
        std::vector<uint32_t> v = ResolveVs();
        v[9] = (v[9] & ~0x3Fu) | 63;  // the export ALU (slot 3) word0 destination
        CHECK(Refuses(v, false, "export to register 63"));
    }
    // Partial colour 0.
    p = a, p.vmask = 0x7;
    CHECK(Refuses(ColourPs(p), true, "all of colour 0"));

    // Control flow: call, the other conditional execs (3, 4, 6, 14), a predicated exec (5) with
    // p0 never set, a missing exec_end, memory alloc (jumps: TestPreviousScalarAndJump).
    for (uint32_t op : {9u, 3u, 4u, 5u, 6u, 14u})
    {
        std::vector<uint32_t> p2 = ColourPs();
        std::vector<uint32_t> q;
        PackCf(q, OpCf(op, 1), ExecCf(1, 1, 0, true));
        p2[0] = q[0], p2[1] = q[1], p2[2] = q[2];
        CHECK(Refuses(p2, true, op == 5 ? "predicated exec reads the predicate before any predicate-setting op sets it" : "is not supported"));
    }
    {
        std::vector<uint32_t> q;
        PackCf(q, AllocCf(2), ExecCf(1, 1, 0, false));
        Put(q, Alu{});
        CHECK(Refuses(q, true, "without exec_end"));
        q.clear();
        PackCf(q, AllocCf(3), ExecCf(1, 1, 0, true));
        Put(q, Alu{});
        CHECK(Refuses(q, true, "memory export"));
        // The draw handler's skip check (ucode.h AllocatesMemoryExport): a memory alloc is found
        // in either control-flow half; an interpolator alloc is not one; nothing past the code start.
        CHECK(AllocatesMemoryExport(q.data(), uint32_t(q.size())));
        std::vector<uint32_t> r;
        PackCf(r, ExecCf(1, 1, 0, false), AllocCf(3));
        Put(r, Alu{});
        CHECK(AllocatesMemoryExport(r.data(), uint32_t(r.size())));
        std::vector<uint32_t> plain;
        PackCf(plain, AllocCf(2), ExecCf(1, 1, 0, true));
        Put(plain, Alu{});
        CHECK(!AllocatesMemoryExport(plain.data(), uint32_t(plain.size())));  // negative control: interpolators
    }

    // Vertex fetch: formats [X] does not name (0, 8, 63), signed normalized repeating-fraction
    // mode 1, predicated, a component the format lacks (z of 32_32), swizzle 6, mini with no full
    // fetch, a fetch in a pixel shader.
    VFetch f;
    f.dest = 0, f.slot = 0, f.format = 8, f.stride = 2, f.dswz = DSwz("xyzw");
    CHECK(Refuses(ResolveVs(f, false), false, "vertex format 8 is not supported"));
    f.format = 0;
    CHECK(Refuses(ResolveVs(f, false), false, "vertex format 0 is not supported"));
    f.format = 63;
    CHECK(Refuses(ResolveVs(f, false), false, "vertex format 63 is not supported"));
    f.format = 6, f.rf = true;
    CHECK(Refuses(ResolveVs(f, false), false, "repeating-fraction mode 1"));
    f.format = 37, f.dswz = DSwz("xy11"), f.rf = false, f.pred = true;
    CHECK(Refuses(ResolveVs(f, false), false, "predicated vertex fetch"));
    f.pred = false, f.dswz = DSwz("xyz1");
    CHECK(Refuses(ResolveVs(f, false), false, "format lacks"));
    f.dswz = DSwz("xy?1");
    CHECK(Refuses(ResolveVs(f, false), false, "swizzle 6"));
    f.dswz = DSwz("xy11"), f.mini = true;
    CHECK(Refuses(ResolveVs(f, false), false, "no preceding full fetch"));
    CHECK(Refuses(ResolveVs(), true, "vertex fetch in a pixel shader"));
    // Reading an unwritten temp in a VS (index from r2.x).
    f.mini = false, f.src = 2;
    CHECK(Refuses(ResolveVs(f, false), false, "reads r2.x before writing it"));

    // Texture fetch: 1D and cube, unnormalized, filter override, register LOD, LOD bias, a Z
    // offset on a 2D fetch, predicated; a texture fetch in a vertex shader samples LOD 0.
    TFetch t;
    t.dim = 0;
    CHECK(Refuses(TexturedPs(t), true, "dimension 0 is not supported"));
    {
        // Cube (3): samplerCube at the direction of (sc, tc, face) = (r0.x, r0.y, r0.z) (Xenia fetch.cc kCube).
        TFetch cubeFetch = t;
        cubeFetch.dim = 3, cubeFetch.srcSwz = 0x24;  // x, y, z
        ShaderTranslation ct;
        CHECK(Translate(TexturedPs(cubeFetch), true, ct) && Has(ct.glsl, "uniform samplerCube xe_texture_0;") &&
              Has(ct.glsl, "texture(xe_texture_0, xe_cube_dir(vec3(r0.x, r0.y, r0.z)))") &&
              Has(ct.glsl, "    float sc = c.x * 2.0 - 3.0, tc = c.y * 2.0 - 3.0;\n") &&
              Has(ct.glsl, "        return vec3(sgn, -tc, neg ? sc : -sc);\n") && Has(ct.glsl, "        return vec3(sc, sgn, neg ? -tc : tc);\n") &&
              Has(ct.glsl, "    return vec3(neg ? -sc : sc, -tc, sgn);\n") && Compiles(ct.glsl, true));
        CONTROL(Has(ct.glsl, "texture(xe_texture_0, vec3(r0.x, r0.y, r0.z))"));
        cubeFetch.ox = 1;
        CHECK(Refuses(TexturedPs(cubeFetch), true, "offsets on a cube fetch"));
    }
    t = {}, t.unnorm = true;
    CHECK(Refuses(TexturedPs(t), true, "unnormalized"));
    t = {}, t.mag = 2;
    CHECK(Refuses(TexturedPs(t), true, "filter 2 (base map)"));
    t = {}, t.aniso = 4;
    CHECK(Refuses(TexturedPs(t), true, "anisotropic filter override 4"));
    t = {}, t.arb = 4;
    CHECK(Refuses(TexturedPs(t), true, "arbitrary filter"));
    t = {}, t.regLod = true;
    CHECK(Refuses(TexturedPs(t), true, "register LOD before any setTextureLod"));
    t = {}, t.regGrad = true;
    CHECK(Refuses(TexturedPs(t), true, "register gradients"));
    {
        // setTextureLod r0.z, then the fetch with the register LOD: a bias of the computed LOD, or
        // (computed LOD off) the explicit LOD (Xenia fetch.cc: lod = register LOD + biases).
        ShaderTranslation lt;
        std::string err;
        CHECK(Translate(TexturedLodPs(true), true, lt, &err) && Has(lt.glsl, "    float xe_lod = 0.0;\n") && Has(lt.glsl, "    xe_lod = r0.z;\n") &&
              Has(lt.glsl, "texture(xe_texture_0, vec2(r0.x, r0.y), xe_lod);") && Compiles(lt.glsl, true));
        if (!err.empty())
            std::fprintf(stderr, "  %s\n", err.c_str());
        CHECK(Translate(TexturedLodPs(false), true, lt) && Has(lt.glsl, "textureLod(xe_texture_0, vec2(r0.x, r0.y), xe_lod);") &&
              Compiles(lt.glsl, true));
        CONTROL(Has(lt.glsl, "textureLod(xe_texture_0, vec2(r0.x, r0.y), 0.0);"));
    }
    t = {}, t.bias = 4;
    CHECK(Refuses(TexturedPs(t), true, "LOD bias"));
    t = {}, t.oz = 1;
    CHECK(Refuses(TexturedPs(t), true, "Z offset"));
    t = {}, t.pred = true;
    CHECK(Refuses(TexturedPs(t), true, "predicated texture fetch"));
    {
        // The same program as a vertex shader (export 0 is interpolator 0 there): its fetch samples
        // LOD 0, the sampler is declared, and it compiles (a VS has no derivatives).
        ShaderTranslation v;
        std::string error;
        // VS: exec{tfetch r1 <- r0.xx}; alloc position; exec_end{MAXv export62 = r1}.
        TFetch xx;
        xx.srcSwz = 0, xx.dest = 1;  // (r0.x, r0.x): a VS preloads only r0.x
        std::vector<uint32_t> vs;
        PackCf(vs, ExecCf(2, 1, 0x1), AllocCf(1));
        PackCf(vs, ExecCf(3, 1, 0, true), Cf{0, 0});
        Put(vs, xx);
        Alu e;
        e.exp = true, e.vdest = 62, e.vmask = 0xF, e.s1 = T(1), e.s2 = T(1), e.s3 = T(0);
        Put(vs, e);
        const bool ok = Translate(vs, false, v, &error);
        CHECK(ok && Has(v.glsl, "vec4 xe_texel0 = textureLod(xe_texture_0, vec2(r0.x, r0.x), 0.0);") &&
              Has(v.glsl, "layout(set = 0, binding = 8) uniform sampler2D xe_texture_0;") && v.textureSlots == std::vector<uint32_t>{0});
        if (!ok)
            std::fprintf(stderr, "  %s\n", error.c_str());
        else
            CHECK(Compiles(v.glsl, false));
        CONTROL(ok && Has(v.glsl, "= texture(xe_texture_0, vec2(r0.x, r0.x));"));  // implicit LOD would be wrong in a VS
    }
    // Fetch opcode 17 (getCompTexLOD).
    std::vector<uint32_t> lod = TexturedPs();
    lod[6] = (lod[6] & ~31u) | 17;
    CHECK(Refuses(lod, true, "fetch opcode 17"));
    {
        // getTextureGradients (18) on r0.xy: (ddx(x), ddy(x), ddx(y), ddy(y)) into r0, no sampler.
        std::vector<uint32_t> grad = TexturedPs();
        grad[6] = (grad[6] & ~31u) | 18;
        ShaderTranslation g;
        std::string error;
        const bool ok = Translate(grad, true, g, &error);
        CHECK(ok && Has(g.glsl, "vec4 xe_grad0 = vec4(dFdxCoarse(r0.x), dFdyCoarse(r0.x), dFdxCoarse(r0.y), dFdyCoarse(r0.y));") &&
              g.textureSlots.empty() && !Has(g.glsl, "sampler2D"));
        if (!ok)
            std::fprintf(stderr, "  %s\n", error.c_str());
        else
            CHECK(Compiles(g.glsl, true));
        CONTROL(ok && Has(g.glsl, "vec4(dFdxCoarse(r0.x), dFdxCoarse(r0.y), dFdyCoarse(r0.x), dFdyCoarse(r0.y))"));  // the other order
        CHECK(Refuses(grad, false, "getTextureGradients in a vertex shader"));
    }

    // Conditional exec (opcode 13): an export or a vertex fetch inside it; more than 12 distinct bool
    // constants (FM4 Recomp: was 6). A texture fetch inside it: TestUniformBranches.
    {
        std::vector<uint32_t> q;
        ShaderTranslation t;
        PackCf(q, AllocCf(2), CondExecCf(2, 1, 0, true));
        PackCf(q, ExecCf(3, 1, 0, true), OpCf(0));
        Alu e;
        e.exp = true, e.vdest = 0, e.vmask = 0xF, e.s1 = T(0), e.s2 = T(0), e.s3 = T(0);
        Put(q, e);
        Put(q, e);
        CHECK(Refuses(q, true, "export inside a conditional exec"));
        q.clear();
        PackCf(q, AllocCf(2), CondExecCf(2, 1, 0, true, 0x1));
        PackCf(q, ExecCf(3, 1, 0, true), OpCf(0));
        Put(q, TFetch{});
        Put(q, e);
        CHECK(Translate(q, true, t) && Has(t.glsl, "        vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y));\n"));
        {
            std::vector<uint32_t> r;
            // A texture fetch inside a predicated exec (FM4's car pixel shaders): p0 = r0.x == 0
            // (PRED_SETEs, slot 2); (p0) exec of a fetch r1 = texture 0 at r0.xy (slot 3); exec_end
            // exporting r1 (slot 4). The fetch is sampled outside the exec's `if` (uniform control flow,
            // defined derivatives) and its result written inside it.
            PackCf(r, AllocCf(2), ExecCf(2, 1, 0));
            PackCf(r, PredExecCf(3, 1, true, true, 0x1), ExecCf(4, 1, 0, true));
            Alu set;
            set.sop = 27, set.s1 = T(0), set.s2 = T(0), set.s3 = T(0, "xxxx");
            Put(r, set);
            TFetch f;
            f.dest = 1;
            Put(r, f);
            Alu x;
            x.exp = true, x.vdest = 0, x.vmask = 0xF, x.s1 = T(1), x.s2 = T(1), x.s3 = T(0), x.vop = 10;
            Put(r, x);
            ShaderTranslation pt;
            std::string why;
            CHECK(Translate(r, true, pt, &why));
            if (!why.empty())
                std::fprintf(stderr, "  %s\n", why.c_str());
            const std::string& g = pt.glsl;
            const size_t open = g.find("    if (xe_p0)\n    {\n"), sample = g.find("    vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y));\n"),
                         write = g.find("        r1.x = xe_texel0.x;\n");
            CHECK(open != std::string::npos && sample != std::string::npos && write != std::string::npos && open < sample && sample < write &&
                  g.find("    if (xe_p0)\n    {\n", sample) < write && Compiles(g, true));
            CONTROL(Has(g, "        vec4 xe_texel0 = texture("));  // sampled inside the predicated block
            // The same fetch predicated on the exec's condition (word 1 bit 31, word 2 bit 31 = 1) is
            // accepted; predicated on the opposite condition it is refused.
            r[3 * 3 + 1] |= 1u << 31, r[3 * 3 + 2] |= 1u << 31;
            CHECK(Translate(r, true, pt) && Has(pt.glsl, "    vec4 xe_texel0 = texture(xe_texture_0, vec2(r0.x, r0.y));\n"));
            r[3 * 3 + 2] &= ~(1u << 31);
            CHECK(Refuses(r, true, "predicated texture fetch"));
        }

        std::vector<uint32_t> v = ResolveVs();
        v[0] = CondExecCf(2, 1, 0, true, 0x1).w0, v[1] = (v[1] & 0xFFFF0000u) | CondExecCf(2, 1, 0, true).w1;
        CHECK(Refuses(v, false, "vertex fetch inside a conditional exec"));
        // A vertex fetch inside a predicated exec (FM4's car vertex shaders): p0 = r0.x == 0 (slot 2);
        // (p0) exec of a fetch r1 = slot 0 at index r0.x, stride 4 (slot 3); exec_end exporting r1 to
        // the position (slot 4). The fetch runs under the exec's test; predicated on the opposite
        // condition it is refused.
        {
            std::vector<uint32_t> r;
            PackCf(r, AllocCf(1), ExecCf(2, 1, 0));
            PackCf(r, PredExecCf(3, 1, true, true, 0x1), ExecCf(4, 1, 0, true));
            Alu set;  // r1 = max(r0, r0) (defined on both paths) + PRED_SETEs
            set.vop = 2, set.vdest = 1, set.vmask = 0xF, set.sop = 27, set.s1 = T(0, "xxxx"), set.s2 = T(0, "xxxx"), set.s3 = T(0, "xxxx");
            Put(r, set);
            VFetch f;
            f.dest = 1, f.stride = 4, f.format = 38, f.dswz = 0x688;
            Put(r, f);
            Alu x;
            x.exp = true, x.vdest = 62, x.vmask = 0xF, x.s1 = T(1), x.s2 = T(1), x.s3 = T(0), x.vop = 10;
            Put(r, x);
            ShaderTranslation vt;
            std::string why;
            CHECK(Translate(r, false, vt, &why));
            if (!why.empty())
                std::fprintf(stderr, "  %s\n", why.c_str());
            const size_t open = vt.glsl.find("    if (xe_p0)\n    {\n"), fetch = vt.glsl.find("        int xe_vidx");
            CHECK(open != std::string::npos && fetch != std::string::npos && open < fetch && Compiles(vt.glsl, false));
            r[3 * 3 + 1] |= 1u << 31, r[3 * 3 + 2] |= 1u << 31;
            CHECK(Translate(r, false, vt));
            r[3 * 3 + 2] &= ~(1u << 31);
            CONTROL(Translate(r, false, vt));
        }
        for (uint32_t last : {12u, 0u})  // bools 0-12, or 0-11 with bool 0 tested twice
        {
            q.clear();
            PackCf(q, AllocCf(2), CondExecCf(8, 1, 0, true));
            for (uint32_t b = 1; b < 13; b += 2)
                PackCf(q, CondExecCf(8, 1, b, true), CondExecCf(8, 1, b + 1 == 12 ? last : b + 1, true));
            PackCf(q, ExecCf(9, 1, 0, true), OpCf(0));
            Put(q, Alu{});
            Put(q, e);
            if (last)
                CHECK(Refuses(q, true, "more than 12 distinct bool constants"));
            else
                CHECK(Translate(q, true, t) && t.boolConstants.size() == 12);
        }
    }

    // Bad sizes.
    ShaderTranslation out;
    std::string error;
    const uint32_t two[2] = {0, 0};
    CHECK(!TranslateShader(two, 2, false, out, error) && !error.empty());

    // Wrong-oracle control: a supported program is not refused.
    CONTROL(Refuses(ColourPs(), true, ""));
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: test_shader_glsl <scratch directory>\n");
        return 2;
    }
    tempDir = argv[1];
    if (std::system(("mkdir -p " + tempDir).c_str()) != 0)
    {
        std::fprintf(stderr, "cannot create %s\n", tempDir.c_str());
        return 1;
    }
    TestTextureFetchDecode();
    TestClearVs();
    TestColourPs();
    TestQuadsVs();
    TestTexturedPs();
    TestResolveAndPointsVs();
    TestVideoPs();
    TestPwlGamma();
    TestBranchesVs();
    TestCompareSelect();
    TestPartialInterpolator();
    TestFetchFiltersOffsets();
    TestTexture3DFetch();
    TestColor7e3Clamp();
    TestLoops();
    TestPreviousScalarAndJump();
    TestPredicate();
    TestVertexFormats();
    TestMiniFetchScope();
    TestUniformBranches();
    TestRefusals();
    std::printf("test_shader_glsl: %d passed, %d failed (%d negative controls, %d shaders compiled and validated)\n",
                passed, failed, controls, compiled);
    return failed ? 1 : 0;
}
