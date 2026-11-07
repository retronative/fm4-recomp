// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// From a decoded DrawState (draw_state.h) to what the recorder needs, on the CPU and without Vulkan
// types, so that tests/runtime/test_draw_record.cpp covers it: the pipeline key and the shader
// variants of the draw (pipeline_key.h, from the decoded fields instead of the raw registers
// DrawStateView carried before DrawState existed), the host indices (auto-index, quad lists, rect
// lists, DMA indices), the byte images of the XeSystem and constant blocks (shader_glsl.h bindings 1,
// 2, 3), and the NoWrite policy switch. Every refusal is a KeyResult the renderer stops with
// (renderer.cpp:Draw, "Require"); nothing here stops or skips on its own.
#pragma once
#include "draw_state.h"
#include "pipeline_key.h"
#include "shader_cache.h"
#include <cstdint>
#include <string>
#include <vector>

namespace runtime::video {

// What a draw that writes nothing (DrawKind::NoWrite: RB_COLOR_MASK 0, no depth or stencil write;
// FM4's legal-screen point lists: 19,187 of 21,643 draws in CP trace10) does on the
// host. Owner policy pending: Skip records nothing and counts the draw in the swap line;
// Stop treats it as unimplemented. One switch, read in renderer.cpp:Draw only.
enum class NoWriteDraws
{
    Skip,
    Stop,
};
constexpr NoWriteDraws kNoWriteDraws = NoWriteDraws::Skip;

// The pipeline key of a Draw or DepthOnly state (a NoWrite or Resolve state is refused: they never
// reach a pipeline). Normalization as pipeline_key.h documents; the topology comes from
// DrawState::hostPrimitive (rect and quad lists are triangle lists on the host), `samples` from the
// target (1, 2 or 4: the EDRAM unit's images carry the sample count, pipeline_cache.cpp), the depth
// attachment is present when the depth test or the stencil test is on (a test without a write still
// needs it), colour 0 when the draw writes colour (DrawKind::Draw with a non-zero mask).
KeyResult KeyFromDrawState(const DrawState& d, const CachedShader& vs, const CachedShader& ps, uint64_t vsVariant, uint64_t psVariant, PipelineKey& out);

// The host defines (shader_glsl.h) of each stage for this draw: XE_RECT_LIST for a rect list;
// XE_TEXTURE_<slot>_GAMMA (TextureSlot::gamma: x y z sign 3, w 0), _SCALE for a non-zero exp_adjust,
// _LOD_BIAS for a non-zero lod_bias (/ 32), for the stage(s) whose translation lists the slot;
// XE_ALPHA_TEST / XE_ALPHA_REF when the alpha test is on (func != 7); XE_COLOR0_GAMMA for storage
// format 1, XE_COLOR0_7E3 for 3, when colour is written.
KeyResult VariantsFromDrawState(const DrawState& d, const CachedShader& vs, const CachedShader& ps, ShaderVariantDefines& vsDefines,
                                ShaderVariantDefines& psDefines);

// ---- Indices --------------------------------------------------------------------------------------------

// A guest index as the GPU reads it: `i` is the element number; the result is the raw 16- or 32-bit
// value, host order (the caller applies the DMA endian: pm4 DRAW_INDX, Xenia primitive_processor.cc).
using GuestIndexReader = uint32_t (*)(const void* context, uint32_t i);

struct HostIndices
{
    enum class Kind
    {
        Direct,    // vkCmdDraw(vertexCount): gl_VertexIndex is the guest index (auto-index, no offset, no clamping)
        Indexed,   // vkCmdDrawIndexed with `indices` (32-bit, host order) in the ring
        RectList,  // vkCmdDraw(vertexCount = 6 per rect) with `indices` (3 guest indices per rect) bound as xe_indices (binding 4)
    };
    Kind kind = Kind::Direct;
    std::vector<uint32_t> indices;
    uint32_t vertexCount = 0;  // what the host draws
    uint32_t highIndex = 0;    // the largest guest index the vertex shader may fetch (VertexWindowDwords)
};

// Index i of the draw -> the guest index the vertex shader sees: ((raw + VGT_INDX_OFFSET) & 0xFFFFFF)
// clamped to [VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX] (draw_state.h; X registers.h:299-360), raw = i for an
// auto-indexed draw (source select 2) or the DMA buffer's element for an indexed one (source 0; `read`
// is then required). Quad lists become triangle lists (q0 q1 q2)(q0 q2 q3) (X primitive_processor.h
// QuadListToTriangleList, draw_state.h); rect lists keep their 3 guest indices per rect for the vertex
// shader's XE_RECT_LIST expansion (shader_glsl.h). Refuses: 0 indices; a rect list whose count is not
// a multiple of 3, a quad list not a multiple of 4; an indexed draw without a reader; a DMA endian
// other than none / 8in16 for 16-bit or none / 8in32 for 32-bit indices (the others have no observed
// meaning: refused, not guessed); a 16-bit DMA buffer whose bytes are fewer than 2 * count (32-bit: 4 * count).
KeyResult BuildHostIndices(const DrawState& d, GuestIndexReader read, const void* context, HostIndices& out);

// The DMA endian applied to one raw element (X xenos.h:193-198 Endian; command_processor.h GpuSwap for 32-bit).
uint32_t SwapIndex(uint32_t raw, bool is32Bit, uint32_t endian);

// ---- Uploads --------------------------------------------------------------------------------------------

// Per used vertex fetch slot: the window the host copies and where it lands (pipeline_key.h D4).
struct VertexWindow
{
    uint32_t slot = 0, physical = 0, endian = 0;
    uint32_t dwords = 0;     // VertexWindowDwords(slot, highIndex): 0 for an invalid constant (an empty window, not a refusal)
    uint32_t baseDword = 0;  // offset in the draw's vertex data block (xe_vertex_base[slot])
};
// The windows of every slot the vertex shader lists, laid out consecutively; `totalDwords` the block's size.
void PlanVertexWindows(const DrawState& d, uint32_t highIndex, std::vector<VertexWindow>& out, uint32_t& totalDwords);

// XeSystem (binding 1, kSystemBytes): xe_fetch[48] holds, for each used vertex slot, word 0 as the
// constant (address | 3) and word 1 as (window dwords << 2) | endian, so the shader's bounds check
// (xe_vertex_word) sees the copied window as the slot's size; every other fetch entry is 0 (the shader
// reads xe_fetch for vertex slots only). xe_vertex_base[slot] = the window's baseDword; the viewport
// scale/offset vec4s from FoldViewport; xe_flags = DrawState::systemFlags (shader_glsl.h kFlag*, from PA_CL_VTE_CNTL).
void FillXeSystem(const std::vector<VertexWindow>& windows, const HostViewport& viewport, uint32_t vteFlags, uint8_t out[kSystemBytes]);

// XeVsConstants / XePsConstants (bindings 2 / 3, kConstantsBytes): the stage's 256 float4 (DrawState
// aluConstants words 0-1023 for the VS, 1024-2047 for the PS, as the register file holds them), then
// the 8 bool and 32 loop dwords (both stages get the same).
void FillConstants(const DrawState& d, bool pixel, uint8_t out[kConstantsBytes]);

// ---- The draw extent from the vertices (FM4 Recomp) -----------------------------------------

// The rows the EDRAM bind may claim for this draw (edram::Bind's heightUsed): d.target.targetHeight, lowered to
// min(edramRows, edram::EstimateMaxY(d.extent, &vertexMaxY)) when d.extentNeedsVertices (X render_target_cache.cc:
// 757-767; draw_extent_estimator.cc:199-264) and the vertex shader's position is a plain vertex fetch
// (ShaderTranslation::positionFetch). The positions are read from `window`, the guest memory of that fetch's slot
// as 32-bit loads (`windowDwords` long, from the fetch constant's address; the bytes the renderer copies), at each
// guest index the draw references (`indices`: every index of Direct, else its index list) with the slot's
// `endian` (edram::PassThroughPositions, w = 1). When the vertices are needed but the shader does not qualify, the
// window is null or too short, `height` stays the scissor bound (today's behaviour: the bind may then refuse) and
// `note` says why, for the stop line. Returns true when the vertex bound was applied.
bool BoundTargetHeight(const DrawState& d, const shader::ShaderTranslation& vs, const uint32_t* window, size_t windowDwords, uint32_t endian,
                       const HostIndices& indices, uint32_t& height, std::string& note);

}  // namespace runtime::video
