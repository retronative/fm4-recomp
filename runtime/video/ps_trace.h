// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (diagnostics behind environment flags, in separate files):
// FM4_TRACE_PS=<16 hex digits, a pixel shader hash as the CP trace / census name it>[,<hash>...] (up to 8 hashes)
// makes the renderer write,
// for each draw whose current pixel shader has that hash (at most kPsTraceCap draws per run), lines to stderr
// starting "[ps-trace]": the draw number, both hashes, VS constants c0-c23 (c12-c23 for the
// panel/logo vertex shaders ae82c3ff7e3cf328 / c565a1113f939851), PS constants c256-c259, c510, c511,
// RB_ALPHA_REF, the 8 bool and 32 loop constant dwords, PA_SC_WINDOW_OFFSET / WINDOW_SCISSOR_TL, and the first
// three vertices of vertex fetch slot 95 as the vertex shader reads them. Unset or empty: nothing (one getenv at
// the first draw). A malformed value stops the run (fail closed: a mistyped hash must not trace nothing silently).
//
// Register indices (Xenia 67f16c4e315f register_table.inc): SHADER_CONSTANT_000_X 0x4000 (:655), so float constant
// n (x, y, z, w) is 0x4000 + 4n .. + 3; the pixel shader's c[k] is constant 256 + k (shader_glsl.h: binding 3 holds
// 256-511 rebased to 0), i.e. 0x4400 + 4k (k 254 -> 0x47F8, k 255 -> 0x47FC). RB_ALPHA_REF 0x210E (:522),
// PA_SC_WINDOW_OFFSET 0x2080 (:504), PA_SC_WINDOW_SCISSOR_TL 0x2081 (:505), SHADER_CONSTANT_BOOL_000_031 0x4900
// (:2897), SHADER_CONSTANT_LOOP_00 0x4908 (:2906), SHADER_CONSTANT_FETCH_00_0 0x4800 (:2704): vertex slot s at
// 0x4800 + 2s, 2s + 1 (slot 95: 0x48BE, 0x48BF).
#pragma once
#include "shader/shader_glsl.h"
#include <cstdint>

namespace runtime::video {

constexpr uint32_t kPsTraceCap = 64;

// From renderer.cpp:Draw once both stages are known, before the draw state is decoded (so a refused draw is traced too).
void TracePsDraw(const uint8_t* base, uint64_t draw, uint64_t vsHash, uint64_t psHash, const shader::ShaderTranslation& vs);

// For a draw TracePsDraw traced, after the EDRAM bind and the dynamic state are set, one more line:
// RB_DEPTHCONTROL, PA_SU_SC_MODE_CNTL, the decoded render target (pitch, MSAA, colour/depth bases and formats, pitch in
// tiles, EDRAM rows, the bind's height), the pipeline key's depth state (attached, write, func, stencil), the host
// targets' size and samples, the host scissor and viewport (with its depth range).
struct PsBindTrace
{
    uint32_t depthControl = 0, suScModeCntl = 0;
    uint32_t pitch = 0, msaa = 0, colorBase = 0, colorFormat = 0, depthBase = 0, depthFormat = 0, pitchTiles = 0, edramRows = 0, targetHeight = 0;
    bool colorWritten = false, depthWritten = false;
    uint32_t keyDepthUsed = 0, keyDepthWrite = 0, keyDepthFunc = 0, keyStencil = 0, keySamples = 0;
    uint32_t hostWidth = 0, hostHeight = 0, hostSamples = 0;
    int32_t scissorX = 0, scissorY = 0;
    uint32_t scissorWidth = 0, scissorHeight = 0;
    float viewportWidth = 0, viewportHeight = 0, minDepth = 0, maxDepth = 0;
    bool noDepthDiagnostic = false;  // FM4_DIAG_PS_NO_DEPTH applied to this draw (diag_depth.h)
    // RB_COLORCONTROL (alpha test enable bit 3, function bits 0-2) and the pixel shader variant's host
    // defines (XE_ALPHA_TEST / XE_ALPHA_REF present or not: shader_glsl.h), so a trace shows whether the alpha test runs.
    uint32_t colorControl = 0;
    // The per-draw registers of the z path (Xenia register_table.inc :523-528, :547, :549, :580):
    // PA_CL_VPORT_XSCALE..ZOFFSET 0x210F-0x2114 (raw bits), PA_CL_CLIP_CNTL 0x2204, PA_CL_VTE_CNTL 0x2206, PA_SU_VTX_CNTL 0x2302,
    // and what the renderer derived: FoldViewport's z scale/offset (gl_Position.z = p.z * scale + offset * p.w) and the
    // translator's xe_flags (DrawState::systemFlags).
    uint32_t vport[6] = {}, clipCntl = 0, vteCntl = 0, vtxCntl = 0, systemFlags = 0;
    float foldZScale = 0, foldZOffset = 0;
    const char* psDefines = "";
};
void TracePsBind(uint64_t draw, const PsBindTrace& b);

// The stored depth of a fragment at clip-space (z, w) as the renderer produces it (the folded z, then the
// Vulkan viewport transform: minDepth + (maxDepth - minDepth) * z_ndc). Pure; tested on the traced glow / text values.
inline float HostStoredDepth(float z, float w, float foldZScale, float foldZOffset, float minDepth, float maxDepth)
{
    const float zNdc = (z * foldZScale + foldZOffset * w) / w;
    return minDepth + (maxDepth - minDepth) * zNdc;
}

}  // namespace runtime::video
