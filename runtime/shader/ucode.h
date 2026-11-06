// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/gpu/ucode.h (GPL-3.0-or-later, same owner), reviewed
// and re-tested here (tests/shader/run_shader_tests.sh). FM4 Recomp changes: namespace gpu::ucode
// -> runtime::shader::ucode; ForzaRecomp's task tag and test path dropped from this comment.
// Every field was re-checked against Xenia ucode.h at 67f16c4e315f (the instruction structs'
// bit fields): no difference.
//
// Xenos shader microcode fields: control flow, ALU, vertex and texture fetch instructions,
// decoded field by field with no semantics. Pure and header-only, so tests/shader checks it alone.
//
// Sources and labels (every field below is REFERENCE_DERIVED unless marked otherwise):
//   [X] Xenia ucode.h (xenia-canary 67f16c4e315f, BSD-3-Clause: see NOTICE),
//       whose layout Xenia checked against the XNA Game Studio 3.1 Xbox 360 shader assembler;
//       the layout is re-written here, not copied.
//   [F] freedreno instr-a2xx.h (Mesa, MIT; Adreno 2xx, the Xenos's sibling), used only as a
//       cross-reference. Where [F] agrees with [X] the field is marked [X][F]; where only [X]
//       defines it, [X]; divergences are named at the field. Agreement between two software
//       sources is not a hardware oracle: nothing here is VERIFIED.
//
// Words are host-order 32-bit values as the GPU reads them (the caller byte-swaps guest
// memory). A shader is a sequence of 3-word slots; the control-flow program occupies the
// first slots, two 48-bit control-flow instructions per slot, and exec instructions address
// ALU/fetch instructions by slot index.
#pragma once
#include <cstdint>

