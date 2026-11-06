// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/gpu/shader_glsl.h (GPL-3.0-or-later, same owner),
// reviewed and re-tested here (tests/shader/run_shader_tests.sh). FM4 Recomp changes: namespace gpu -> runtime::shader, include paths
// gpu/ -> shader/; this comment documents what the emitted GLSL already declared (samplerCube for
// a cube slot, XE_TEXTURE_<slot>_LOD_BIAS, XE_COLOR0_SCALE); the rect-list and ForzaRecomp-path
// references are marked as ForzaRecomp's. The contract below is ForzaRecomp's renderer's; FM4
// Recomp's host has yet to implement and check it. FM4 Recomp: a pixel shader
// may `discard` at the end of main (kill ops) and textureSlots includes conditionally skipped fetches.
//
// Xenos shader microcode -> GLSL 450 (Vulkan), for exactly the instruction subset FM4's first
// frames use; everything else is refused with an error naming it, never skipped. Semantics and
// their labels are in shader_glsl.cpp.
//
// Host binding contract (descriptor set 0):
//   0  std430 XeVertexData { uint xe_vertex_words[]; }                         (VS)
//   1  std140 XeSystem { uvec4 xe_fetch[48]; uvec4 xe_vertex_base[24];
//                        vec4 xe_viewport_scale; vec4 xe_viewport_offset; uint xe_flags; } (VS)
//        xe_fetch: the raw vertex fetch constants, slot s = words 2s, 2s+1 (endian = word 1
//        bits 0-1); xe_vertex_base[s/4][s%4]: slot s's first word in xe_vertex_words.
//   2  std140 XeVsConstants { vec4 xe_c[256]; uvec4 xe_bool[2]; uvec4 xe_loop[8]; }  (VS)
//   3  std140 XePsConstants { vec4 xe_c[256]; uvec4 xe_bool[2]; uvec4 xe_loop[8]; }  (PS)
//        xe_c: ALU constants 0-255 (VS) or 256-511 rebased to 0 (PS), bytes 0-4095.
//        xe_bool (bytes 4096-4127): the eight raw bool constant dwords, registers
//        SHADER_CONSTANT_BOOL_000_031..224_255 (0x4900-0x4907) in order, so bool constant b
//        is bit b % 32 of xe_bool[b / 128][(b / 32) % 4]; the same 256 bools serve both
//        stages (indices are absolute, not rebased). Declared when the shader's
//        boolConstants or loopConstants is non-empty; the host then binds at least 4128 bytes.
//        xe_loop (bytes 4128-4255): the 32 raw loop constant dwords, registers
//        SHADER_CONSTANT_LOOP_00..31 (0x4908-0x4927) in order, so loop constant L is
//        xe_loop[L / 4][L % 4] (count bits 0-7); the same 32 serve both stages. Declared only
//        when loopConstants is non-empty (xe_bool is then declared too, used or not); the host
//        then binds at least 4256 bytes.
//   4  std430 XeIndices { uint xe_indices[]; }    guest indices of a rect list    (VS, XE_RECT_LIST)
//   5  std430 XeErrors { uint xe_error; }          bit 0: a vertex fetch outside its slot's
//                                                  data (fetch word 1 size); bit 1: a 16-bit
//                                                  float vertex component with exponent 31
//                                                  (meaning UNKNOWN); the host zeroes it
//                                                  before and stops after a draw that sets
//                                                  either bit (VS)
//   8 + slot  sampler2D, sampler3D or samplerCube xe_texture_<slot> (textureDimensions 1, 2,
//        3)                                                                     (PS, VS)
//        declared exactly as `layout(set = 0, binding = <8 + slot>) uniform sampler2D
//        xe_texture_<slot>;` or the same with `sampler3D` / `samplerCube`.
// The vertex shader's body is `void xe_vertex(uint index, out vec4 position, out vec4
// interp[N])` (N = max(interpolatorCount, 1)). Its default main runs it once for
// gl_VertexIndex (r0.x = float(index): the host passes the guest index already endian-swapped
// and offset, as Xenia does before writing r0.x); with `#define XE_RECT_LIST` inserted by the
// host as line 2 (line 1 is `#version 450`), main expands rect lists: draw 6 vertices per rect,
// guest indices xe_indices[3 * rect + 0..2], corners as ForzaRecomp runtime/video/renderer.cpp
// ExpandRectList and Xenia's rectangle-list geometry shader (longest edge, v3 =
// (v1 - v0) + v2), here on the exported positions before the viewport fold.
// The host may also insert, after line 1, `#define XE_TEXTURE_<slot>_GAMMA` (pixel shader; the
// fetch result's x, y, z for that slot, after the fetch constant's swizzle, are all sign 3 =
// gamma: the shader applies Xenos PWL gamma -> linear to them; the image is plain UNORM) and
// `#define XE_COLOR0_GAMMA` (pixel shader; colour 0's target is RB_COLOR_INFO format 1,
// k_8_8_8_8_GAMMA, bound as a UNORM image: the shader encodes xyz linear -> PWL gamma at the
// end of main, alpha unchanged) or `#define XE_COLOR0_7E3` (colour 0's target is format 3,
// k_2_10_10_10_FLOAT, on a float host image: RGB clamped to [0, 31.875], alpha to [0, 1]).
// `#define XE_TEXTURE_<slot>_SCALE ldexp(1.0, n)` multiplies that slot's fetched texels (after
// any gamma conversion) by 2^n, the fetch constant's exponent bias (dword 3 bits 13-18).
// `#define XE_TEXTURE_<slot>_LOD_BIAS <float>` adds the fetch constant's LOD bias (dword 4 bits
// 12-21, signed, in 1/32 LOD units: the host passes the value in LODs) to that slot's LOD.
// `#define XE_ALPHA_TEST <func 0-7>` with `#define XE_ALPHA_REF <float>` (pixel shader): discard
// where colour 0's alpha (as exported) fails func(alpha, ref) (xenos::CompareFunction).
// `#define XE_COLOR0_SCALE <float>` (pixel shader) multiplies colour 0 by it after the alpha test
// and the conversions (ForzaRecomp: 2^(bias - 5) for a 16_16_16_16 target held as SNORM).
// Without them the shader neither converts, clamps, scales nor tests.
// The exported position p goes through the W-format fix-up and the viewport fold (FM4 Recomp;
// Xenia 67f16c4e315f spirv_shader_translator.cc:1474-1575
// CompleteVertexOrTessEvalShaderInMain, the flags from PA_CL_VTE_CNTL bits 8-10 as
// vulkan_command_processor.cc:3539-3555 sets them): xe_fixup(p) = (xy, z, w) with
//   w  = (xe_flags & kFlagWNotReciprocal) ? p.w : 1.0 / p.w   (the guest exports 1/W unless vtx_w0_fmt)
//   xy = (xe_flags & kFlagXyDividedByW) ? p.xy * w : p.xy      (vtx_xy_fmt: XY were already divided by W)
//   z  = (xe_flags & kFlagZDividedByW) ? p.z * w : p.z         (vtx_z_fmt)
// then gl_Position = vec4(q.xyz * xe_viewport_scale.xyz + xe_viewport_offset.xyz * q.w, q.w) for
// q = xe_fixup(p) (equal to scale * p + offset when q.w == 1). The rect-list expansion mirrors the
// fixed-up corners (Xenia expands rectangles after its vertex shader's epilogue). The host writes
// xe_flags from the draw state (runtime/video/draw_state.h DrawState::systemFlags). Interpolator i
// is `layout(location = i) vec4 xe_interp_<i>` on both sides; the pixel shader's colour 0 is
// `layout(location = 0) out vec4 xe_color0`.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime::shader {

