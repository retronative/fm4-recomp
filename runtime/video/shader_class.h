// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The shader "class" tag of the isolation rule (a never-seen
// shader class (loops, jumps, calls) runs first in its own isolated, approved run; execution model
// §4 and §7 unit 8). The class is read from the microcode's control-flow program alone (ucode.h
// opcodes, Xenia 67f16c4e315f ucode.h ControlFlowOpcode), not from what the translator accepted,
// so the tag is the same whichever translator version runs and names what the title asked for.
//
// Bits (a shader's class is the union over its control-flow instructions):
//   kBranch     conditional execs on a bool constant (3, 4, 13, 14) and conditional jumps (11)
//   kPredicate  execs conditional on the predicate p0 (5, 6)
//   kLoop       loop_start / loop_end (7, 8)
//   kCall       cond_call (9) and return (10): the translator refuses them; listed so the tag names them
//   kUnknown    opcode 15 (mark_vs_fetch_done) or a control-flow program with no exec_end: refused always
// A shader with none of them is straight-line (nop, exec, exec_end, alloc).
//
// The allow-list: `fm4recomp --shader-classes <list>` (required with a GPU renderer mode, no silent
// default; required arguments have no silent defaults). "none" allows straight-line
// shaders only; "branch,predicate,loop" allows those classes too; "call" and unknown opcodes are
// never allowed. The renderer checks a pair's classes before its first pipeline is created
// (pipeline_cache.cpp), because MoltenVK compiles the MSL at vkCreateGraphicsPipelines and the hang
// hypothesis of the execution model (§8: PS 8776478A) sits below the GLSL. The run log names each
// loaded shader's class so a reviewer sees which classes a screen needs before approving them.
#pragma once
#include "shader/ucode.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace runtime::video {

enum ShaderClassBits : uint32_t
{
    kClassStraight = 0,
    kClassBranch = 1u << 0,
    kClassPredicate = 1u << 1,
    kClassLoop = 1u << 2,
    kClassCall = 1u << 3,
    kClassUnknown = 1u << 4,
};
using ShaderClass = uint32_t;

// The control-flow program ends where the first exec's instructions begin (ucode.h
// AllocatesMemoryExport, shader_glsl.cpp ProgramReadsPs: exec addresses count 3-word slots), or at
// the first exec_end when that comes first; a program whose control flow runs into its
// instructions, or has no exec_end, is kClassUnknown (the translator refuses it too).
inline ShaderClass ScanShaderClass(const uint32_t* words, uint32_t count)
{
    using namespace runtime::shader::ucode;
    ShaderClass cls = kClassStraight;
    uint32_t firstInstruction = count / 3;
    bool ended = false;
    CfWords cf;
    for (uint32_t i = 0; !ended && i / 2 < firstInstruction && ReadCf(words, count, i, cf); i++)
    {
        const CfOpcode op = CfOpcodeOf(cf);
        switch (op)
        {
        case CfOpcode::Nop: case CfOpcode::Alloc: break;
        case CfOpcode::Exec: case CfOpcode::ExecEnd:
            firstInstruction = std::min(firstInstruction, DecodeExec(cf).address);
            ended = op == CfOpcode::ExecEnd;
            break;
        case CfOpcode::CondExec: case CfOpcode::CondExecEnd: case CfOpcode::CondExecPredClean: case CfOpcode::CondExecPredCleanEnd:
            firstInstruction = std::min(firstInstruction, DecodeExec(cf).address);
            cls |= kClassBranch;
            ended = IsExecEnd(op);
            break;
        case CfOpcode::CondExecPred: case CfOpcode::CondExecPredEnd:
            firstInstruction = std::min(firstInstruction, DecodeExec(cf).address);
            cls |= kClassPredicate;
            ended = IsExecEnd(op);
            break;
        case CfOpcode::LoopStart: case CfOpcode::LoopEnd: cls |= kClassLoop; break;
        case CfOpcode::CondJmp: cls |= kClassBranch; break;
        case CfOpcode::CondCall: case CfOpcode::Return: cls |= kClassCall; break;
        default: cls |= kClassUnknown; break;  // 15 (mark_vs_fetch_done)
        }
    }
    if (!ended)
        cls |= kClassUnknown;
    return cls;
}

inline std::string ShaderClassName(ShaderClass cls)
{
    if (cls == kClassStraight)
        return "straight";
    std::string s;
    const struct { ShaderClass bit; const char* name; } names[] = {
        {kClassBranch, "branch"}, {kClassPredicate, "predicate"}, {kClassLoop, "loop"}, {kClassCall, "call"}, {kClassUnknown, "unknown"}};
    for (const auto& n : names)
        if (cls & n.bit)
            s += (s.empty() ? "" : ",") + std::string(n.name);
    return s;
}

// Parses --shader-classes: "none", or a comma-separated list of branch, predicate, loop. Anything
// else (an empty string, "call", "unknown", a typo) is refused with `error` set: a mistyped
// allow-list must not silently allow or refuse.
inline bool ParseShaderClassAllowList(const char* text, ShaderClass& allowed, std::string& error)
{
    allowed = kClassStraight;
    if (text == nullptr || *text == '\0')
        return error = "--shader-classes is empty (use none, or a list of branch, predicate, loop)", false;
    const std::string s = text;
    if (s == "none")
        return true;
    for (size_t at = 0; at <= s.size();)
    {
        const size_t end = std::min(s.find(',', at), s.size());
        const std::string item = s.substr(at, end - at);
        if (item == "branch")
            allowed |= kClassBranch;
        else if (item == "predicate")
            allowed |= kClassPredicate;
        else if (item == "loop")
            allowed |= kClassLoop;
        else
            return error = "--shader-classes: \"" + item + "\" is not one of branch, predicate, loop", allowed = kClassStraight, false;
        at = end + 1;
    }
    return true;
}

// The classes of a pair that the allow-list does not cover (0: the pair may run).
inline ShaderClass RefusedClasses(ShaderClass vs, ShaderClass ps, ShaderClass allowed) { return (vs | ps) & ~allowed; }

}  // namespace runtime::video