namespace runtime::shader::ucode {

constexpr uint32_t Bits(uint32_t value, uint32_t low, uint32_t count)
{
    return (value >> low) & ((count >= 32) ? ~0u : ((1u << count) - 1));
}

constexpr int32_t SignedBits(uint32_t value, uint32_t low, uint32_t count)
{
    const uint32_t field = Bits(value, low, count), sign = 1u << (count - 1);
    return int32_t(field ^ sign) - int32_t(sign);
}

// ---- Control flow ----------------------------------------------------------------------------

// All 16 values of the 4-bit opcode are named by both [X] and [F].
enum class CfOpcode : uint32_t
{
    Nop = 0,
    Exec = 1,
    ExecEnd = 2,
    CondExec = 3,            // on a bool constant
    CondExecEnd = 4,
    CondExecPred = 5,        // on the predicate
    CondExecPredEnd = 6,
    LoopStart = 7,
    LoopEnd = 8,
    CondCall = 9,
    Return = 10,
    CondJmp = 11,
    Alloc = 12,
    CondExecPredClean = 13,  // [X]: a bool-constant cexec (layout of CondExec); [F] names it only
    CondExecPredCleanEnd = 14,
    MarkVsFetchDone = 15,
};

// One control-flow instruction: word0 is 32 bits, word1 the upper 16 bits (bits 0-15 used).
struct CfWords
{
    uint32_t word0, word1;
};

// Slot `pair` holds instructions 2*pair (bits 0-47) and 2*pair+1 (bits 48-95) [X][F].
constexpr CfWords UnpackCf(const uint32_t slot[3], uint32_t half)
{
    return half == 0 ? CfWords{slot[0], slot[1] & 0xFFFF} : CfWords{(slot[1] >> 16) | (slot[2] << 16), slot[2] >> 16};
}

// The `index`th control-flow instruction of a shader `wordCount` words long; false when it lies
// past the end (callers stop).
constexpr bool ReadCf(const uint32_t* words, uint32_t wordCount, uint32_t index, CfWords& out)
{
    const uint32_t slot = index / 2;
    if (slot >= wordCount / 3)
        return false;
    out = UnpackCf(words + 3 * slot, index & 1);
    return true;
}

constexpr CfOpcode CfOpcodeOf(CfWords cf) { return CfOpcode(Bits(cf.word1, 12, 4)); }

constexpr bool IsExec(CfOpcode op)
{
    return op == CfOpcode::Exec || op == CfOpcode::ExecEnd || op == CfOpcode::CondExec || op == CfOpcode::CondExecEnd ||
           op == CfOpcode::CondExecPred || op == CfOpcode::CondExecPredEnd || op == CfOpcode::CondExecPredClean ||
           op == CfOpcode::CondExecPredCleanEnd;
}

// [X]: an *_end exec ends the shader (a conditional one only when its condition holds).
constexpr bool IsExecEnd(CfOpcode op)
{
    return op == CfOpcode::ExecEnd || op == CfOpcode::CondExecEnd || op == CfOpcode::CondExecPredEnd ||
           op == CfOpcode::CondExecPredCleanEnd;
}

// The opcodes whose control flow is straight-line and fully described by this header (nop,
// exec, exec_end, alloc). Every other opcode (conditional exec, loops, calls, jumps, the
// fetch-done hint) is decoded but has semantics nothing here models: callers stop on it.
constexpr bool IsStraightLineCf(CfOpcode op)
{
    return op == CfOpcode::Nop || op == CfOpcode::Exec || op == CfOpcode::ExecEnd || op == CfOpcode::Alloc;
}

// Exec family (opcodes 1-6, 13, 14).
struct CfExec
{
    uint32_t address;      // word0 0-11 [X]; [F] has 9 bits + 3 reserved (agree below 512)
    uint32_t count;        // word0 12-14 [X][F]
    bool yield;            // word0 15 [X][F] (meaning UNKNOWN)
    uint32_t sequence;     // word0 16-27, 2 bits per instruction: bit 0 fetch (1) / ALU (0),
                           // bit 1 serialize [X][F]
    uint32_t vc;           // word0 28-31 | word1 0-1 << 4 [X][F] (meaning UNKNOWN)
    uint32_t boolAddress;  // word1 2-9: bool constant (CondExec, CondExecPredClean*) [X][F]
    bool predicateClean;   // word1 9: Exec, CondExecPred* only [X] ([F]: part of bool_addr)
    bool condition;        // word1 10 [X][F]
    bool absoluteAddress;  // word1 11: address mode [X][F]
};

constexpr bool IsFetch(const CfExec& e, uint32_t i) { return Bits(e.sequence, 2 * i, 1) != 0; }
constexpr bool IsSerialized(const CfExec& e, uint32_t i) { return Bits(e.sequence, 2 * i + 1, 1) != 0; }

constexpr CfExec DecodeExec(CfWords cf)
{
    return CfExec{Bits(cf.word0, 0, 12),
                  Bits(cf.word0, 12, 3),
                  Bits(cf.word0, 15, 1) != 0,
                  Bits(cf.word0, 16, 12),
                  Bits(cf.word0, 28, 4) | (Bits(cf.word1, 0, 2) << 4),
                  Bits(cf.word1, 2, 8),
                  Bits(cf.word1, 9, 1) != 0,
                  Bits(cf.word1, 10, 1) != 0,
                  Bits(cf.word1, 11, 1) != 0};
}

// [X]: alloc type 2 means interpolators in a vertex shader and colours in a pixel shader.
enum class AllocType : uint32_t
{
    None = 0,
    VsPosition = 1,
    InterpolatorsOrColors = 2,
    Memory = 3,
};

struct CfAlloc
{
    uint32_t size;      // word0 0-2 [X]; [F] has 4 bits
    bool unserialized;  // word1 8 [X][F]
    AllocType type;     // word1 9-10 [X][F]
};

constexpr CfAlloc DecodeAlloc(CfWords cf)
{
    return CfAlloc{Bits(cf.word0, 0, 3), Bits(cf.word1, 8, 1) != 0, AllocType(Bits(cf.word1, 9, 2))};
}

// Whether a control-flow instruction allocates a memory export (memexport: the shader writes guest
// memory through eA/eM). Scans the control-flow slots, which end where the first exec's
// instructions begin (exec addresses count 3-word slots).
constexpr bool AllocatesMemoryExport(const uint32_t* words, uint32_t wordCount)
{
    uint32_t codeStart = wordCount / 3;
    for (uint32_t slot = 0; slot < codeStart; slot++)
        for (uint32_t half = 0; half < 2; half++)
        {
            const CfWords cf = UnpackCf(words + 3 * slot, half);
            const CfOpcode op = CfOpcodeOf(cf);
            if (IsExec(op) && DecodeExec(cf).address < codeStart)
                codeStart = DecodeExec(cf).address;
            if (op == CfOpcode::Alloc && DecodeAlloc(cf).type == AllocType::Memory)
                return true;
        }
    return false;
}

// Loops, calls and jumps: fields only (shader_glsl.cpp gives loops their semantics).
struct CfBranch
{
    uint32_t address;      // word0 0-12 [X] ([F]: 10 bits)
    uint32_t loopId;       // word0 16-20 (LoopStart, LoopEnd) [X][F]
    bool isRepeat;         // word0 13 (LoopStart) [X]
    bool unconditional;    // word0 13 (CondCall, CondJmp) [X][F]
    bool predicated;       // word0 14 (CondCall, CondJmp) [X][F]
    bool predicatedBreak;  // word0 21 (LoopEnd) [X]
    uint32_t boolAddress;  // word1 2-9 (CondCall, CondJmp) [X][F]
    bool direction;        // word1 1 (CondJmp) [X][F]
    bool condition;        // word1 10 [X][F]
    bool absoluteAddress;  // word1 11 [X][F]
};

constexpr CfBranch DecodeBranch(CfWords cf)
{
    return CfBranch{Bits(cf.word0, 0, 13),     Bits(cf.word0, 16, 5),      Bits(cf.word0, 13, 1) != 0,
                    Bits(cf.word0, 13, 1) != 0, Bits(cf.word0, 14, 1) != 0, Bits(cf.word0, 21, 1) != 0,
                    Bits(cf.word1, 2, 8),       Bits(cf.word1, 1, 1) != 0,  Bits(cf.word1, 10, 1) != 0,
                    Bits(cf.word1, 11, 1) != 0};
}

// The 3-word instruction in slot `slot`; false past the end of the shader.
constexpr bool ReadSlot(const uint32_t* words, uint32_t wordCount, uint32_t slot, const uint32_t*& out)
{
    if (slot >= wordCount / 3)
        return false;
    out = words + 3 * slot;
    return true;
}

// ---- ALU -------------------------------------------------------------------------------------

// Vector opcodes 0-29 [X][F]. 30 and 31 are undefined in [X] ([F] names 31 VECTOR_NONE).
enum class VectorOpcode : uint32_t
{
    Add = 0, Mul, Max, Min, Seq, Sgt, Sge, Sne, Frc, Trunc, Floor, Mad, CndEq, CndGe, CndGt, Dp4, Dp3,
    Dp2Add, Cube, Max4, SetpEqPush, SetpNePush, SetpGtPush, SetpGePush, KillEq, KillGt, KillGe, KillNe,
    Dst, MaxA,
};
constexpr bool IsKnownVectorOpcode(uint32_t op) { return op <= 29; }

// Scalar opcodes 0-40 and 42-50 [X][F]. 41 and 51-63 are undefined in [X] ([F] names 63
// SCALAR_NONE).
enum class ScalarOpcode : uint32_t
{
    Adds = 0, AddsPrev, Muls, MulsPrev, MulsPrev2, Maxs, Mins, Seqs, Sgts, Sges, Snes, Frcs, Truncs,
    Floors, Exp, Logc, Log, Rcpc, Rcpf, Rcp, Rsqc, Rsqf, Rsq, MaxAs, MaxAsf, Subs, SubsPrev, SetpEq,
    SetpNe, SetpGt, SetpGe, SetpInv, SetpPop, SetpClr, SetpRstr, KillsEq, KillsGt, KillsGe, KillsNe,
    KillsOne, Sqrt,
    Mulsc0 = 42, Mulsc1, Addsc0, Addsc1, Subsc0, Subsc1, Sin, Cos, RetainPrev,
};
constexpr bool IsKnownScalarOpcode(uint32_t op) { return op <= 50 && op != 41; }

// One ALU source operand (source n = 1, 2, 3).
struct AluSource
{
    bool isTemp;        // word2 31 / 30 / 29 for sources 1 / 2 / 3: 1 temp, 0 constant [X][F]
    uint32_t reg;       // word2 16-23 / 8-15 / 0-7: constant index 0-255, or the temp byte below
    uint32_t tempIndex; // temp: reg bits 0-5 [X][F]
    bool tempRelative;  // temp: reg bit 6, aL-relative [X] ([F]: "reg_select")
    bool tempAbs;       // temp: reg bit 7 [X][F]
    uint32_t swizzle;   // word1 16-23 / 8-15 / 0-7, 2 bits per component, relative [X][F]
    bool negate;        // word1 26 / 25 / 24 [X][F]
};

// Component read for result component i: (2-bit field i + i) & 3 [X][F]. 0 = x ... 3 = w.
constexpr uint32_t SwizzleComponent(uint32_t swizzle, uint32_t i) { return (Bits(swizzle, 2 * i, 2) + i) & 3; }

struct AluInstruction
{
    // word0 [X][F]
    uint32_t vectorDest;     // 0-5
    bool vectorDestRelative; // 6
    bool absConstants;       // 7: |constant| for constant sources [X] ([F]: low_precision_16b_fp)
    uint32_t scalarDest;     // 8-13
    bool scalarDestRelative; // 14 (when exporting: the constant-0 enable, see below) [X]
    bool isExport;           // 15
    uint32_t vectorMask;     // 16-19, bit 0 = x
    uint32_t scalarMask;     // 20-23
    bool vectorClamp;        // 24
    bool scalarClamp;        // 25
    uint32_t scalarOpcode;   // 26-31
    // word1 [X][F]
    bool predicateCondition;     // 27 ([F]: pred_select bits 27-28)
    bool predicated;             // 28
    bool constAddressRelativeA0; // 29: relative constants use a0 (1) or aL (0) [X]
    bool const1Addressed;        // 30
    bool const0Addressed;        // 31
    // word2 [X][F]
    uint32_t vectorOpcode;       // 24-28
    AluSource src[3];            // sources 1, 2, 3
};

constexpr AluSource DecodeAluSource(const uint32_t w[3], uint32_t n)
{
    // Field shift for source n: 1 -> 16, 2 -> 8, 3 -> 0.
    const uint32_t shift = 8 * (3 - n);
    const uint32_t reg = Bits(w[2], shift, 8);
    return AluSource{Bits(w[2], 32 - n, 1) != 0, reg, Bits(reg, 0, 6), Bits(reg, 6, 1) != 0, Bits(reg, 7, 1) != 0,
                     Bits(w[1], shift, 8), Bits(w[1], 27 - n, 1) != 0};
}

constexpr AluInstruction DecodeAlu(const uint32_t w[3])
{
    return AluInstruction{Bits(w[0], 0, 6),
                          Bits(w[0], 6, 1) != 0,
                          Bits(w[0], 7, 1) != 0,
                          Bits(w[0], 8, 6),
                          Bits(w[0], 14, 1) != 0,
                          Bits(w[0], 15, 1) != 0,
                          Bits(w[0], 16, 4),
                          Bits(w[0], 20, 4),
                          Bits(w[0], 24, 1) != 0,
                          Bits(w[0], 25, 1) != 0,
                          Bits(w[0], 26, 6),
                          Bits(w[1], 27, 1) != 0,
                          Bits(w[1], 28, 1) != 0,
                          Bits(w[1], 29, 1) != 0,
                          Bits(w[1], 30, 1) != 0,
                          Bits(w[1], 31, 1) != 0,
                          Bits(w[2], 24, 5),
                          {DecodeAluSource(w, 1), DecodeAluSource(w, 2), DecodeAluSource(w, 3)}};
}

// [X] (Xenia's GetConstant0/1WriteMask and src_const_is_addressed; single source, [F] has the
// fields but not these rules): whether constant source n (1-3) is relatively addressed. The
// flag used is const_0 for the first constant among the sources and const_1 for any later one.
constexpr bool IsConstantAddressed(const AluInstruction& a, uint32_t n)
{
    if (n == 1)
        return a.const0Addressed;
    if (n == 2)
        return a.src[0].isTemp ? a.const0Addressed : a.const1Addressed;
    return (a.src[0].isTemp && a.src[1].isTemp) ? a.const0Addressed : a.const1Addressed;
}

// Write masks [X]. Not exporting: the vector op writes vectorDest.vectorMask and the scalar op
// scalarDest.scalarMask. Exporting: both ops write vectorDest, per component:
//   vector 1, scalar 0 -> vector result;   vector 0, scalar 1 -> scalar result;
//   vector 1, scalar 1 -> constant 1;      vector 0, scalar 0 -> constant 0 when
//   scalarDestRelative is set, otherwise unchanged.
constexpr uint32_t VectorResultMask(const AluInstruction& a)
{
    return a.isExport ? (a.vectorMask & ~a.scalarMask) : a.vectorMask;
}
constexpr uint32_t ScalarResultMask(const AluInstruction& a)
{
    return a.isExport ? (a.scalarMask & ~a.vectorMask) : a.scalarMask;
}
constexpr uint32_t Constant1Mask(const AluInstruction& a) { return a.isExport ? (a.vectorMask & a.scalarMask) : 0; }
constexpr uint32_t Constant0Mask(const AluInstruction& a)
{
    return (a.isExport && a.scalarDestRelative) ? (0xFu & ~(a.vectorMask | a.scalarMask)) : 0;
}
constexpr uint32_t ScalarResultDest(const AluInstruction& a) { return a.isExport ? a.vectorDest : a.scalarDest; }

// Export register numbers (vectorDest when isExport) [X]; [F] names only 62, 63 and PS 0.
enum class ExportTarget
{
    Unknown,
    Interpolator,  // VS 0-15
    Position,      // VS 62 (oPos)
    PointSizeEdgeKill, // VS 63 (oPts)
    MemExportAddress,  // VS/PS 32 (eA)
    MemExportData,     // VS/PS 33-37 (eM0-eM4)
    Color,         // PS 0-3 (oC0-oC3)
    Depth,         // PS 61 (oDepth)
};

constexpr ExportTarget ClassifyExport(bool pixelShader, uint32_t dest)
{
    if (dest == 32)
        return ExportTarget::MemExportAddress;
    if (dest >= 33 && dest <= 37)
        return ExportTarget::MemExportData;
    if (pixelShader)
        return dest <= 3 ? ExportTarget::Color : dest == 61 ? ExportTarget::Depth : ExportTarget::Unknown;
    if (dest <= 15)
        return ExportTarget::Interpolator;
    return dest == 62 ? ExportTarget::Position : dest == 63 ? ExportTarget::PointSizeEdgeKill : ExportTarget::Unknown;
}

// ---- Fetch -----------------------------------------------------------------------------------

// word0 bits 0-4 [X][F]. The vertex fetch (0) and texture fetch (1) are decoded here; the
// texture helper opcodes (16-19, 24-26) are recognised but not decoded; anything else is
// undefined. Callers stop on both.
constexpr uint32_t FetchOpcodeOf(const uint32_t w[3]) { return Bits(w[0], 0, 5); }
constexpr uint32_t kVertexFetch = 0;
constexpr uint32_t kGetTextureGradients = 18;  // getTextureGradients (Xenia ucode.h FetchOpcode)
constexpr uint32_t kSetTextureLod = 24;        // setTextureLod (Xenia ucode.h FetchOpcode)
constexpr uint32_t kTextureFetch = 1;
constexpr bool IsKnownFetchOpcode(uint32_t op)
{
    return op == 0 || op == 1 || (op >= 16 && op <= 19) || (op >= 24 && op <= 26);
}

// Fetch destination swizzle, 3 bits per component [X][F]: 0-3 = x-w, 4 = 0, 5 = 1,
// 6 = UNKNOWN, 7 = keep (not written).
enum class FetchDestComponent : uint32_t
{
    X = 0, Y, Z, W, Zero, One, Unknown6, Keep,
};
constexpr FetchDestComponent FetchDestSwizzle(uint32_t destSwizzle, uint32_t i)
{
    return FetchDestComponent(Bits(destSwizzle, 3 * i, 3));
}

// Vertex formats named by [X] (gpu/xenos.h VertexFormat). Any other value is unsupported.
enum class VertexFormat : uint32_t
{
    F8_8_8_8 = 6, F2_10_10_10 = 7, F10_11_11 = 16, F11_11_10 = 17, F16_16 = 25, F16_16_16_16 = 26,
    F16_16_FLOAT = 31, F16_16_16_16_FLOAT = 32, F32 = 33, F32_32 = 34, F32_32_32_32 = 35, F32_FLOAT = 36,
    F32_32_FLOAT = 37, F32_32_32_32_FLOAT = 38, F32_32_32_FLOAT = 57,
};

// Components per vertex element [X]; 0 for a format [X] does not name.
constexpr uint32_t VertexFormatComponents(uint32_t format)
{
    switch (format)
    {
    case 33: case 36: return 1;
    case 25: case 31: case 34: case 37: return 2;
    case 16: case 17: case 57: return 3;
    case 6: case 7: case 26: case 32: case 35: case 38: return 4;
    default: return 0;
    }
}

struct VertexFetch
{
    // word0 [X][F]
    uint32_t srcReg;            // 5-10: register holding the index
    bool srcRelative;           // 11
    uint32_t destReg;           // 12-17
    bool destRelative;          // 18
    bool mustBeOne;             // 19
    uint32_t constIndex;        // 20-24
    uint32_t constIndexSelect;  // 25-26
    uint32_t prefetchCount;     // 27-29 [X] ([F]: reserved; meaning UNKNOWN)
    uint32_t srcSwizzle;        // 30-31: the index component, 0 = x ... 3 = w
    // word1
    uint32_t destSwizzle;       // 0-11 [X][F]
    bool isSigned;              // 12: format_comp_all [X][F]
    bool isNormalized;          // 13: num_format_all == 0 [X][F]
    bool signedRfModeAll;       // 14 [X][F] (signed repeating-fraction mode, meaning per [X])
    bool isIndexRounded;        // 15: round instead of floor the index [X] ([F]: reserved)
    uint32_t format;            // 16-21 [X][F]
    int32_t expAdjust;          // 24-29, signed [X] ([F]: unsigned exp_adjust_all)
    bool isMiniFetch;           // 30 [X] only ([F]: reserved3): reuses the previous full fetch's
                                // address; its own const/src/stride are not used
    bool predicated;            // 31 [X][F]
    // word2
    uint32_t stride;            // 0-7, in dwords [X][F]
    int32_t offset;             // 8-30, dwords, signed [X] ([F]: 22 bits unsigned + reserved)
    bool predicateCondition;    // 31 [X][F]
};

// Vertex fetch constant slot 0-95: constIndex * 3 + constIndexSelect [X] (each 6-dword fetch
// constant register holds three 2-dword vertex fetch constants).
constexpr uint32_t FetchConstantSlot(const VertexFetch& f) { return f.constIndex * 3 + f.constIndexSelect; }

constexpr VertexFetch DecodeVertexFetch(const uint32_t w[3])
{
    return VertexFetch{Bits(w[0], 5, 6),
                       Bits(w[0], 11, 1) != 0,
                       Bits(w[0], 12, 6),
                       Bits(w[0], 18, 1) != 0,
                       Bits(w[0], 19, 1) != 0,
                       Bits(w[0], 20, 5),
                       Bits(w[0], 25, 2),
                       Bits(w[0], 27, 3),
                       Bits(w[0], 30, 2),
                       Bits(w[1], 0, 12),
                       Bits(w[1], 12, 1) != 0,
                       Bits(w[1], 13, 1) == 0,
                       Bits(w[1], 14, 1) != 0,
                       Bits(w[1], 15, 1) != 0,
                       Bits(w[1], 16, 6),
                       SignedBits(w[1], 24, 6),
                       Bits(w[1], 30, 1) != 0,
                       Bits(w[1], 31, 1) != 0,
                       Bits(w[2], 0, 8),
                       SignedBits(w[2], 8, 23),
                       Bits(w[2], 31, 1) != 0};
}

// Texture fetch (opcode 1) [X] only (Xenia TextureFetchInstruction; not cross-checked).
// Filter fields: 0 point, 1 linear, 2 base map (mip filter only), 3 use the fetch constant.
// Aniso: 0 disabled, 1-5 max 1:1 ... 16:1, 7 use the fetch constant. Dimension: 0 1D, 1 2D,
// 2 3D or stacked, 3 cube. Sample location: 0 centroid, 1 center.
constexpr uint32_t kFilterUseFetchConst = 3;
constexpr uint32_t kAnisoUseFetchConst = 7;
constexpr uint32_t kArbitraryUseFetchConst = 7;
constexpr uint32_t kDimension2D = 1;
// 3D or stacked: the instruction cannot tell them apart; the fetch constant's dimension (dword 5
// bits 9-10) decides at draw time [X].
constexpr uint32_t kDimension3DOrStacked = 2;
constexpr uint32_t kDimensionCube = 3;

struct TextureFetch
{
    // word0
    uint32_t srcReg;             // 5-10
    bool srcRelative;            // 11
    uint32_t destReg;            // 12-17
    bool destRelative;           // 18
    bool fetchValidOnly;         // 19 (meaning UNKNOWN)
    uint32_t constIndex;         // 20-24: texture fetch constant 0-31
    bool unnormalized;           // 25: texel, not normalized, coordinates
    uint32_t srcSwizzle;         // 26-31: 2 bits per coordinate, absolute (0 = x ... 3 = w)
    // word1
    uint32_t destSwizzle;        // 0-11, as the vertex fetch's (FetchDestSwizzle)
    uint32_t magFilter;          // 12-13
    uint32_t minFilter;          // 14-15
    uint32_t mipFilter;          // 16-17
    uint32_t anisoFilter;        // 18-20
    uint32_t arbitraryFilter;    // 21-23
    uint32_t volMagFilter;       // 24-25
    uint32_t volMinFilter;       // 26-27
    bool useComputedLod;         // 28
    bool useRegisterLod;         // 29
    bool unknown30;              // 30 (meaning UNKNOWN)
    bool predicated;             // 31
    // word2
    bool useRegisterGradients;   // 0
    uint32_t sampleLocation;     // 1
    int32_t lodBias;             // 2-8, signed, in 1/16 LOD units
    uint32_t dimension;          // 14-15
    int32_t offsetX;             // 16-20, signed, in half texels
    int32_t offsetY;             // 21-25
    int32_t offsetZ;             // 26-30
    bool predicateCondition;     // 31
};

// Source coordinate i (0-2) of a texture fetch: absolute component 0-3.
constexpr uint32_t TextureSourceComponent(const TextureFetch& t, uint32_t i) { return Bits(t.srcSwizzle, 2 * i, 2); }

constexpr TextureFetch DecodeTextureFetch(const uint32_t w[3])
{
    return TextureFetch{Bits(w[0], 5, 6),        Bits(w[0], 11, 1) != 0,    Bits(w[0], 12, 6),
                        Bits(w[0], 18, 1) != 0,  Bits(w[0], 19, 1) != 0,    Bits(w[0], 20, 5),
                        Bits(w[0], 25, 1) != 0,  Bits(w[0], 26, 6),         Bits(w[1], 0, 12),
                        Bits(w[1], 12, 2),       Bits(w[1], 14, 2),         Bits(w[1], 16, 2),
                        Bits(w[1], 18, 3),       Bits(w[1], 21, 3),         Bits(w[1], 24, 2),
                        Bits(w[1], 26, 2),       Bits(w[1], 28, 1) != 0,    Bits(w[1], 29, 1) != 0,
                        Bits(w[1], 30, 1) != 0,  Bits(w[1], 31, 1) != 0,    Bits(w[2], 0, 1) != 0,
                        Bits(w[2], 1, 1),        SignedBits(w[2], 2, 7),    Bits(w[2], 14, 2),
                        SignedBits(w[2], 16, 5), SignedBits(w[2], 21, 5),   SignedBits(w[2], 26, 5),
                        Bits(w[2], 31, 1) != 0};
}

}  // namespace runtime::shader::ucode