// XeSystem.xe_flags bits (the GLSL spells them as literals: 1u, 2u, 4u). Named after Xenia's
// kSysFlag_XYDividedByW / kSysFlag_ZDividedByW / kSysFlag_WNotReciprocal (spirv_shader_translator.h:94-96;
// Xenia's own bit positions differ and do not matter here). Any other bit must be 0.
constexpr uint32_t kFlagXyDividedByW = 1u << 0;   // PA_CL_VTE_CNTL bit 8 vtx_xy_fmt
constexpr uint32_t kFlagZDividedByW = 1u << 1;    // bit 9 vtx_z_fmt
constexpr uint32_t kFlagWNotReciprocal = 1u << 2;  // bit 10 vtx_w0_fmt

struct ShaderTranslation
{
    std::string glsl;
    // VS: interpolators exported (0..n-1, all four components each). PS: interpolators the
    // shader reads (registers r0..r(n-1) read before written, preloaded from xe_interp_i); the
    // host must check the paired VS exports at least this many.
    uint32_t interpolatorCount = 0;
    std::vector<uint32_t> vertexFetchSlots;  // distinct vertex fetch constant slots (0-95), ascending
    // Per vertex fetch slot (vertexFetchSlots order): the largest stride of its fetches and the
    // largest dword just past an element (offset + components), in dwords. A fetch of vertex index i
    // reads dwords below i * stride + end: the host may copy only [0, high * stride + end) of the
    // slot's data for a draw whose largest index is `high` (shader-computed indices beyond it then
    // fail the bounds check and stop the draw).
    std::vector<uint32_t> vertexFetchStrides, vertexFetchEnds;
    std::vector<uint32_t> textureSlots;      // distinct texture fetch constants (0-31), ascending; FM4
                                             // Recomp: including fetches in loops and bool-constant
                                             // conditionals a draw may skip
    // The magnification and minification filters the shader's fetches of each texture slot set
    // (xenos::TextureFilter: 0 point, 1 linear, 3 the fetch constant's), one entry per slot in
    // textureSlots order: the host samples with these instead of the fetch constant's where they
    // are not 3. Fetches of one slot that disagree are refused.
    // volMag / volMin: the volume filters (0 point, 1 linear, 3 the fetch constant's dword 4 bit 0
    // / bit 1) of a 3D fetch; always 3 for a 2D fetch, where they do not apply. A Vulkan sampler
    // filters all three axes with its mag/min filters, so the host must refuse a 3D slot whose
    // resolved volume filter differs from its resolved mag (volMag) or min (volMin) filter.
    // The translator already refuses an explicit volume filter that differs from an explicit
    // mag/min filter.
    // mip: the instruction's mip filter (0 point, 1 linear, 2 base map, 3 the fetch constant's),
    // which matters for textures with more than one level.
    struct Filters { uint32_t mag = 3, min = 3, volMag = 3, volMin = 3, mip = 3; };
    std::vector<Filters> textureFilters;
    // The dimension each slot's fetches use, one entry per slot in textureSlots order
    // (ucode::kDimension2D = 1: sampler2D; ucode::kDimension3DOrStacked = 2: sampler3D;
    // ucode::kDimensionCube = 3: samplerCube). Fetches of one slot with different dimensions are
    // refused. The host must check the slot's fetch constant: 1 needs dimension 1 (2D, not
    // stacked), 2 needs dimension 2 (3D), 3 a cube map; a stacked 2D texture fetched as 3D (a 2D
    // array on the host) is not supported and must be refused.
    std::vector<uint32_t> textureDimensions;
    std::vector<uint32_t> boolConstants;     // distinct bool constants (0-255) tested by
                                             // conditional execs and conditional jumps,
                                             // ascending (see bindings 2/3)
    std::vector<uint32_t> loopConstants;     // distinct loop constants (0-31) read by loops,
                                             // ascending (see bindings 2/3)
};

// `words` are host-order (already byte-swapped). Returns false with `error` set on anything
// outside the supported subset; `out` is then unspecified.
bool TranslateShader(const uint32_t* words, size_t count, bool pixel, ShaderTranslation& out, std::string& error);

}  // namespace runtime::shader
