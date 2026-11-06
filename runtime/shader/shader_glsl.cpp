// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/gpu/shader_glsl.cpp (GPL-3.0-or-later, same owner),
// reviewed and re-tested here (tests/shader/run_shader_tests.sh). FM4 Recomp changes:
//   - namespace gpu -> runtime::shader, include paths gpu/ -> shader/, the emitted GLSL's second
//     line names FM4 Recomp's file;
//   - a vertex fetch mini whose full fetch sits in another predicated exec is refused (it named
//     that exec's block-scoped index variable, so the GLSL failed to compile, and the mini fetch
//     reused a full fetch that may not have run);
//   - this comment: the subset list now matches what the code accepts (nested loops up to 4 deep,
//     the four PRED_SETs, FRC/TRUNC/FLOOR, CUBEv and cube fetches, setTextureLod, fetches inside a
//     predicated exec), and the ForzaRecomp-only paths are marked as ForzaRecomp's;
//   - texture fetches and getTextureGradients in bool-constant control flow
//     (conditional execs, jump regions), refused in p0-dependent control flow; conditional execs
//     inside a jump's region; a bool-constant jump over a region ending in exec_end; the pixel kill
//     ops 24-27; RECIP_CLAMP (17); MAXAs (23); up to 12 pass dimensions (was 6).
//
// Xenos microcode -> GLSL 450 for FM4's first frames (see shader_glsl.h for the host contract).
//
// Supported subset, everything else refused:
//   control flow  nop, alloc (position / interpolators / colours), exec, exec_end, and the
//                 bool-constant conditional exec of opcode 13 (not _end) holding ALU instructions
//                 that export nothing and texture fetches; loop_start / loop_end (7 / 8) around a body of
//                 exec instructions (no exports or vertex fetches; loops nest up to 4 deep); the program
//                 must end in exec_end before reaching the instruction slots; the bool-constant
//                 cond_jmp (11) forward over nop / exec / predicated exec / opcode-13 conditional exec
//                 instructions only (outside loops, not nested), optionally ending in an unconditional
//                 jump forward (if / else), or (pixel shaders) ending in alloc / exec_end (an early
//                 end of the program where the jump is not taken); the predicated cond_jmp over nop /
//                 exec / predicated exec / conditional exec, optionally with the unconditional jump;
//                 the predicated exec (5, not _end) holding only ALU instructions predicated on its
//                 condition that export nothing, and fetches; at most 12 distinct bool constants,
//                 loops and predicate sets together.
//   vector ALU    ADDv (0), MULv (1), MAXv (2), MINv (3), SETEv (4), SETGTv (5), SETGTEv (6),
//                 SETNEv (7), FRACv (8), TRUNCv (9), FLOORv (10), MULADDv (11), CNDEv (12), CNDGEv (13), CNDGTv (14), DOT4v (15),
//                 DOT3v (16), DOT2ADDv (17), CUBEv (18), KILLEv / KILLGTv / KILLGTEv / KILLNEv (24-27, pixel
//                 shaders).
//   scalar ALU    ADDs (0), ADD_PREVs (1), MULs (2), MUL_PREVs (3), MAXs (5), MINs (6), SETEs (7), SETGTs (8), SETGTEs (9),
//                 SETNEs (10), FRACs (11), TRUNCs (12), FLOORs (13), EXP_IEEE (14), LOG_IEEE (16), RECIP_CLAMP (17),
//                 RECIP_IEEE (19), RECIPSQ_IEEE (22), MAXAs (23, without its a0 write), SUBs (25), SUB_PREVs (26),
//                 PRED_SETEs (27), PRED_SETNEs (28), PRED_SETGTs (29), PRED_SETGTEs (30), SQRT_IEEE (40), MUL_CONST_0/1 (42/43),
//                 ADD_CONST_0/1 (44/45), SUB_CONST_0/1 (46/47), SIN (48), COS (49), RETAIN_PREV (50);
//                 predicated ALU instructions.
//   operands      temp or constant (absolute addressing), abs (temp bit 7, abs_constants),
//                 negate, swizzle; clamp; non-relative destinations; export constant 0 / 1.
//   exports       VS position (62) and PS colour 0, each written in full; interpolators 0-15, whose
//                 unwritten components are 0 (REFERENCE_DERIVED: Xenia zeroes every interpolator
//                 output before the shader runs; on the hardware they are UNKNOWN: APPROXIMATE).
//   vertex fetch  full and mini; formats 8_8_8_8 (6), 2_10_10_10 (7), 10_11_11 (16), 11_11_10
//                 (17), 16_16 (25), 16_16_16_16 (26), 16_16_FLOAT (31), 16_16_16_16_FLOAT (32),
//                 32 (33), 32_32 (34), 32_32_32_32 (35), 32_FLOAT (36), 32_32_FLOAT (37),
//                 32_32_32_32_FLOAT (38), 32_32_32_FLOAT (57); exponent adjust.
//   getTextureGradients (fetch opcode 18) in pixel shaders: coarse ddx/ddy of the source.
//   setTextureLod (fetch opcode 24): the register LOD.
//   texture fetch 2D, 3D and cube, normalized coordinates, computed, explicit 0 or register LOD,
//                 point / linear / fetch constant filters, half-texel offsets (not on cubes).
//   fetches in a predicated exec: unpredicated or predicated on the exec's condition (see
//                 VertexFetchOp and TextureFetchOp).
//   texture fetches and getTextureGradients in bool-constant conditional execs and jump regions
//                 (uniform control flow: see "Uniform control flow" below); not in a predicated
//                 jump's region.
// The other predicate-setting ops (vector 20-23, scalar 31-34), the other predicated fetches and
// all predicated exports, relative addressing (so aL and a0 are never read), calls, other
// unconditional jumps, backward, in-loop and nested jumps, the other conditional execs (3, 4, 6,
// 14), loops nested deeper than 4, loop_start reusing aL and loop_end's predicated break are
// refused.
//
// Semantics, labels and sources. [X] = Xenia ucode.h / xenos.h (xenia-canary 67f16c4e315f,
// BSD-3-Clause, see NOTICE); re-derived here, not copied. Nothing is VERIFIED:
// there is no hardware oracle for these.
//   ADDv a + b; MULv a * b; DOT2ADDv a.x*b.x + a.y*b.y + c.x, summed in that order; MAXv
//     a >= b ? a : b per component (so MAX(r, r) is an exact move, NaN included)
//     REFERENCE_DERIVED [X] (DOT2ADD operand order: [X] citing MSDN bb313922 and XNA).
//   SETEv / SETGTv / SETGTEv / SETNEv a == / > / >= / != b ? 1 : 0; CNDEv / CNDGEv / CNDGTv
//     a == / >= / > 0 ? b : c (operands src1, src2, src3); MINv a < b ? a : b; all per component,
//     ordered comparisons (NaN fails them) except != (true for NaN, as C's and Xenia's); -0 == 0; MULADDv a * b + c; DOT4v
//     ((a.x*b.x + a.y*b.y) + a.z*b.z) + a.w*b.w. REFERENCE_DERIVED [X] (ucode.h definitions and
//     its SPIR-V translator). Summation order: [X] and MSDN sum XYZW, while [X] reports the
//     IPR2015-00325 sq_alu model sums WXYZ; the order changes only rounding: APPROXIMATE.
//     MULADDv is not fused (a rounded product, then the sum): [X]'s conventions (D3D9 mad is
//     v_mad_legacy, not fma) and its translators; [X] also cites sq_alu as fused, so the
//     rounding of the sum is APPROXIMATE.
//   Multiplication follows the Direct3D 9 rule [X] (R5xx Acceleration 8.7.5): +-0 times anything
//     (Inf and NaN included) is +0; REFERENCE_DERIVED. Denormal operands, which [X] also treats as
//     zero, depend on the host's denormal mode: APPROXIMATE. Products and sums are `precise` so
//     the host compiler cannot fuse them ([X]: multiply-add is not fused).
//   Scalar operands of src3: a = component (3 + swz[6:7]) & 3, b = component swz[0:1] [X].
//   DOT3v (a.x*b.x + a.y*b.y) + a.z*b.z, products by the multiply rule below: REFERENCE_DERIVED [X]
//     (ucode.h, SPIR-V translator); the summation order is APPROXIMATE, as DOT4v's.
//   ADDs a + b; MULs a * b (the multiply rule above); SUBs a - b; MAXs a >= b ? a : b (MAXs(a, a) is an exact move); MINs a < b ? a : b; SETEs /
//     SETGTs / SETGTEs / SETNEs a == / > / >= / != 0 ? 1 : 0 (ordered, != true for NaN).
//     REFERENCE_DERIVED [X] (ucode.h definitions).
//   EXP_IEEE 2^a with a == 0 -> 1; LOG_IEEE log2(a) with a == 1 -> 0: REFERENCE_DERIVED [X]. The
//     other IEEE special cases are spelled out because GLSL leaves them undefined: log2 0 = -Inf
//     (INFERRED: [X]'s LOG_CLAMP maps LOG's -Inf to -FLT_MAX), log2 of a negative = NaN, 2^-Inf
//     = 0, +Inf and NaN pass through (IEEE 754, INFERRED); the NaN's bits are APPROXIMATE.
//     Precision is the host's exp2 / log2: APPROXIMATE (no oracle for the Xenos's tables).
//   RECIP_IEEE 1 / a with a == 1 -> 1: REFERENCE_DERIVED [X] (ucode.h; its SPIR-V translator
//     divides with no clamp or flush: the clamping reciprocals are RECIP_CLAMP / RECIP_FF, 17 /
//     18). GLSL leaves division by 0 or Inf undefined, so the IEEE 754 cases are spelled out:
//     1 / +-0 = +-Inf, 1 / +-Inf = +-0, NaN passes through (INFERRED from the IEEE name and
//     [X]'s plain division; the NaN's bits are APPROXIMATE). Other values: the host's division
//     (2.5 ULP in Vulkan), APPROXIMATE; denormal operands and results as the host's denormal
//     mode, APPROXIMATE.
//   SQRT_IEEE sqrt(a); ADD_CONST_0/1 c.a + r.b and MUL_CONST_0/1 c.a * r.b (the multiply rule
//     below) with the constant from src3's index, a = its component (3 + swz[6:7]) & 3, the
//     temp index (opcode & 1) | (src3_sel << 1) | (swz & 0x3C) and b = component swz[0:1];
//     abs_constants and src3's negate apply to both. Scalar results replicate into every masked
//     component. REFERENCE_DERIVED [X].
//   Previous scalar result ps ([X] ucode.h: "ps is the previous result of a scalar ALU
//     operation"; every scalar op except RETAIN_PREV modifies it, whether or not it writes a
//     register; an instruction predicated off does not modify it, [X] ucode.h). MUL_PREVs a * ps (the multiply rule;
//     a = component (3 + swz[6:7]) & 3 of src3), ADD_PREVs a + ps, SUB_PREVs a - ps (Xenia ucode.h kAddsPrev
//     "dest.xyzw = src0.a + ps", kSubsPrev "src0.a - ps", as ported in ReCarbon
//     runtime/gpu/xenos/ucode.h; REFERENCE_DERIVED); RETAIN_PREV writes ps to its masked components
//     and leaves ps unchanged. ps holds the result before the scalar clamp (Xenia stores it
//     before applying the clamp to the destination; on hardware UNKNOWN). It persists across
//     ALU instructions, fetches, exec boundaries, conditional execs, jumps and loop iterations
//     like a register (Xenia keeps one function-scope variable). REFERENCE_DERIVED [X] (ucode.h
//     AluScalarOpcodeInfo, SpirvShaderTranslator::ProcessAluInstruction). Its initial value is
//     UNKNOWN ([X] uses 0): a read on any path (bool / loop assignment) before a scalar op has
//     run is refused, checked like register definedness. Programs that never read ps (no
//     *_PREV op, no RETAIN_PREV that writes) emit exactly as before: there a scalar op with an
//     empty mask has no observable effect and is skipped. MUL_PREV2s is refused
//     (not needed yet).
//   Clamp: to [0, 1], NaN -> 0 (REFERENCE_DERIVED [X]: Xenia clamps with NaN to 0).
//   Co-issue: the vector and scalar ops read their operands before either writes (REFERENCE_
//     DERIVED [X]).
//   Export masks (vector / scalar / constant 1 / constant 0): ucode.h's helpers, [X].
//   Vertex fetch: index = floor(r.c) (floor(r.c + 0.5) when rounded, INFERRED in [X]); element
//     address = base(slot) + index * stride + offset dwords; a mini fetch reuses the last full
//     fetch's slot, index and stride with its own offset and format; each dword is byte-swapped by
//     the fetch constant's endian (1 8in16, 2 8in32, 3 16in32), then unpacked. REFERENCE_DERIVED
//     [X] (xenos.h, spirv_shader_translator_fetch.cc) throughout unless labelled otherwise:
//     - Every dword of the element is read (and range checked): 1 for 6, 7, 16, 17, 25, 31, 33,
//       36; 2 for 26, 32, 34, 37; 3 for 57; 4 for 35, 38.
//     - 32-bit float formats: dword c is component c as an IEEE float.
//     - Packed formats: component x occupies the lowest bits of the swapped dword, the format's
//       name lists the widths from the most significant bits down: 8_8_8_8 x 0-7, y 8-15, z
//       16-23, w 24-31; 2_10_10_10 x 0-9, y 10-19, z 20-29, w 30-31; 10_11_11 x 0-10, y 11-21, z
//       22-31; 11_11_10 x 0-9, y 10-20, z 21-31; 16_16 x 0-15, y 16-31; 16_16_16_16 x, y in dword
//       0 and z, w in dword 1 the same way; 32, 32_32, 32_32_32_32 one dword per component. So
//       the endian mode decides which bytes form x: 8in16 swaps the bytes within each 16-bit
//       half, 8in32 reverses all four, so the same memory puts a different half in x.
//       format_comp_all (word1 bit 12) set: the field is sign-extended two's complement, else
//       zero-extended. num_format_all (bit 13) set: the integer value as a float; clear
//       (normalized): unsigned v * (1 / (2^n - 1)), signed max(v * (1 / (2^(n-1) - 1)), -1.0)
//       (signed repeating-fraction mode 0, [X] kZeroClampMinusOne: -2^(n-1) and -(2^(n-1) - 1)
//       both read -1, the Direct3D SNORM convention). The reciprocal is the float32 nearest to it
//       and the product is rounded once (Vulkan's multiplication is correctly rounded; a division
//       is only 2.5 ULP). This is [X]'s computation; it differs from the correctly rounded
//       quotient v / (2^n - 1) by 1 ULP for some v (e.g. 3 / 255): APPROXIMATE, the hardware's
//       rounding is UNKNOWN. For 32-bit fields the integer -> float conversion rounds (|v| >
//       2^24) as the host's OpConvertUToF / OpConvertSToF (INFERRED). Signed normalized mode 1
//       ([X] kNoZero) is refused; for unsigned or integer fields bit 14 has no effect ([X]).
//     - 16-bit float formats: halves split as 16_16 / 16_16_16_16, each decoded exactly by
//       xe_half with integer operations (no float16 conversion, so no host denormal flushing):
//       exponents 0-30 as IEEE 754 binary16 (subnormals included); that Xenos halves follow
//       IEEE there is REFERENCE_DERIVED [X]. Exponent 31 is UNKNOWN ([X] decodes Inf / NaN with
//       unpackHalf2x16 and notes that the Xbox 360 may use an extended range instead): it sets
//       xe_error bit 1 for the host to stop on and reads 0.
//     - The sign / normalization flags do not apply to float formats.
//     - exp_adjust (word1 bits 24-29, signed): the result is multiplied by 2^exp_adjust after
//       normalization, for every format (exact unless it overflows or becomes subnormal).
//     A destination component that selects an element component the format lacks is refused
//     (what the hardware returns there is not established here).
//   Texture fetch: texture(sampler, vec2(src.c0, src.c1)) (2D) or texture(sampler, vec3(src.c0,
//     src.c1, src.c2)) (dimension 2, a sampler3D: U, V, W normalized [X] ucode.h) with the implicit
//     LOD; filters, LOD bias, clamps and the format come from the fetch constant through the
//     host's sampler and image. A 3D fetch's volume filters (word1 24-25, 26-27) choose the
//     filter along W [X]; Xenia applies them only between the layers of a stacked texture and
//     filters a real 3D texture with the sampler's mag/min (REFERENCE_DERIVED [X]
//     spirv_shader_translator_fetch.cc), so here they must equal mag/min: explicit values that
//     differ are refused, the rest is reported for the host to check (APPROXIMATE only in that
//     the hardware's separate W filter is not modelled; equal filters filter W the same way). Xenos normalized coordinates use the same texel-centre convention as Vulkan (Xenia
//     samples them directly): REFERENCE_DERIVED [X]. fetch_valid_only and the sample location
//     (centroid / centre) are accepted and ignored: their meaning is UNKNOWN; with single-sample
//     targets the sample location cannot change the result (INFERRED).
//   Conditional exec, opcode 13 (kCondExecPredClean): [X] establishes (against XNA's
//     disassembler) that it is the bool-constant cexec with word1 bits 2-9 the constant and bit
//     10 the condition (its bit 9 is the constant's MSB, not a predicate-clean bit); it runs its
//     instructions when bit (b % 32) of bool dword b / 32 equals the condition. The predicate is
//     never read or written by a translated program, so the clean hint has nothing to act on.
//     REFERENCE_DERIVED [X] (ucode.h, SpirvShaderTranslator::UpdateExecConditionals). Bool
//     constants are uniform per draw, so register definedness and the PS interpolator count are
//     checked on every assignment of the tested bools (with loops and predicate sets, up to 2^12
//     walks: FM4's largest accepted pixel shader, 11 dimensions, takes ~0.1 s); exports are
//     refused inside a conditional exec, so they do not depend on the path. Fetches inside one are
//     accepted (FM4 Recomp; see "Uniform control flow"): a texture slot fetched only there is still
//     listed in textureSlots, so the host may validate a texture the draw never samples
//     (fail-closed, not a wrong result), as for a loop body.
//   Uniform control flow (FM4 Recomp). The bool constants (xe_bool) and loop constants (xe_loop)
//     come from the uniform block: one value for every invocation of a draw, as Xenia's
//     uniform_bool_loop_constants_ (Uniform storage class; spirv_shader_translator.cc), so a
//     conditional exec on a bool, a bool-constant jump's region and a loop are dynamically uniform
//     control flow (GLSL 4.50 "Dynamically Uniform Expressions": uniform-block values are), where
//     implicit derivatives are defined (GLSL 4.50 texture functions: undefined only in non-uniform
//     control flow; the same rule in Vulkan's derivative operations). A texture
//     fetch (implicit LOD) or getTextureGradients there is emitted in place. p0 is per invocation:
//     inside a predicated exec a pixel shader's texture fetch is hoisted out of the exec's `if` (see
//     TextureFetchOp; also inside a bool jump's region, where the hoisted sample stays in the bool
//     `if`), and inside a predicated jump's region (whose registers may have been written under p0
//     before the fetch) texture fetches and getTextureGradients are refused. Xenia itself does
//     nothing special here: its translator takes OpDPdxCoarse / OpDPdyCoarse of the coordinates at
//     the fetch, wherever it sits, and samples with those gradients
//     (spirv_shader_translator_fetch.cc, ProcessTextureFetchInstruction; coarse derivatives in
//     divergent control flow are undefined in Vulkan), while noting that the implicit-derivative
//     `lod` query "can't be used when there's control flow divergence" (its getWeights comment);
//     this unit keeps the uniformity requirement instead. Hardware derivatives in divergent quads:
//     UNKNOWN.
//   Loops (opcodes 7 / 8): addresses are control-flow instruction indices. loop_start reads loop
//     constant L (word0 bits 16-20) = register SHADER_CONSTANT_LOOP_00 + L, whose bits 0-7 are
//     the unsigned iteration count (8-15 aL start, 16-23 signed aL step); a count of 0 jumps to
//     its address, past the loop. loop_end decrements the count and jumps back to its address
//     (the body) while it is not 0. So the body runs exactly `count` times, 0-255; the count is
//     read once at loop_start and emitted as a `for` bounded by it (a malformed constant cannot
//     hang the GPU). REFERENCE_DERIVED [X] (xenos.h LoopConstant; ucode.h loop instructions;
//     SpirvShaderTranslator::ProcessLoopStart/EndInstruction). Only the shape a compiler emits is
//     accepted: the skip address is the instruction after the matching loop_end, whose address is
//     the instruction after the loop_start and whose loop constant is the same. aL (start, step,
//     is_repeat) only feeds relative addressing, which is refused, so it is not modelled and
//     is_repeat is refused. loop_end word0 bit 21 is the predicated break (refused); its word1
//     bit 10 is the break's condition, read by [X] only with bit 21 set, so it is ignored
//     without it. Address mode (word1 bit 11) set is refused (meaning UNKNOWN). The loop
//     constants are uniform per draw, so each loop adds a pass dimension like a bool constant:
//     every analysis either skips its body (count 0) or walks it once from the pre-loop state;
//     iteration 1 sees the pre-loop registers and later iterations a superset, so one walk
//     checks every read in the body. A register first written in the body and read after the
//     loop is thus refused in a VS (undefined on the zero-trip path) and makes a PS read the
//     interpolator preload, as after a conditional exec. The trip count comes from the uniform
//     block, so the `for` is uniform control flow and the implicit LOD of a texture fetch in the
//     body stays defined (Vulkan leaves it undefined under non-uniform flow). A texture fetch in the body lists its
//     slot even when a draw's count is 0 (the host may then validate a texture the hardware
//     never samples: fail-closed, not a wrong result).
//   cond_jmp (opcode 11): word0 bits 0-12 the target control-flow instruction index, bit 13
//     unconditional, bit 14 predicated; word1 bits 2-9 the bool constant, bit 10 the condition.
//     Taken when bit (b % 32) of bool dword b / 32 equals the condition, as the bool-constant
//     cexec runs (REFERENCE_DERIVED [X]: ucode.h ControlFlowCondJmpInstruction,
//     ParseControlFlowCondJmp, SpirvShaderTranslator::ProcessJumpInstruction; the target is a
//     control-flow index as loop addresses are). Only a forward jump outside loops whose skipped
//     region holds nop and exec instructions is accepted, emitted as `if (bit != condition)
//     { region }`; the region is a conditional for exports and fetches (refused there), and the
//     jump's bool joins the analysis passes like a cexec's (the taken pass resumes at the
//     target). Unconditional and predicated jumps, the address mode bit (11) and word1 bit 1
//     (named "direction" by [X] and [F], read by neither; meaning UNKNOWN) are refused.
//     FM4 Recomp: the region may hold opcode-13 conditional execs (nested `if`s; the region stays
//     conditional after them) and, in a bool-constant jump's region, texture fetches. A region whose
//     last instruction is exec_end (it may also hold alloc) ends the program where the jump is not
//     taken ([X] ucode.h: exec_end ends the shader); it is emitted as `if (!taken) { region } else
//     { rest of the program }`, so both paths reach main's one epilogue (kill, alpha test, colour
//     conversions), as Xenia's translator leaves its main loop for its single epilogue; the
//     region's own execs may export (each analysis pass checks its path's colour 0). Pixel shaders
//     and bool-constant jumps only.
//   PWL gamma (the host's choice, as Xenia with gamma_render_target_as_srgb off: UNORM host
//     formats, the curve applied in the shader). Segments, 8-bit gamma k / 10-bit linear n:
//       k 0-63: n = k;  64-95: n = 2k - 64;  96-191: n = 4k - 256 + (4k - 256) / 256;
//       192-255: n = 8k - 1024 + (8k - 1024) / 128;  inverse n 0-63: k = n;  64-127: n / 2 + 32;
//       128-511: n / 4 + 64;  512-1023: n / 8 + 128 (divisions truncate).
//     xe_pwl_gamma_to_linear / xe_linear_to_pwl_gamma follow Xenia's float form (xenos.cc
//     PWLGammaToLinear / LinearToPWLGamma and its SPIR-V versions: saturate with NaN -> 0,
//     `>=` thresholds, gamma * (255 * 1024) * scale + offset plus trunc(linear * scale), times
//     1/1023; trunc(linear * scale) / 255 + offset), as ForzaRecomp runtime/gpu/edram.h does on the CPU:
//     REFERENCE_DERIVED [X] (after the Source Engine's X360GammaToLinear / X360LinearToGamma).
//     Like Xenia's float form, a few gamma bytes land just below n / 1023 (ForzaRecomp edram.h).
//     Texture: with XE_TEXTURE_<slot>_GAMMA defined, the fetch result's x, y, z (not w) are
//     linearised after the host's UNORM filtering; the hardware linearises texels before
//     filtering, so filtered values are APPROXIMATE (exact for single-texel point samples).
//     Colour 0: with XE_COLOR0_GAMMA defined, xyz are encoded at the end of main (the curve
//     saturates, as the 8-bit target would); w is unchanged. Xenia does the same in its
//     host-render-target path and notes the hardware encodes after blending, so blending with
//     a gamma target operates on encoded values here: APPROXIMATE.
//     Colour 0: with XE_COLOR0_7E3 defined (a k_2_10_10_10_FLOAT target), RGB are clamped to
//     [0, 31.875] and alpha to [0, 1], NaN and -0 to 0 (REFERENCE_DERIVED: Xenia's
//     render_target_cache.cc clamps shader output for these targets, so blending sees clamped
//     values; the guards keep NaN and -0 out, which Xenia's NClamp may not).
//   Predicate p0 (one bit per invocation, emitted as `bool xe_p0`). PRED_SETEs: p0 = a == 0
//     (ordered: NaN gives 0), result 0.0 where p0 is set else 1.0; PRED_SETNEs: p0 = a != 0 (true
//     for NaN, [X]'s OpFUnordNotEqual), result likewise; a = component (3 + swz[6:7]) & 3 of src3.
//     They always run (they write p0 even with an empty mask) and, like every scalar op, set ps.
//     REFERENCE_DERIVED [X] (ucode.h kSetpEq / kSetpNe; SpirvShaderTranslator::
//     ProcessScalarAluOperation). p0's initial value is UNKNOWN ([X] initialises it to false
//     "for safety"): a predicated instruction, exec or jump reached on any path before a
//     predicate set has run is refused. p0 persists across instructions, execs and jumps (one
//     function-scope variable, as [X]).
//     Predicated ALU instruction (word1 bit 28, condition bit 27): both the vector and the scalar
//     op run, read operands and write (registers, ps, p0) only where p0 equals the condition;
//     emitted inside `if`. REFERENCE_DERIVED [X] (ucode.h on ps; UpdateInstructionPredication).
//     Predicated exports and fetches are refused.
//     Predicated exec (opcode 5; word1 bit 10 the condition, bit 9 predicate clean): per [X]
//     (citing the IPR2015-00325 sequencer specification) the hardware enters it for a whole
//     group of invocations when any of them passes, and the compiler therefore predicates every
//     instruction inside on the same condition. Only that shape is accepted (every instruction
//     an ALU instruction predicated on the exec's condition, no exports, no predicate set
//     inside), so the result per invocation is exactly "runs where p0 equals the condition",
//     whatever the grouping: REFERENCE_DERIVED [X]. Emitted as one `if` around the exec.
//     Predicate clean (word1 bit 9 of exec / exec_end / predicated exec; implied for opcode 13):
//     per [X] the compiler clears it when the exec modifies p0 and the sequencer then does not
//     wait for p0 updates; a predicate set inside an exec that has it set is refused (when the
//     new p0 is seen is UNKNOWN). Clear on an exec that sets nothing it is only a stall hint.
//     Predicated cond_jmp (word0 bit 14; word1 bit 10 the condition): taken where p0 equals the
//     condition, emitted per invocation as `if (p0 != condition) { region }`, as [X]
//     (ProcessJumpInstruction treats it as a predicated exec). APPROXIMATE: a jump moves a whole
//     group's control flow, and what the hardware does when the group's p0 values differ is
//     UNKNOWN ([X] documents the any / all rule for execs only); per-invocation semantics are
//     the program's intent and exact when p0 is uniform over the group. Its bool constant
//     field must be 0 (meaning UNKNOWN otherwise).
//     Unconditional cond_jmp (word0 bit 13; [X]: ignores condition and predication): accepted
//     only as the last instruction of a conditional jump's region, forward, with the predicate,
//     bool and condition fields 0; the instructions it skips run where the first jump was
//     taken: `if (!taken) { region } else { skipped }`. REFERENCE_DERIVED [X].
//     Analysis: p0 is data-dependent, so each predicate set adds a pass dimension (its p0 is
//     free; correlations between sets are not modelled, so passes over-approximate the paths
//     and every check stays sound). A predicate set inside a loop (one value per iteration) is
//     refused.
//   Register state: the VS gets r0 = (vertex index, 0, 0, 0) as floats [X] for the index (the
//     rest INFERRED); reading any other never-written temp component in a VS is refused. A PS
//     gets r0..r(n-1) = interpolators 0..n-1, n = 1 + the highest register it reads before
//     writing (INFERRED: the count lives in SQ_PROGRAM_CNTL, which the host must check).
#include "shader/shader_glsl.h"
#include "shader/ucode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace runtime::shader {
namespace {

using namespace ucode;

constexpr const char* kComponents = "xyzw";
// Distinct bool constants a program may test: each doubles the analysis passes.
constexpr size_t kMaxBools = 12;

std::string Num(uint32_t v) { return std::to_string(v); }

// Element layout of a vertex format [X] (spirv_shader_translator_fetch.cc, xenos.h): dwords per
// element and, for packed formats, each component's dword, bit offset and width (x lowest; the
// format's name lists the components from the most significant bits down).
struct VertexLayout
{
    enum Kind { Float32, Half, Packed } kind;
    uint32_t words;
    uint8_t word[4], offset[4], width[4];
};
bool GetVertexLayout(uint32_t format, VertexLayout& l)
{
    switch (VertexFormat(format))
    {
    case VertexFormat::F32_FLOAT: l = {VertexLayout::Float32, 1, {}, {}, {}}; return true;
    case VertexFormat::F32_32_FLOAT: l = {VertexLayout::Float32, 2, {}, {}, {}}; return true;
    case VertexFormat::F32_32_32_FLOAT: l = {VertexLayout::Float32, 3, {}, {}, {}}; return true;
    case VertexFormat::F32_32_32_32_FLOAT: l = {VertexLayout::Float32, 4, {}, {}, {}}; return true;
    case VertexFormat::F16_16_FLOAT: l = {VertexLayout::Half, 1, {0, 0}, {0, 16}, {16, 16}}; return true;
    case VertexFormat::F16_16_16_16_FLOAT: l = {VertexLayout::Half, 2, {0, 0, 1, 1}, {0, 16, 0, 16}, {16, 16, 16, 16}}; return true;
    case VertexFormat::F8_8_8_8: l = {VertexLayout::Packed, 1, {0, 0, 0, 0}, {0, 8, 16, 24}, {8, 8, 8, 8}}; return true;
    case VertexFormat::F2_10_10_10: l = {VertexLayout::Packed, 1, {0, 0, 0, 0}, {0, 10, 20, 30}, {10, 10, 10, 2}}; return true;
    case VertexFormat::F10_11_11: l = {VertexLayout::Packed, 1, {0, 0, 0}, {0, 11, 22}, {11, 11, 10}}; return true;
    case VertexFormat::F11_11_10: l = {VertexLayout::Packed, 1, {0, 0, 0}, {0, 10, 21}, {10, 11, 11}}; return true;
    case VertexFormat::F16_16: l = {VertexLayout::Packed, 1, {0, 0}, {0, 16}, {16, 16}}; return true;
    case VertexFormat::F16_16_16_16: l = {VertexLayout::Packed, 2, {0, 0, 1, 1}, {0, 16, 0, 16}, {16, 16, 16, 16}}; return true;
    case VertexFormat::F32: l = {VertexLayout::Packed, 1, {0}, {0}, {32}}; return true;
    case VertexFormat::F32_32: l = {VertexLayout::Packed, 2, {0, 1}, {0, 0}, {32, 32}}; return true;
    case VertexFormat::F32_32_32_32: l = {VertexLayout::Packed, 4, {0, 1, 2, 3}, {0, 0, 0, 0}, {32, 32, 32, 32}}; return true;
    default: return false;
    }
}

// `uintBitsToFloat(0x...u)`: an exact float32 constant (a literal quotient could be folded in
// another precision).
std::string FloatBits(float v)
{
    uint32_t b;
    std::memcpy(&b, &v, 4);
    char s[40];
    std::snprintf(s, sizeof s, "uintBitsToFloat(0x%08Xu)", b);
    return s;
}

const char* CfName(CfOpcode op)
{
    switch (op)
    {
    case CfOpcode::CondExec: case CfOpcode::CondExecEnd: case CfOpcode::CondExecPredClean:
    case CfOpcode::CondExecPredCleanEnd: return "conditional exec";
    case CfOpcode::CondExecPred: case CfOpcode::CondExecPredEnd: return "predicated exec";
    case CfOpcode::LoopStart: case CfOpcode::LoopEnd: return "loop";
    case CfOpcode::CondCall: return "call";
    case CfOpcode::Return: return "return";
    case CfOpcode::CondJmp: return "jump";
    case CfOpcode::MarkVsFetchDone: return "vertex fetch done mark";
    case CfOpcode::Alloc: return "alloc";
    default: return "control flow";
    }
}

// Scalar ops that read the previous scalar result ps.
bool ReadsPs(const AluInstruction& a)
{
    switch (ScalarOpcode(a.scalarOpcode))
    {
    case ScalarOpcode::AddsPrev: case ScalarOpcode::MulsPrev: case ScalarOpcode::MulsPrev2: case ScalarOpcode::SubsPrev:
        return true;
    case ScalarOpcode::RetainPrev: return ScalarResultMask(a) != 0;
    default: return false;
    }
}

// Whether any ALU instruction an exec of the program addresses reads ps (a superset of what a
// walk executes; Run refuses malformed programs).
bool ProgramReadsPs(const uint32_t* words, uint32_t count)
{
    uint32_t firstInstruction = ~0u;
    CfWords cf;
    for (uint32_t i = 0; i / 2 < firstInstruction && ReadCf(words, count, i, cf); i++)
    {
        if (!IsExec(CfOpcodeOf(cf)))
            continue;
        const CfExec e = DecodeExec(cf);
        firstInstruction = std::min(firstInstruction, e.address);
        const uint32_t* w;
        for (uint32_t k = 0; k < e.count && ReadSlot(words, count, e.address + k, w); k++)
            if (!IsFetch(e, k) && ReadsPs(DecodeAlu(w)))
                return true;
    }
    return false;
}

// One walk of the control-flow program. Emit walks every exec, emitting conditional execs as
// `if` blocks; its register definedness is the union over all paths, so it refuses no read a
// path defines. Analyze walks only the execs taken for one assignment of the bool constants the
// program tests and checks definedness (and the PS interpolator count) and exports on that path.
enum class Mode
{
    Emit,
    Analyze,
};

class Translator
{
public:
    Translator(const uint32_t* words, uint32_t count, bool pixel, Mode mode, const std::vector<uint32_t>& bools = {},
               const std::vector<uint32_t>& loops = {}, const std::vector<uint32_t>& predSets = {}, uint32_t assignment = 0)
        : words_(words), count_(count), pixel_(pixel), mode_(mode), bools_(bools), loops_(loops), predSets_(predSets),
          assignment_(assignment), readsPs_(ProgramReadsPs(words, count))
    {
        if (!pixel_)
            written_[0] = 1;  // r0.x = vertex index
    }

    bool Run(std::string& error);
    // Emit only: the program's GLSL, given the analysis passes' register results.
    void Finish(const std::vector<Translator>& analyses, ShaderTranslation& out);
    const std::vector<uint32_t>& Bools() const { return bools_; }
    const std::vector<uint32_t>& Loops() const { return loops_; }
    const std::vector<uint32_t>& PredSets() const { return predSets_; }
    size_t Dimensions() const { return bools_.size() + loops_.size() + predSets_.size(); }

private:
    const uint32_t* words_;
    uint32_t count_;
    bool pixel_;
    Mode mode_;
    std::vector<uint32_t> bools_;  // Emit: collected; Analyze: assignment_ bit i is bools_[i]'s value
    // The loop constant of each loop, in control-flow order. Analyze: assignment_ bit
    // bools_.size() + k set means loop k runs its body (count >= 1), clear that it is skipped.
    std::vector<uint32_t> loops_;
    // The predicate-setting instructions (key: control-flow index * 8 + position in the exec),
    // in control-flow order. Analyze: assignment_ bit bools_.size() + loops_.size() + k is the
    // p0 value predSets_[k] produces.
    std::vector<uint32_t> predSets_;
    uint32_t assignment_;
    // The program reads ps: every scalar op but RETAIN_PREV then runs and updates xe_ps.
    bool readsPs_;
    bool psWritten_ = false;  // a scalar op has run (Emit: on some path; Analyze: on this one)
    bool predWritten_ = false;  // p0 has been set (Emit: on some path; Analyze: on this one)
    bool lodWritten_ = false;   // setTextureLod has set the register LOD (xe_lod) in program order
    bool pred_ = false;         // Analyze: p0's value on this path
    bool inPredExec_ = false, predExecCondition_ = false;  // inside a predicated exec on p0 == condition
    bool execPredClean_ = false;  // the current exec claims not to modify p0
    bool inElse_ = false;         // in the region an unconditional jump at a jump region's end skips
    uint32_t instrKey_ = 0;       // control-flow index * 8 + position of the current instruction
    bool inConditional_ = false;
    bool inCondExec_ = false;  // inside a bool-constant conditional exec (opcode 13)
    bool inLoop_ = false;
    bool inJump_ = false;     // inside a cond_jmp's skipped region, which ends at jumpEnd_
    bool inPredJump_ = false; // that region (or its else) depends on p0: a predicated cond_jmp's
    // FM4 Recomp: the current bool-constant jump region ends in exec_end (an early end of the
    // program on the path that does not jump); Emit then puts the rest of the program in an `else`
    // that closes at the program's end, so both paths reach main's one epilogue.
    bool terminalRegion_ = false;
    uint32_t elsesToEnd_ = 0;  // Emit: `else` blocks opened at terminal regions' ends, still open
    bool kills_ = false;       // a kill op was emitted: xe_kill is declared and tested at main's end
    uint32_t jumpEnd_ = 0;
    bool needDp3_ = false, needRcp_ = false, needRcpc_ = false, needHalf_ = false, needCube_ = false, needCubeDir_ = false;  // helpers emitted only when used
    uint32_t loopOrdinal_ = 0;          // loops walked so far
    std::vector<uint32_t> loopEnds_;    // the open loops' loop_end indices, innermost last (nesting up to kMaxLoopDepth)
    static constexpr size_t kMaxLoopDepth = 4;
    std::string indent_;
    uint32_t interpolators_ = 0;
    std::string body_, error_;
    uint8_t written_[64] = {};   // temp components holding a defined value
    uint64_t usedTemps_ = 0;
    uint32_t preloadCount_ = 0;  // PS: interpolators read
    uint8_t exported_[64] = {};  // export components written, by export register
    std::set<uint32_t> vertexSlots_, textureSlots_;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> vertexExtents_;  // slot -> (largest stride, largest element end), dwords
    std::map<uint32_t, ShaderTranslation::Filters> textureFilters_;  // per slot: the instruction filters
    std::map<uint32_t, uint32_t> textureDimensions_;                  // per slot: kDimension2D or 3DOrStacked
    bool haveFullFetch_ = false;
    // The control-flow index of the predicated exec holding the last full vertex fetch, or ~0u
    // outside one (FM4 Recomp: mini fetches must not reach into another exec's block).
    uint32_t fullFetchExec_ = ~0u;
    uint32_t fullSlot_ = 0, fullStride_ = 0, fetchId_ = 0, aluId_ = 0;
    std::string fullIndex_;

    bool Fail(const std::string& message)
    {
        if (error_.empty())
            error_ = message;
        return false;
    }
    void Line(const std::string& s) { body_ += "    " + indent_ + s + "\n"; }
    const char* Conditional() const
    {
        return inPredExec_   ? "predicated exec"
               : inCondExec_ ? "conditional exec"
               : inPredJump_ ? "predicated jump's skipped region"
                             : "jump's skipped region";
    }
    // Control flow that may differ between the invocations of a draw: a predicated exec or a
    // predicated jump's region (p0 is per invocation). Bool-constant conditional execs, bool-constant
    // jump regions and loops branch on the uniform block (xe_bool, xe_loop), the same value for
    // every invocation of a draw, so they stay uniform control flow (see the header comment).
    bool Divergent() const { return inPredExec_ || inPredJump_; }

    // Records a read of temp `reg` components `mask` (bit 0 = x); fails on undefined data.
    bool ReadTemp(uint32_t reg, uint32_t mask)
    {
        usedTemps_ |= uint64_t(1) << reg;
        const uint32_t missing = mask & ~written_[reg];
        if (!missing)
            return true;
        if (pixel_)
        {
            if (reg >= 16)
                return Fail("pixel shader reads r" + Num(reg) + " before writing it, beyond the 16 interpolators");
            preloadCount_ = std::max(preloadCount_, reg + 1);
            for (uint32_t r = 0; r < preloadCount_; r++)
                written_[r] = 0xF, usedTemps_ |= uint64_t(1) << r;
            return true;
        }
        uint32_t c = 0;
        while (!(missing >> c & 1))
            c++;
        return Fail("vertex shader reads r" + Num(reg) + "." + kComponents[c] + " before writing it");
    }
    void WriteTemp(uint32_t reg, uint32_t mask)
    {
        usedTemps_ |= uint64_t(1) << reg;
        written_[reg] |= uint8_t(mask);
    }

    bool Exec(const CfExec& e, uint32_t index);
    bool Alu(const uint32_t w[3], uint32_t slot);
    bool AluOps(const AluInstruction& a, const std::string& where);
    bool PredicateSet(const AluInstruction& a, const std::string& where, std::string& operand);
    bool ReadPredicate(const std::string& what, const std::string& where);
    static std::string PredicateTest(bool condition) { return condition ? "if (xe_p0)" : "if (!xe_p0)"; }
    bool VectorOperand(const AluInstruction& a, uint32_t n, uint32_t positions, std::string& out);
    bool ScalarOperand(const AluInstruction& a, uint32_t position, std::string& out);
    bool VertexFetchOp(const uint32_t w[3], uint32_t slot);
    bool TextureFetchOp(const uint32_t w[3], uint32_t slot);
    bool TextureGradientsOp(const uint32_t w[3], uint32_t slot);
    bool SetTextureLodOp(const uint32_t w[3], uint32_t slot);
    bool StoreFetch(uint32_t dest, uint32_t destSwizzle, uint32_t available, const std::string& value,
                    const char* what, uint32_t slot);
    bool BoolTaken(uint32_t b, bool condition);
    bool AddBool(uint32_t b, const std::string& where);
    std::string BoolTest(uint32_t b, bool condition) const;
    bool CondExec(uint32_t index, const CfExec& e);
    bool PredExec(uint32_t index, const CfExec& e);
    bool Jump(uint32_t index, CfWords cf, uint32_t& next);
    bool LoopStart(uint32_t index, CfWords cf, uint32_t& next);
    bool ConstTempOperands(const AluInstruction& a, const std::string& where, std::string& c, std::string& r);
    std::string SamplerDeclarations() const;
    std::string Assemble(uint32_t interpolators) const;
};

bool Translator::VectorOperand(const AluInstruction& a, uint32_t n, uint32_t positions, std::string& out)
{
    const AluSource& s = a.src[n - 1];
    std::string swizzle;
    uint32_t reads = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        const uint32_t c = SwizzleComponent(s.swizzle, i);
        swizzle += kComponents[c];
        if (positions >> i & 1)
            reads |= 1u << c;
    }
    std::string base;
    if (s.isTemp)
    {
        if (s.tempRelative)
            return Fail("ALU source " + Num(n) + " is a relatively addressed temp (aL)");
        if (!ReadTemp(s.tempIndex, reads))
            return false;
        base = s.tempAbs ? "abs(r" + Num(s.tempIndex) + ")" : "r" + Num(s.tempIndex);
    }
    else
    {
        if (IsConstantAddressed(a, n))
            return Fail("ALU source " + Num(n) + " is a relatively addressed constant");
        base = a.absConstants ? "abs(xe_c[" + Num(s.reg) + "])" : "xe_c[" + Num(s.reg) + "]";
    }
    out = base + "." + swizzle;
    if (s.negate)
        out = "(-" + out + ")";
    return true;
}

// Scalar operand `a` (position 3) or `b` (position 0) of source 3.
bool Translator::ScalarOperand(const AluInstruction& a, uint32_t position, std::string& out)
{
    std::string v;
    if (!VectorOperand(a, 3, 1u << position, v))
        return false;
    // VectorOperand gave "<base>.<4 components>" or "(-<base>.<4>)": keep one component.
    const AluSource& s = a.src[2];
    const std::string component(1, kComponents[SwizzleComponent(s.swizzle, position)]);
    const size_t dot = v.rfind('.');
    v = v.substr(0, dot + 1) + component + (s.negate ? ")" : "");
    out = v;
    return true;
}

// The constant `c` (component a) and temp `r` (component b) of ADD_CONST / MUL_CONST (see the
// header comment), with abs_constants and src3's negate applied to both.
bool Translator::ConstTempOperands(const AluInstruction& a, const std::string& where, std::string& c, std::string& r)
{
    const AluSource& s = a.src[2];
    if (IsConstantAddressed(a, 3))
        return Fail("relatively addressed constant in a constant-temp scalar op" + where);
    const uint32_t temp = (a.scalarOpcode & 1) | (uint32_t(s.isTemp) << 1) | (s.swizzle & 0x3C);
    if (!ReadTemp(temp, 1u << SwizzleComponent(s.swizzle, 0)))
        return false;
    c = "xe_c[" + Num(s.reg) + "]." + kComponents[SwizzleComponent(s.swizzle, 3)];
    r = "r" + Num(temp) + "." + kComponents[SwizzleComponent(s.swizzle, 0)];
    if (a.absConstants)
        c = "abs(" + c + ")", r = "abs(" + r + ")";
    if (s.negate)
        c = "(-" + c + ")", r = "(-" + r + ")";
    return true;
}

bool Translator::ReadPredicate(const std::string& what, const std::string& where)
{
    if (!predWritten_)
        return Fail(what + " reads the predicate before any predicate-setting op sets it" + where);
    return true;
}

// A predicated instruction runs, both halves, only where p0 equals its condition (see the
// header comment).
bool Translator::Alu(const uint32_t w[3], uint32_t slot)
{
    const AluInstruction a = DecodeAlu(w);
    const std::string where = " (ALU instruction at slot " + Num(slot) + ")";
    if (!a.predicated)
    {
        if (inPredExec_)
            return Fail("unpredicated ALU instruction inside a predicated exec" + where);
        return AluOps(a, where);
    }
    if (!ReadPredicate("predicated ALU instruction", where))
        return false;
    if (inPredExec_)
    {
        // The exec's test guards it: p0 cannot change inside a predicated exec (refused).
        if (a.predicateCondition != predExecCondition_)
            return Fail("ALU instruction predicated on the opposite condition of its predicated exec" + where);
        return AluOps(a, where);
    }
    if (mode_ == Mode::Analyze)
        return pred_ == a.predicateCondition ? AluOps(a, where) : true;
    Line(PredicateTest(a.predicateCondition));
    Line("{");
    indent_ += "    ";
    const bool ok = AluOps(a, where);
    indent_.resize(indent_.size() - 4);
    Line("}");
    return ok;
}

// PRED_SETEs / PRED_SETNEs: checks where p0 may be set, reads the operand and records the set
// as a pass dimension (Emit) or takes this pass's p0 value (Analyze).
bool Translator::PredicateSet(const AluInstruction& a, const std::string& where, std::string& operand)
{
    if (inLoop_)
        return Fail("predicate set inside a loop" + where);
    if (inPredExec_)
        return Fail("predicate set inside a predicated exec" + where);
    if (execPredClean_)
        return Fail("predicate set inside an exec whose predicate-clean hint says p0 is not modified" + where);
    if (!ScalarOperand(a, 3, operand))
        return false;
    if (mode_ == Mode::Emit)
    {
        if (Dimensions() == kMaxBools)
            return Fail("more than " + Num(kMaxBools) + " bool constants, loops and predicate sets" + where);
        predSets_.push_back(instrKey_);
    }
    else
    {
        const size_t k = size_t(std::find(predSets_.begin(), predSets_.end(), instrKey_) - predSets_.begin());
        if (k == predSets_.size())
            return Fail("predicate set missing from the emit walk" + where);
        pred_ = (assignment_ >> (bools_.size() + loops_.size() + k)) & 1;
    }
    predWritten_ = true;
    return true;
}

bool Translator::AluOps(const AluInstruction& a, const std::string& where)
{
    if (a.vectorDestRelative)
        return Fail("relatively addressed ALU destination" + where);
    if (!a.isExport && a.scalarDestRelative)
        return Fail("relatively addressed scalar destination" + where);

    const uint32_t vectorMask = VectorResultMask(a), scalarMask = ScalarResultMask(a);
    const uint32_t one = Constant1Mask(a), zero = Constant0Mask(a);
    const uint32_t id = aluId_++;

    // Operands of both ops are read before either op writes.
    std::string vector;
    switch (VectorOpcode(a.vectorOpcode))
    {
    case VectorOpcode::Add:
    case VectorOpcode::Mul:
    case VectorOpcode::Max:
    {
        if (!vectorMask)
            break;
        std::string s1, s2;
        if (!VectorOperand(a, 1, vectorMask, s1) || !VectorOperand(a, 2, vectorMask, s2))
            return false;
        if (VectorOpcode(a.vectorOpcode) == VectorOpcode::Add)
            vector = "(" + s1 + " + " + s2 + ")";
        else if (VectorOpcode(a.vectorOpcode) == VectorOpcode::Mul)
            vector = "xe_mul(" + s1 + ", " + s2 + ")";
        else
            vector = s1 == s2 ? s1 : "xe_max(" + s1 + ", " + s2 + ")";
        break;
    }
    case VectorOpcode::Cube:
    {
        // CUBEv: cube map coordinates of src1 read as .z_xy (x, z, w = z, x, y; y unused):
        // (tc, sc, 2 * major axis, face id) (REFERENCE_DERIVED [X] spirv_shader_translator_alu.cc
        // kCube, its ordered comparisons; src2 is not read).
        if (!vectorMask)
            break;
        std::string s1;
        if (!VectorOperand(a, 1, 0xD, s1))
            return false;
        vector = "xe_cube(" + s1 + ")";
        needCube_ = true;
        break;
    }
    case VectorOpcode::Frc:
    case VectorOpcode::Trunc:
    case VectorOpcode::Floor:
    {
        // FRACv a - floor(a); TRUNCv a >= 0 ? floor(a) : -floor(-a) (GLSL trunc); FLOORv floor(a);
        // src1 only (REFERENCE_DERIVED [X] ucode.h).
        if (!vectorMask)
            break;
        std::string s1;
        if (!VectorOperand(a, 1, vectorMask, s1))
            return false;
        vector = VectorOpcode(a.vectorOpcode) == VectorOpcode::Frc     ? "(" + s1 + " - floor(" + s1 + "))"
                 : VectorOpcode(a.vectorOpcode) == VectorOpcode::Trunc ? "trunc(" + s1 + ")"
                                                                       : "floor(" + s1 + ")";
        break;
    }
    case VectorOpcode::Min:
    case VectorOpcode::Seq:
    case VectorOpcode::Sgt:
    case VectorOpcode::Sge:
    case VectorOpcode::Sne:
    {
        if (!vectorMask)
            break;
        std::string s1, s2;
        if (!VectorOperand(a, 1, vectorMask, s1) || !VectorOperand(a, 2, vectorMask, s2))
            return false;
        static constexpr const char* kNames[] = {"xe_min", "xe_seq", "xe_sgt", "xe_sge", "xe_sne"};
        vector = std::string(kNames[a.vectorOpcode - uint32_t(VectorOpcode::Min)]) + "(" + s1 + ", " + s2 + ")";
        break;
    }
    case VectorOpcode::Mad:
    case VectorOpcode::CndEq:
    case VectorOpcode::CndGe:
    case VectorOpcode::CndGt:
    {
        if (!vectorMask)
            break;
        std::string s1, s2, s3;
        if (!VectorOperand(a, 1, vectorMask, s1) || !VectorOperand(a, 2, vectorMask, s2) ||
            !VectorOperand(a, 3, vectorMask, s3))
            return false;
        static constexpr const char* kCnd[] = {"xe_cndeq", "xe_cndge", "xe_cndgt"};
        if (VectorOpcode(a.vectorOpcode) == VectorOpcode::Mad)
            vector = "(xe_mul(" + s1 + ", " + s2 + ") + " + s3 + ")";
        else
            vector = std::string(kCnd[a.vectorOpcode - uint32_t(VectorOpcode::CndEq)]) + "(" + s1 + ", " + s2 + ", " + s3 + ")";
        break;
    }
    case VectorOpcode::Dp4:
    {
        if (!vectorMask)
            break;
        std::string s1, s2;
        if (!VectorOperand(a, 1, 0xF, s1) || !VectorOperand(a, 2, 0xF, s2))
            return false;
        vector = "vec4(xe_dp4(" + s1 + ", " + s2 + "))";
        break;
    }
    case VectorOpcode::Dp3:
    {
        if (!vectorMask)
            break;
        std::string s1, s2;
        if (!VectorOperand(a, 1, 0x7, s1) || !VectorOperand(a, 2, 0x7, s2))
            return false;
        vector = "vec4(xe_dp3(" + s1 + ", " + s2 + "))";
        needDp3_ = true;
        break;
    }
    case VectorOpcode::KillEq:
    case VectorOpcode::KillGt:
    case VectorOpcode::KillGe:
    case VectorOpcode::KillNe:
    {
        // FM4 Recomp: KILLEv / KILLGTv / KILLGTEv / KILLNEv (24-27): the pixel is killed when any of
        // the four components compares true (==, >, >= ordered; != true for NaN, as SETEv..SETNEv),
        // and the result is 1.0 where it is killed, else 0.0 (REFERENCE_DERIVED [X] ucode.h kKillEq..
        // kKillNe; spirv_shader_translator_alu.cc kOps). The kill does not change control flow: it
        // sets xe_kill and the pixel is discarded at the end of main, before the alpha test, once
        // control flow and derivatives are no longer needed (REFERENCE_DERIVED [X]
        // spirv_shader_translator.cc "pixel killing ... without affecting the control flow ...
        // similarly to how pixel killing works on the Xenos", spirv_shader_translator_rb.cc
        // CompleteFragmentShaderInMain, its path without demote-to-helper). [X]'s translator writes
        // 0.0 even where the pixel is killed: the two differ only in a killed pixel's registers,
        // which reach nothing but its neighbours' derivatives (APPROXIMATE there). Pixel shaders
        // only; it runs whatever the write mask (it always may kill).
        if (!pixel_)
            return Fail("pixel kill in a vertex shader" + where);
        std::string s1, s2;
        if (!VectorOperand(a, 1, 0xF, s1) || !VectorOperand(a, 2, 0xF, s2))
            return false;
        static constexpr const char* kCmp[] = {"xe_seq", "xe_sgt", "xe_sge", "xe_sne"};
        const std::string killed = "xe_killed" + Num(id);
        Line("bool " + killed + " = any(notEqual(" + kCmp[a.vectorOpcode - uint32_t(VectorOpcode::KillEq)] + "(" + s1 + ", " +
             s2 + "), vec4(0.0)));");
        Line("xe_kill = xe_kill || " + killed + ";");
        kills_ = true;
        if (vectorMask)
            vector = "vec4(" + killed + " ? 1.0 : 0.0)";
        break;
    }
    case VectorOpcode::Dp2Add:
    {
        if (!vectorMask)
            break;
        std::string s1, s2, s3;
        if (!VectorOperand(a, 1, 0x3, s1) || !VectorOperand(a, 2, 0x3, s2) || !VectorOperand(a, 3, 0x1, s3))
            return false;
        vector = "vec4(xe_dp2add(" + s1 + ", " + s2 + ", " + s3 + "))";
        break;
    }
    default:
        return Fail("vector ALU opcode " + Num(a.vectorOpcode) + " is not supported" + where);
    }

    // A scalar op runs when it writes a component or, in a program that reads ps, always (it
    // updates ps) except RETAIN_PREV, which only copies ps.
    const bool retain = a.scalarOpcode == uint32_t(ScalarOpcode::RetainPrev);
    const bool runScalar = scalarMask || (readsPs_ && !retain);
    std::string scalar, predLine;
    switch (a.scalarOpcode)
    {
    case uint32_t(ScalarOpcode::SetpEq):
    case uint32_t(ScalarOpcode::SetpNe):
    case uint32_t(ScalarOpcode::SetpGt):
    case uint32_t(ScalarOpcode::SetpGe):
    {
        // Always runs (it sets p0): p0 = a == 0, a > 0, a >= 0 (ordered: NaN gives false) / a != 0
        // (true for NaN); the result is 0 where p0 is set, else 1 (REFERENCE_DERIVED [X] ucode.h
        // kSetpEq/Ne/Gt/Ge).
        std::string sa;
        if (!PredicateSet(a, where, sa))
            return false;
        const uint32_t op = a.scalarOpcode;
        const char* cmp = op == uint32_t(ScalarOpcode::SetpEq)   ? " == "
                          : op == uint32_t(ScalarOpcode::SetpNe) ? " != "
                          : op == uint32_t(ScalarOpcode::SetpGt) ? " > "
                                                                 : " >= ";
        predLine = "xe_p0 = (" + sa + cmp + "0.0);";
        if (runScalar)
            scalar = "(xe_p0 ? 0.0 : 1.0)";
        break;
    }
    case uint32_t(ScalarOpcode::Sqrt):
    case uint32_t(ScalarOpcode::Exp):
    case uint32_t(ScalarOpcode::Log):
    case uint32_t(ScalarOpcode::Rcp):
    case uint32_t(ScalarOpcode::Rcpc):
    case uint32_t(ScalarOpcode::Rsq):
    {
        if (!runScalar)
            break;
        std::string s;
        if (!ScalarOperand(a, 3, s))
            return false;
        const char* f = a.scalarOpcode == uint32_t(ScalarOpcode::Sqrt)  ? "sqrt("
                        : a.scalarOpcode == uint32_t(ScalarOpcode::Exp) ? "xe_exp2("
                        : a.scalarOpcode == uint32_t(ScalarOpcode::Log) ? "xe_log2("
                        : a.scalarOpcode == uint32_t(ScalarOpcode::Rsq) ? "xe_rsq("
                        : a.scalarOpcode == uint32_t(ScalarOpcode::Rcpc) ? "xe_rcpc("
                                                                         : "xe_rcp(";
        if (a.scalarOpcode == uint32_t(ScalarOpcode::Rcp) || a.scalarOpcode == uint32_t(ScalarOpcode::Rsq) ||
            a.scalarOpcode == uint32_t(ScalarOpcode::Rcpc))
            needRcp_ = true;  // kRcpHelper holds both; xe_rcpc calls xe_rcp
        needRcpc_ = needRcpc_ || a.scalarOpcode == uint32_t(ScalarOpcode::Rcpc);
        scalar = f + s + ")";
        break;
    }
    case uint32_t(ScalarOpcode::Frcs):
    case uint32_t(ScalarOpcode::Truncs):
    case uint32_t(ScalarOpcode::Floors):
    {
        // FRACs a - floor(a); TRUNCs trunc(a); FLOORs floor(a): the scalar forms of FRACv/TRUNCv/FLOORv
        // on src3's scalar operand (REFERENCE_DERIVED [X] ucode.h kFrcs/kTruncs/kFloors).
        if (!runScalar)
            break;
        std::string s;
        if (!ScalarOperand(a, 3, s))
            return false;
        scalar = a.scalarOpcode == uint32_t(ScalarOpcode::Frcs)     ? "(" + s + " - floor(" + s + "))"
                 : a.scalarOpcode == uint32_t(ScalarOpcode::Truncs) ? "trunc(" + s + ")"
                                                                    : "floor(" + s + ")";
        break;
    }
    case uint32_t(ScalarOpcode::Seqs):
    case uint32_t(ScalarOpcode::Sgts):
    case uint32_t(ScalarOpcode::Sges):
    case uint32_t(ScalarOpcode::Snes):
    {
        // a ==, >, >=, != 0.0 ? 1.0 : 0.0 (ordered, except != : true for NaN).
        if (!runScalar)
            break;
        std::string sa;
        if (!ScalarOperand(a, 3, sa))
            return false;
        static constexpr const char* kOps[] = {" == ", " > ", " >= ", " != "};
        scalar = "(" + sa + kOps[a.scalarOpcode - uint32_t(ScalarOpcode::Seqs)] + "0.0 ? 1.0 : 0.0)";
        break;
    }
    case uint32_t(ScalarOpcode::Mins):
    {
        if (!runScalar)
            break;
        std::string sa, sb;
        if (!ScalarOperand(a, 3, sa) || !ScalarOperand(a, 0, sb))
            return false;
        scalar = sa == sb ? sa : "(" + sa + " < " + sb + " ? " + sa + " : " + sb + ")";
        break;
    }
    case uint32_t(ScalarOpcode::Adds):
    case uint32_t(ScalarOpcode::Muls):
    case uint32_t(ScalarOpcode::Subs):
    case uint32_t(ScalarOpcode::Maxs):
    case uint32_t(ScalarOpcode::MaxAs):
    {
        // FM4 Recomp: MAXAs (23) is MAXs plus a write of the address register a0 = int(clamp(floor(a +
        // 0.5), -256, 255)) (REFERENCE_DERIVED [X] ucode.h kMaxAs, spirv_shader_translator_alu.cc). a0
        // is read only by a0-relative constant addressing, which this unit refuses (IsConstantAddressed
        // fails every relatively addressed constant): the a0 write has no reader and is not emitted.
        // Admitting a0-relative addressing requires modelling it here first.
        if (!runScalar)
            break;
        std::string sa, sb;
        if (!ScalarOperand(a, 3, sa) || !ScalarOperand(a, 0, sb))
            return false;
        if (a.scalarOpcode == uint32_t(ScalarOpcode::Adds))
            scalar = "(" + sa + " + " + sb + ")";
        else if (a.scalarOpcode == uint32_t(ScalarOpcode::Muls))
            scalar = "xe_mul(" + sa + ", " + sb + ")";
        else if (a.scalarOpcode == uint32_t(ScalarOpcode::Subs))
            scalar = "(" + sa + " - " + sb + ")";
        else  // MAXs, MAXAs
            scalar = sa == sb ? sa : "(" + sa + " >= " + sb + " ? " + sa + " : " + sb + ")";
        break;
    }
    case uint32_t(ScalarOpcode::MulsPrev):
    case uint32_t(ScalarOpcode::AddsPrev):
    case uint32_t(ScalarOpcode::SubsPrev):
    {
        if (!runScalar)
            break;
        std::string sa;
        if (!ScalarOperand(a, 3, sa))
            return false;
        const char* name = a.scalarOpcode == uint32_t(ScalarOpcode::MulsPrev)   ? "MUL_PREVs"
                           : a.scalarOpcode == uint32_t(ScalarOpcode::AddsPrev) ? "ADD_PREVs"
                                                                                : "SUB_PREVs";
        if (!psWritten_)
            return Fail(std::string(name) + " reads the previous scalar result before any scalar op sets it" + where);
        scalar = a.scalarOpcode == uint32_t(ScalarOpcode::MulsPrev)   ? "xe_mul(" + sa + ", xe_ps)"
                 : a.scalarOpcode == uint32_t(ScalarOpcode::AddsPrev) ? "(" + sa + " + xe_ps)"
                                                                      : "(" + sa + " - xe_ps)";
        break;
    }
    case uint32_t(ScalarOpcode::Mulsc0):
    case uint32_t(ScalarOpcode::Mulsc1):
    case uint32_t(ScalarOpcode::Addsc0):
    case uint32_t(ScalarOpcode::Addsc1):
    case uint32_t(ScalarOpcode::Subsc0):
    case uint32_t(ScalarOpcode::Subsc1):
    {
        // SUB_CONST_0/1: c - r with ADD_CONST's operands (Xenia ucode.h kSubsc0/1 "src0.a, src1.a", as
        // ported in ReCarbon runtime/gpu/xenos/ucode.h:1317-1320; its translator emits ca - cb).
        if (!runScalar)
            break;
        std::string c, r;
        if (!ConstTempOperands(a, where, c, r))
            return false;
        const bool mul = a.scalarOpcode <= uint32_t(ScalarOpcode::Mulsc1);
        const bool sub = a.scalarOpcode >= uint32_t(ScalarOpcode::Subsc0);
        scalar = mul ? "xe_mul(" + c + ", " + r + ")" : "(" + c + (sub ? " - " : " + ") + r + ")";
        break;
    }
    case uint32_t(ScalarOpcode::Sin):
    case uint32_t(ScalarOpcode::Cos):
    {
        // SIN / COS of src0.a (Xenia ucode.h kSin / kCos "dest.xyzw = sin(src0.a)", ReCarbon
        // ucode.h:1322-1330). Radians; the hardware's range reduction and precision are UNKNOWN
        // (APPROXIMATE: the host's sin/cos).
        if (!runScalar)
            break;
        std::string sa;
        if (!ScalarOperand(a, 3, sa))
            return false;
        scalar = std::string(a.scalarOpcode == uint32_t(ScalarOpcode::Sin) ? "sin(" : "cos(") + sa + ")";
        break;
    }
    case uint32_t(ScalarOpcode::RetainPrev):
        if (!scalarMask)
            break;
        if (!psWritten_)
            return Fail("RETAIN_PREV writes the previous scalar result before any scalar op sets it" + where);
        scalar = "xe_ps";
        break;
    default:
        return Fail("scalar ALU opcode " + Num(a.scalarOpcode) + " is not supported" + where);
    }

    const std::string v = "xe_v" + Num(id);
    std::string s = "xe_s" + Num(id);
    if (!vector.empty())
        Line("precise vec4 " + v + " = " + (a.vectorClamp ? "xe_sat(" + vector + ")" : vector) + ";");
    if (!predLine.empty())
        Line(predLine);
    if (!scalar.empty() && readsPs_ && !retain)
    {
        // ps takes the result before the clamp; the destination the clamped one.
        Line("precise float " + s + " = " + scalar + ";");
        Line("xe_ps = " + s + ";");
        if (a.scalarClamp)
            s = "xe_sat(" + s + ")";
        psWritten_ = true;
    }
    else if (!scalar.empty())
        Line("precise float " + s + " = " + (a.scalarClamp ? "xe_sat(" + scalar + ")" : scalar) + ";");

    std::string target;
    if (a.isExport)
    {
        if (!(vectorMask | scalarMask | one | zero))
            return true;  // writes nothing
        // A terminal jump region's own execs may export: on that path the program ends there (see
        // Jump), and each analysis pass checks its own path's exports.
        if (inConditional_ && !(inJump_ && terminalRegion_ && !inCondExec_ && !inPredExec_))
            return Fail("export inside a " + std::string(Conditional()) + where);
        if (inLoop_)
            return Fail("export inside a loop" + where);
        if (a.predicated)
            return Fail("predicated export" + where);
        switch (ClassifyExport(pixel_, a.vectorDest))
        {
        case ExportTarget::Position: target = "xe_position_out"; break;
        case ExportTarget::Interpolator: target = "xe_interp_out[" + Num(a.vectorDest) + "]"; break;
        case ExportTarget::Color:
            if (a.vectorDest == 0)
            {
                target = "xe_color0";
                break;
            }
            [[fallthrough]];
        default:
            return Fail(std::string(pixel_ ? "pixel" : "vertex") + " shader export to register " + Num(a.vectorDest) +
                        " is not supported" + where);
        }
        exported_[a.vectorDest] |= uint8_t(vectorMask | scalarMask | one | zero);
    }
    for (uint32_t i = 0; i < 4; i++)
    {
        const std::string c(1, kComponents[i]);
        if (vectorMask >> i & 1)
            Line((a.isExport ? target : "r" + Num(a.vectorDest)) + "." + c + " = " + v + "." + c + ";");
        if (scalarMask >> i & 1)
            Line((a.isExport ? target : "r" + Num(a.scalarDest)) + "." + c + " = " + s + ";");
        if (one >> i & 1)
            Line(target + "." + c + " = 1.0;");
        if (zero >> i & 1)
            Line(target + "." + c + " = 0.0;");
    }
    if (!a.isExport)
    {
        if (vectorMask)
            WriteTemp(a.vectorDest, vectorMask);
        if (scalarMask)
            WriteTemp(a.scalarDest, scalarMask);
    }
    return true;
}

bool Translator::StoreFetch(uint32_t dest, uint32_t destSwizzle, uint32_t available, const std::string& value,
                            const char* what, uint32_t slot)
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        const FetchDestComponent d = FetchDestSwizzle(destSwizzle, i);
        const std::string target = "r" + Num(dest) + "." + kComponents[i] + " = ";
        if (d == FetchDestComponent::Keep)
            continue;
        if (d == FetchDestComponent::Zero)
            Line(target + "0.0;");
        else if (d == FetchDestComponent::One)
            Line(target + "1.0;");
        else if (d == FetchDestComponent::Unknown6)
            return Fail(std::string(what) + " destination swizzle 6 (meaning unknown) at slot " + Num(slot));
        else if (uint32_t(d) >= available)
            return Fail(std::string(what) + " reads element component " + kComponents[uint32_t(d)] +
                        " its format lacks, at slot " + Num(slot));
        else
            Line(target + value + "." + kComponents[uint32_t(d)] + ";");
        mask |= 1u << i;
    }
    WriteTemp(dest, mask);
    return true;
}

bool Translator::VertexFetchOp(const uint32_t w[3], uint32_t slot)
{
    const VertexFetch f = DecodeVertexFetch(w);
    const std::string where = " (vertex fetch at slot " + Num(slot) + ")";
    if (pixel_)
        return Fail("vertex fetch in a pixel shader" + where);
    // Inside a predicated exec (no predicate-dependent jump region) the fetch runs under the exec's
    // test, as on the hardware (its bounds check flags an error only where it runs); a fetch
    // predicated on the exec's own condition is the same guard (FM4's car vertex shaders).
    // An unpredicated fetch there runs per invocation where p0 equals the condition, as Xenia's
    // exec `if` does; on the hardware a whole group enters the exec when any invocation passes
    // ([X] ucode.h kCondExecPred), so its destination may also be written where p0 differs:
    // APPROXIMATE, exact when the compiler predicates the fetch too (as [X] says it does).
    const bool guarded = inPredExec_ && !inJump_;
    if (inConditional_ && !guarded)
        return Fail("vertex fetch inside a " + std::string(Conditional()) + where);
    if (inLoop_)
        return Fail("vertex fetch inside a loop" + where);
    if (f.predicated && !(guarded && f.predicateCondition == predExecCondition_))
        return Fail("predicated vertex fetch" + where);
    if (f.destRelative || (!f.isMiniFetch && f.srcRelative))
        return Fail("relatively addressed vertex fetch register" + where);
    if (!f.mustBeOne)
        return Fail("vertex fetch with word 0 bit 19 clear (meaning unknown)" + where);
    VertexLayout layout;
    if (!GetVertexLayout(f.format, layout))
        return Fail("vertex format " + Num(f.format) + " is not supported" + where);
    // Signed repeating-fraction mode 1 ([X] kNoZero) is not implemented: refused where it applies.
    const bool normalized = layout.kind == VertexLayout::Packed && f.isNormalized;
    if (normalized && f.isSigned && f.signedRfModeAll)
        return Fail("vertex fetch signed repeating-fraction mode 1 is not supported" + where);

    const uint32_t id = fetchId_++;
    if (!f.isMiniFetch)
    {
        if (!ReadTemp(f.srcReg, 1u << f.srcSwizzle))
            return false;
        haveFullFetch_ = true;
        fullFetchExec_ = inPredExec_ ? instrKey_ / 8 : ~0u;
        fullSlot_ = FetchConstantSlot(f);
        fullStride_ = f.stride;
        fullIndex_ = "xe_vidx" + Num(id);
        const std::string src = "r" + Num(f.srcReg) + "." + kComponents[f.srcSwizzle];
        Line("int " + fullIndex_ + " = int(floor(" + src + (f.isIndexRounded ? " + 0.5" : "") + "));");
        vertexSlots_.insert(fullSlot_);
    }
    else if (!haveFullFetch_)
        return Fail("mini vertex fetch with no preceding full fetch" + where);
    // FM4 Recomp: a full fetch inside a predicated exec runs only where that exec runs, and its
    // index variable is scoped to the exec's `if`; a mini fetch elsewhere would reuse an address
    // the hardware may not have computed (it keeps the last full fetch that ran) and name an
    // undeclared variable. Refused.
    else if (fullFetchExec_ != ~0u && fullFetchExec_ != instrKey_ / 8)
        return Fail("mini vertex fetch whose full fetch is in the predicated exec at control-flow instruction " +
                    Num(fullFetchExec_) + where);

    const uint32_t s = fullSlot_, n = VertexFormatComponents(f.format);
    auto& extent = vertexExtents_[s];
    extent.first = std::max(extent.first, fullStride_);
    extent.second = std::max(extent.second, uint32_t(std::max(0, f.offset + int32_t(n))));
    const std::string word1 = "xe_fetch[" + Num(s / 2) + "]." + (s % 2 ? "w" : "y"), value = "xe_fetch" + Num(id);
    // Every dword of the element goes through xe_vertex_word, which checks it against the slot's
    // size (fetch word 1 bits 2-25) and flags an out-of-range fetch for the host to stop on.
    auto word = [&](uint32_t i) {
        return "xe_vertex_word(xe_vertex_base[" + Num(s / 4) + "][" + Num(s % 4) + "], " + fullIndex_ + ", " +
               Num(fullStride_) + "u, " + std::to_string(f.offset + int32_t(i)) + ", " + word1 + ")";
    };
    if (layout.kind == VertexLayout::Float32 && f.expAdjust == 0)
    {
        std::string vec = "vec4(";
        for (uint32_t c = 0; c < 4; c++)
        {
            vec += c < n ? "uintBitsToFloat(" + word(c) + ")" : std::string("0.0");
            vec += c < 3 ? ", " : ")";
        }
        Line("vec4 " + value + " = " + vec + ";");
        return StoreFetch(f.destReg, f.destSwizzle, n, value, "vertex fetch", slot);
    }

    for (uint32_t i = 0; i < layout.words; i++)
        Line("uint " + value + "_" + Num(i) + " = " + word(i) + ";");
    const std::string scale = f.expAdjust ? " * " + FloatBits(std::ldexp(1.0f, f.expAdjust)) : "";
    std::string vec = "vec4(";
    for (uint32_t c = 0; c < 4; c++)
    {
        std::string e = "0.0";
        if (c < n && layout.kind == VertexLayout::Float32)
            e = "uintBitsToFloat(" + value + "_" + Num(c) + ")" + scale;
        else if (c < n)
        {
            const std::string w = value + "_" + Num(layout.word[c]);
            const uint32_t width = layout.width[c], offset = layout.offset[c];
            if (layout.kind == VertexLayout::Half)
            {
                needHalf_ = true;
                e = "xe_half(" + (offset ? "(" + w + " >> 16)" : w) + " & 0xFFFFu)" + scale;
            }
            else
            {
                // Signed: sign-extended two's complement; unsigned: zero-extended.
                const std::string bits = width == 32 ? w : "bitfieldExtract(" + w + ", " + Num(offset) + ", " + Num(width) + ")";
                e = f.isSigned ? "float(" + (width == 32 ? "int(" + w + ")" : "bitfieldExtract(int(" + w + "), " + Num(offset) + ", " + Num(width) + ")") + ")"
                               : "float(" + bits + ")";
                if (normalized)
                {
                    const double d = f.isSigned ? std::ldexp(1.0, int(width) - 1) - 1.0 : std::ldexp(1.0, int(width)) - 1.0;
                    e += " * " + FloatBits(float(1.0 / d));
                    if (f.isSigned)
                        e = "max(" + e + ", -1.0)";
                }
                e += scale;
            }
        }
        vec += e + (c < 3 ? ", " : ")");
    }
    Line("precise vec4 " + value + " = " + vec + ";");
    return StoreFetch(f.destReg, f.destSwizzle, n, value, "vertex fetch", slot);
}

// getTextureGradients (fetch opcode 18): the screen-space derivatives of the source's first two
// components, (ddx(s.x), ddy(s.x), ddx(s.y), ddy(s.y)); no texture is read (REFERENCE_DERIVED [X]
// spirv_shader_translator_fetch.cc). Coarse derivatives as Xenia; whether the hardware's are
// coarse or fine is UNKNOWN (APPROXIMATE). Pixel shaders only, in uniform control flow (derivatives
// need it: outside predicated execs and predicated jump regions; bool-constant conditionals and
// loops are uniform, see Divergent); predication and relative registers are refused.
bool Translator::TextureGradientsOp(const uint32_t w[3], uint32_t slot)
{
    const TextureFetch t = DecodeTextureFetch(w);
    const std::string where = " (getTextureGradients at slot " + Num(slot) + ")";
    if (!pixel_)
        return Fail("getTextureGradients in a vertex shader" + where);
    if (Divergent())
        return Fail("getTextureGradients inside a " + std::string(Conditional()) + where);
    if (t.predicated)
        return Fail("predicated getTextureGradients" + where);
    if (t.srcRelative || t.destRelative)
        return Fail("relatively addressed getTextureGradients register" + where);
    const uint32_t c0 = TextureSourceComponent(t, 0), c1 = TextureSourceComponent(t, 1);
    if (!ReadTemp(t.srcReg, (1u << c0) | (1u << c1)))
        return false;
    const std::string value = "xe_grad" + Num(fetchId_++), src = "r" + Num(t.srcReg) + ".";
    const std::string sx = src + kComponents[c0], sy = src + kComponents[c1];
    Line("vec4 " + value + " = vec4(dFdxCoarse(" + sx + "), dFdyCoarse(" + sx + "), dFdxCoarse(" + sy + "), dFdyCoarse(" + sy + "));");
    return StoreFetch(t.destReg, t.destSwizzle, 4, value, "getTextureGradients", slot);
}

// setTextureLod (fetch opcode 24): the register LOD (xe_lod) = the source's first component
// (REFERENCE_DERIVED [X] spirv_shader_translator_fetch.cc kSetTextureLod; [X] starts it at 0, here a
// fetch reading it before any setTextureLod in program order is refused). Predication and relative
// registers are refused.
bool Translator::SetTextureLodOp(const uint32_t w[3], uint32_t slot)
{
    const TextureFetch t = DecodeTextureFetch(w);
    const std::string where = " (setTextureLod at slot " + Num(slot) + ")";
    if (t.predicated)
        return Fail("predicated setTextureLod" + where);
    if (t.srcRelative)
        return Fail("relatively addressed setTextureLod register" + where);
    const uint32_t c0 = TextureSourceComponent(t, 0);
    if (!ReadTemp(t.srcReg, 1u << c0))
        return false;
    Line("xe_lod = r" + Num(t.srcReg) + "." + kComponents[c0] + ";");
    lodWritten_ = true;
    return true;
}

bool Translator::TextureFetchOp(const uint32_t w[3], uint32_t slot)
{
    const TextureFetch t = DecodeTextureFetch(w);
    const std::string where = " (texture fetch at slot " + Num(slot) + ")";
    // Inside a predicated exec (and no predicate-dependent jump region) the fetch is sampled in
    // uniform control flow, outside the exec's `if`, and only its destination is written under the
    // predicate: a texture fetch has no side effects, so the registers end as the hardware leaves
    // them, and the implicit derivatives are defined (FM4's car shaders). For an
    // unpredicated fetch that holds where p0 equals the exec's condition; see VertexFetchOp on the
    // hardware's group-wide exec (APPROXIMATE there, as Xenia).
    // FM4 Recomp: bool-constant conditional execs and bool-constant jump regions are uniform control
    // flow (Divergent), so a fetch there samples in place, with defined implicit derivatives, and a
    // predicated exec inside a bool-constant jump region hoists its fetches into that region. Inside a
    // predicated jump's region the registers the fetch reads may have been written under p0 earlier in
    // the region, so it can be neither hoisted nor sampled in place: refused.
    const bool hoisted = inPredExec_ && !inPredJump_ && pixel_;
    if (Divergent() && !hoisted)
        return Fail("texture fetch inside a " + std::string(Conditional()) + where);
    // A predicated fetch inside a predicated exec on the same condition is the exec's own guard
    // (Xenia ucode.h TextureFetchInstruction is_predicated / pred_condition, word 1 bit 31 / word 2
    // bit 31, as ReCarbon xenos/ucode.h:784-879); any other predicated fetch is refused.
    if (t.predicated && !(hoisted && t.predicateCondition == predExecCondition_))
        return Fail("predicated texture fetch" + where);
    if (t.srcRelative || t.destRelative)
        return Fail("relatively addressed texture fetch register" + where);
    const bool volume = t.dimension == kDimension3DOrStacked, cube = t.dimension == kDimensionCube;
    if (t.dimension != kDimension2D && !volume && !cube)
        return Fail("texture fetch dimension " + Num(t.dimension) + " is not supported (2D, 3D and cube only)" + where);
    if (cube && (t.offsetX != 0 || t.offsetY != 0 || t.offsetZ != 0))
        return Fail("texture fetch offsets on a cube fetch are not supported" + where);
    if (t.unnormalized)
        return Fail("unnormalized texture coordinates are not supported" + where);
    // Filters: the instruction's mag/min (point, linear or the fetch constant's) go to the host's
    // sampler for the slot, as does the mip filter (Filters::mip; the host applies it to textures
    // with mip levels). The volume filters have no effect on a 2D fetch. Anisotropy only as
    // the fetch constant's or off; base-map (2) mag/min filters are not defined for them. A 3D
    // fetch's volume filters must match mag/min (see above).
    if (t.magFilter == 2 || t.minFilter == 2)
        return Fail("texture fetch mag/min filter 2 (base map) is not supported" + where);
    if (volume && (t.volMagFilter == 2 || t.volMinFilter == 2))
        return Fail("texture fetch volume filter 2 is not supported" + where);
    if (volume && ((t.volMagFilter != kFilterUseFetchConst && t.magFilter != kFilterUseFetchConst && t.volMagFilter != t.magFilter) ||
                   (t.volMinFilter != kFilterUseFetchConst && t.minFilter != kFilterUseFetchConst && t.volMinFilter != t.minFilter)))
        return Fail("texture fetch volume filters that differ from the mag/min filters are not supported" + where);
    if (t.anisoFilter != kAnisoUseFetchConst && t.anisoFilter != 0)
        return Fail("texture fetch anisotropic filter override " + Num(t.anisoFilter) + " is not supported" + where);
    if (t.arbitraryFilter != 0 && t.arbitraryFilter != kArbitraryUseFetchConst)
        return Fail("texture fetch arbitrary filter " + Num(t.arbitraryFilter) + " is not supported" + where);
    // LOD: computed (from derivatives) only in a pixel shader with use_computed_lod; otherwise the
    // explicit LOD, 0 plus the biases (the instruction's is refused below, the fetch constant's by
    // the host) (REFERENCE_DERIVED: Xenia's fetch translation). With use_register_lod the register
    // LOD is added ([X] fetch.cc: lod = fetch constant bias + register LOD + instruction bias): the
    // explicit LOD, or with a computed LOD its bias (GLSL texture bias; [X] scales the gradients by
    // 2^lod instead, the same for isotropic footprints: APPROXIMATE). Register gradients are refused.
    if (t.useRegisterGradients)
        return Fail("texture fetch with register gradients is not supported" + where);
    if (t.useRegisterLod && !lodWritten_)
        return Fail("texture fetch reads the register LOD before any setTextureLod" + where);
    const bool computedLod = pixel_ && t.useComputedLod;
    if (t.lodBias != 0)
        return Fail("texture fetch LOD bias is not supported" + where);
    if (t.offsetZ != 0 && !volume)
        return Fail("texture fetch Z offset on a 2D fetch is not supported" + where);
    if (t.unknown30)
        return Fail("texture fetch word 1 bit 30 set (meaning unknown)" + where);

    const uint32_t c0 = TextureSourceComponent(t, 0), c1 = TextureSourceComponent(t, 1), c2 = TextureSourceComponent(t, 2);
    if (!ReadTemp(t.srcReg, (1u << c0) | (1u << c1) | (volume || cube ? 1u << c2 : 0u)))
        return false;
    // Volume filters do not apply to a 2D fetch: reported as 3 (see ShaderTranslation::Filters).
    const ShaderTranslation::Filters filters{t.magFilter, t.minFilter, volume ? t.volMagFilter : kFilterUseFetchConst,
                                             volume ? t.volMinFilter : kFilterUseFetchConst, t.mipFilter};
    if (textureSlots_.count(t.constIndex))
    {
        if (textureDimensions_[t.constIndex] != t.dimension)
            return Fail("fetches of texture slot " + Num(t.constIndex) + " use different dimensions" + where);
        const ShaderTranslation::Filters& seen = textureFilters_[t.constIndex];
        if (seen.mag != filters.mag || seen.min != filters.min || seen.volMag != filters.volMag || seen.volMin != filters.volMin ||
            seen.mip != filters.mip)
            return Fail("fetches of texture slot " + Num(t.constIndex) + " set different filters" + where);
    }
    textureSlots_.insert(t.constIndex);
    textureFilters_[t.constIndex] = filters;
    textureDimensions_[t.constIndex] = t.dimension;
    const std::string value = "xe_texel" + Num(fetchId_++), src = "r" + Num(t.srcReg) + ".";
    const std::string sampler = "xe_texture_" + Num(t.constIndex), vec = volume || cube ? "vec3" : "vec2";
    std::string coordinates = vec + "(" + src + kComponents[c0] + ", " + src + kComponents[c1] +
                              (volume || cube ? ", " + src + kComponents[c2] : std::string()) + ")";
    // A cube fetch's (sc, tc, face) in 1...2, 1...2, 0...5 as a direction (REFERENCE_DERIVED [X]
    // spirv_shader_translator_fetch.cc kCube: *2 - 3, face clamped and floored, CUBEv's axes inverted).
    if (cube)
    {
        coordinates = "xe_cube_dir(" + coordinates + ")";
        needCubeDir_ = true;
    }
    // Offsets are in half texels (REFERENCE_DERIVED: Xenia's parsed offset_x/y/z, applied to the
    // texel-space coordinate, a 3D texture's Z as X and Y); normalized coordinates move by
    // offset / 2 / size. Xenia also adds a 1.5/1024-texel epsilon for point sampling between
    // texels, which is not done here.
    if (t.offsetX != 0 || t.offsetY != 0 || t.offsetZ != 0)
        coordinates = "(" + coordinates + " + " + vec + "(" + std::to_string(t.offsetX) + ".0, " + std::to_string(t.offsetY) + ".0" +
                      (volume ? ", " + std::to_string(t.offsetZ) + ".0" : std::string()) + ") * 0.5 / " + vec + "(textureSize(" +
                      sampler + ", 0)))";
    // A vertex shader has no derivatives: it samples LOD 0 (the LOD bias is refused above), as
    // Xenia uses the computed LOD only in pixel shaders (REFERENCE_DERIVED).
    if (hoisted && mode_ != Mode::Analyze)
    {
        indent_.resize(indent_.size() - 4);
        Line("}");
    }
    const std::string lod = t.useRegisterLod ? "xe_lod" : "0.0";
    // The fetch constant's LOD bias (dword 4 bits 12-21), when the host defines
    // XE_TEXTURE_<slot>_LOD_BIAS, is added like the register LOD ([X] fetch.cc: lod = fetch constant
    // bias + register LOD + instruction bias).
    const std::string biasMacro = "XE_TEXTURE_" + Num(t.constIndex) + "_LOD_BIAS";
    body_ += "#ifdef " + biasMacro + "\n";
    Line("vec4 " + value + " = " +
         (computedLod ? "texture(" + sampler + ", " + coordinates + ", " + lod + " + " + biasMacro + ")"
                      : "textureLod(" + sampler + ", " + coordinates + ", " + lod + " + " + biasMacro + ")") + ";");
    body_ += "#else\n";
    Line("vec4 " + value + " = " +
         (computedLod ? "texture(" + sampler + ", " + coordinates + (t.useRegisterLod ? ", xe_lod" : "") + ")"
                      : "textureLod(" + sampler + ", " + coordinates + ", " + lod + ")") + ";");
    body_ += "#endif\n";
    if (hoisted && mode_ != Mode::Analyze)
    {
        Line(PredicateTest(predExecCondition_));
        Line("{");
        indent_ += "    ";
    }
    body_ += "#ifdef XE_TEXTURE_" + Num(t.constIndex) + "_GAMMA\n";
    Line(value + ".xyz = xe_pwl_gamma_to_linear(" + value + ".xyz);");
    body_ += "#endif\n";
    // The fetch constant's exponent bias (dword 3 bits 13-18), after the sign/gamma conversion,
    // on every component (REFERENCE_DERIVED: Xenia's fetch translation multiplies by
    // ldexp(1, bias)); the host defines the factor.
    body_ += "#ifdef XE_TEXTURE_" + Num(t.constIndex) + "_SCALE\n";
    Line(value + " *= XE_TEXTURE_" + Num(t.constIndex) + "_SCALE;");
    body_ += "#endif\n";
    return StoreFetch(t.destReg, t.destSwizzle, 4, value, "texture fetch", slot);
}

bool Translator::Exec(const CfExec& e, uint32_t index)
{
    for (uint32_t k = 0; k < e.count; k++)
    {
        instrKey_ = index * 8 + k;
        const uint32_t slot = e.address + k;
        const uint32_t* w;
        if (!ReadSlot(words_, count_, slot, w))
            return Fail("exec instruction slot " + Num(slot) + " lies past the end of the shader");
        if (!IsFetch(e, k))
        {
            if (!Alu(w, slot))
                return false;
            continue;
        }
        const uint32_t op = FetchOpcodeOf(w);
        if (op == kVertexFetch)
        {
            if (!VertexFetchOp(w, slot))
                return false;
        }
        else if (op == kTextureFetch)
        {
            if (!TextureFetchOp(w, slot))
                return false;
        }
        else if (op == kGetTextureGradients)
        {
            if (!TextureGradientsOp(w, slot))
                return false;
        }
        else if (op == kSetTextureLod)
        {
            if (!SetTextureLodOp(w, slot))
                return false;
        }
        else
            return Fail("fetch opcode " + Num(op) + " is not supported (slot " + Num(slot) + ")");
    }
    return true;
}

// Conditional exec on bool constant `e.boolAddress` (opcode 13; [X]: word1 bits 2-9 are the
// constant, bit 10 the condition, and there is no predicate-clean bit): runs when the constant's
// bit equals the condition.
// Analyze: whether bool constant b equals `condition` in this pass's assignment.
bool Translator::BoolTaken(uint32_t b, bool condition)
{
    const size_t i = size_t(std::find(bools_.begin(), bools_.end(), b) - bools_.begin());
    return (i < bools_.size() && (assignment_ >> i & 1)) == condition;
}

// Emit: records bool constant b as a pass dimension.
bool Translator::AddBool(uint32_t b, const std::string& where)
{
    if (std::find(bools_.begin(), bools_.end(), b) != bools_.end())
        return true;
    if (Dimensions() == kMaxBools)
        return Fail(where + " tests more than " + Num(kMaxBools) + " distinct bool constants (loops and predicate sets included)");
    bools_.push_back(b);
    return true;
}

// Bool constant b: bit b % 32 of dword b / 32 (register SHADER_CONSTANT_BOOL_000_031 + b / 32).
std::string Translator::BoolTest(uint32_t b, bool condition) const
{
    return "if (((xe_bool[" + Num(b >> 7) + "]." + kComponents[(b >> 5) & 3] + " >> " + Num(b & 31) + ") & 1u) == " +
           (condition ? "1u)" : "0u)");
}

bool Translator::CondExec(uint32_t index, const CfExec& e)
{
    const uint32_t b = e.boolAddress;
    if (mode_ == Mode::Analyze)
        return BoolTaken(b, e.condition) ? Exec(e, index) : true;  // Emit has refused what it may not hold
    if (!AddBool(b, "conditional exec at control-flow instruction " + Num(index)))
        return false;
    Line(BoolTest(b, e.condition));
    Line("{");
    indent_ += "    ";
    // FM4 Recomp: restored, not cleared, so a conditional exec inside a jump's region leaves the
    // region conditional (exports after it stay refused).
    const bool outer = inConditional_;
    inConditional_ = inCondExec_ = true;
    const bool ok = Exec(e, index);
    inConditional_ = outer;
    inCondExec_ = false;
    indent_.resize(indent_.size() - 4);
    Line("}");
    return ok;
}

// Predicated exec (opcode 5, see the header comment): runs where p0 equals its condition; every
// instruction in it must be predicated on that condition.
bool Translator::PredExec(uint32_t index, const CfExec& e)
{
    const std::string where = " (predicated exec at control-flow instruction " + Num(index) + ")";
    if (e.absoluteAddress)
        return Fail("predicated exec with word 1 bit 11 (address mode) set is not supported" + where);
    if (!ReadPredicate("predicated exec", where))
        return false;
    inPredExec_ = true;
    predExecCondition_ = e.condition;
    bool ok = true;
    if (mode_ == Mode::Analyze)
        ok = pred_ != e.condition || Exec(e, index);
    else
    {
        Line(PredicateTest(e.condition));
        Line("{");
        indent_ += "    ";
        const bool outer = inConditional_;
        inConditional_ = true;
        ok = Exec(e, index);
        inConditional_ = outer;
        indent_.resize(indent_.size() - 4);
        Line("}");
    }
    inPredExec_ = false;
    return ok;
}

// cond_jmp at control-flow instruction `index` (see the header comment). Opens the skipped
// region as a conditional (Emit; Analyze when this pass does not jump) or sets `next` so the walk
// resumes at the target (Analyze, jump taken).
bool Translator::Jump(uint32_t index, CfWords cf, uint32_t& next)
{
    const CfBranch j = DecodeBranch(cf);
    const std::string where = " (cond_jmp at control-flow instruction " + Num(index) + ")";
    if (j.absoluteAddress)
        return Fail("jump with word 1 bit 11 (address mode) set is not supported" + where);
    if (j.direction)
        return Fail("jump with word 1 bit 1 (direction, meaning unknown) set is not supported" + where);
    if (j.address <= index)
        return Fail("backward jump to control-flow instruction " + Num(j.address) + " is not supported" + where);
    if (j.unconditional)
    {
        // Only as the last instruction of a jump's region, skipping forward from its end: the
        // region it skips is the else branch.
        if (!inJump_ || inElse_ || index + 1 != jumpEnd_)
            return Fail("unconditional jump other than at the end of a conditional jump's region is not supported" + where);
        if (j.predicated || j.boolAddress != 0 || j.condition)
            return Fail("unconditional jump with predicate, bool constant or condition fields set is not supported" + where);
        if (mode_ == Mode::Analyze)
        {
            inJump_ = inConditional_ = inPredJump_ = false;
            next = j.address - 1;
            return true;
        }
        indent_.resize(indent_.size() - 4);
        Line("}");
        Line("else");
        Line("{");
        indent_ += "    ";
        inElse_ = true;
        jumpEnd_ = j.address;
        next = index;
        return true;
    }
    if (j.predicated)
    {
        if (j.boolAddress != 0)
            return Fail("predicated jump with a bool constant field set is not supported" + where);
        if (!ReadPredicate("predicated jump", where))
            return false;
    }
    // FM4 Recomp: a region whose last instruction is exec_end ends the program where the jump is
    // not taken (exec_end ends the shader, [X] ucode.h; Xenia's translator then leaves its main loop
    // for the one epilogue, SpirvShaderTranslator::ProcessExecInstructionEnd). Bool-constant jumps,
    // pixel shaders only: a vertex shader's interpolator count is taken from one pass (Finish).
    CfWords last;
    const bool terminal = ReadCf(words_, count_, j.address - 1, last) && CfOpcodeOf(last) == CfOpcode::ExecEnd;
    if (terminal && j.predicated)
        return Fail("predicated jump over an exec_end is not supported" + where);
    if (terminal && !pixel_)
        return Fail("jump over an exec_end in a vertex shader is not supported" + where);
    if (mode_ == Mode::Analyze)
    {
        if (j.predicated ? pred_ == j.condition : BoolTaken(j.boolAddress, j.condition))
        {
            next = j.address - 1;
            return true;
        }
    }
    else
    {
        if (!j.predicated && !AddBool(j.boolAddress, "cond_jmp at control-flow instruction " + Num(index)))
            return false;
        // The region runs when the jump is not taken.
        Line(j.predicated ? PredicateTest(!j.condition) : BoolTest(j.boolAddress, !j.condition));
        Line("{");
        indent_ += "    ";
    }
    inJump_ = inConditional_ = true;
    inPredJump_ = j.predicated;
    terminalRegion_ = terminal;
    jumpEnd_ = j.address;
    next = index;
    return true;
}

// loop_start at control-flow instruction `index` (see the header comment). Checks the loop's
// shape, then either opens the loop (Emit; Analyze when this assignment runs the body) or sets
// `next` to its loop_end so the walk resumes after it (Analyze, skipped loop).
bool Translator::LoopStart(uint32_t index, CfWords cf, uint32_t& next)
{
    const CfBranch b = DecodeBranch(cf);
    const std::string where = " (loop_start at control-flow instruction " + Num(index) + ")";
    if (loopEnds_.size() == kMaxLoopDepth)
        return Fail("loops nested deeper than " + Num(uint32_t(kMaxLoopDepth)) + where);
    if (b.isRepeat)
        return Fail("loop_start reusing aL (is_repeat) is not supported" + where);
    if (b.absoluteAddress)
        return Fail("loop_start with word 1 bit 11 (address mode) set is not supported" + where);
    // The skip address is the control-flow instruction after the matching loop_end, whose
    // address is the instruction after this loop_start.
    CfWords endCf;
    if (b.address < index + 2 || !ReadCf(words_, count_, b.address - 1, endCf) || CfOpcodeOf(endCf) != CfOpcode::LoopEnd)
        return Fail("loop_start skip address " + Num(b.address) + " does not follow a loop_end" + where);
    const CfBranch e = DecodeBranch(endCf);
    if (e.address != index + 1 || e.loopId != b.loopId || e.absoluteAddress)
        return Fail("loop_end at control-flow instruction " + Num(b.address - 1) +
                    " does not close the loop (body address, loop constant or address mode)" + where);
    if (e.predicatedBreak)
        return Fail("loop_end with a predicated break is not supported" + where);
    // A nested loop closes inside the loop around it.
    if (!loopEnds_.empty() && b.address - 1 >= loopEnds_.back())
        return Fail("nested loop ends after the loop around it" + where);

    const uint32_t k = loopOrdinal_++;
    if (mode_ == Mode::Analyze)
    {
        if (!(assignment_ >> (bools_.size() + k) & 1))
        {
            // The body is skipped (count 0): loops nested in it are not walked, but keep their ordinals.
            for (uint32_t j = index + 1; j + 1 < b.address; j++)
            {
                CfWords inner;
                if (ReadCf(words_, count_, j, inner) && CfOpcodeOf(inner) == CfOpcode::LoopStart)
                    loopOrdinal_++;
            }
            next = b.address - 1;  // the walk resumes after this loop's loop_end
            return true;
        }
    }
    else
    {
        if (Dimensions() == kMaxBools)
            return Fail("more than " + Num(kMaxBools) + " conditional exec bool constants, loops and predicate sets" + where);
        loops_.push_back(b.loopId);
        // Loop constant L: dword L (register SHADER_CONSTANT_LOOP_00 + L), count in bits 0-7.
        const std::string n = Num(k), count = "xe_loop_count" + n, i = "xe_i" + n;
        Line("uint " + count + " = xe_loop[" + Num(b.loopId >> 2) + "]." + kComponents[b.loopId & 3] + " & 0xFFu;");
        Line("for (uint " + i + " = 0u; " + i + " < " + count + "; " + i + "++)");
        Line("{");
        indent_ += "    ";
    }
    inLoop_ = true;
    loopEnds_.push_back(b.address - 1);
    next = index;
    return true;
}

bool Translator::Run(std::string& error)
{
    uint32_t firstInstruction = ~0u;
    for (uint32_t i = 0;; i++)
    {
        CfWords cf;
        if (i / 2 >= firstInstruction || !ReadCf(words_, count_, i, cf))
        {
            Fail("control flow ends without exec_end");
            break;
        }
        if (inJump_ && i == jumpEnd_)
        {
            inJump_ = inConditional_ = inElse_ = inPredJump_ = false;
            if (mode_ == Mode::Emit)
            {
                indent_.resize(indent_.size() - 4);
                Line("}");
                if (terminalRegion_)
                {
                    // The rest of the program runs only where the jump was taken.
                    Line("else");
                    Line("{");
                    indent_ += "    ";
                    elsesToEnd_++;
                }
            }
            terminalRegion_ = false;
        }
        const CfOpcode op = CfOpcodeOf(cf);
        if (op == CfOpcode::Nop)
            continue;
        // A jump's region holds execs, predicated execs and bool-constant conditional execs, and may
        // end in an unconditional jump; a terminal region also allocs and ends in its exec_end.
        if (inJump_ && op != CfOpcode::Exec && op != CfOpcode::CondExecPred && op != CfOpcode::CondExecPredClean &&
            !(op == CfOpcode::CondJmp && DecodeBranch(cf).unconditional && !inElse_ && i + 1 == jumpEnd_) &&
            !(terminalRegion_ && (op == CfOpcode::Alloc || (op == CfOpcode::ExecEnd && i + 1 == jumpEnd_))))
        {
            Fail(std::string(op == CfOpcode::ExecEnd ? "exec_end" : CfName(op)) + " (control-flow opcode " +
                 Num(uint32_t(op)) + ") inside a jump's skipped region is not supported");
            break;
        }
        if (op == CfOpcode::Alloc && inLoop_)
        {
            Fail("alloc (control-flow opcode 12) inside a loop is not supported");
            break;
        }
        if (op == CfOpcode::Alloc)
        {
            if (DecodeAlloc(cf).type == AllocType::Memory)
            {
                Fail("memory export allocation is not supported");
                break;
            }
            continue;
        }
        if (op == CfOpcode::LoopStart)
        {
            if (!LoopStart(i, cf, i))
                break;
            continue;
        }
        if (op == CfOpcode::LoopEnd)
        {
            if (loopEnds_.empty() || i != loopEnds_.back())
            {
                Fail("loop_end at control-flow instruction " + Num(i) + " has no matching loop_start");
                break;
            }
            loopEnds_.pop_back();
            inLoop_ = !loopEnds_.empty();
            if (mode_ == Mode::Emit)
            {
                indent_.resize(indent_.size() - 4);
                Line("}");
            }
            continue;
        }
        if (inLoop_ && op != CfOpcode::Exec)
        {
            Fail(std::string(op == CfOpcode::ExecEnd ? "exec_end" : CfName(op)) + " (control-flow opcode " +
                 Num(uint32_t(op)) + ") inside a loop is not supported");
            break;
        }
        if (op == CfOpcode::CondJmp)
        {
            if (!Jump(i, cf, i))
                break;
            continue;
        }
        if (op != CfOpcode::Exec && op != CfOpcode::ExecEnd && op != CfOpcode::CondExecPredClean &&
            op != CfOpcode::CondExecPred)
        {
            Fail(std::string(CfName(op)) + " (control-flow opcode " + Num(uint32_t(op)) + ") is not supported");
            break;
        }
        const CfExec e = DecodeExec(cf);
        if (e.address <= i / 2)
        {
            Fail("exec at control-flow instruction " + Num(i) + " addresses control-flow slot " + Num(e.address));
            break;
        }
        firstInstruction = std::min(firstInstruction, e.address);
        // Opcode 13 has no predicate-clean bit ([X]: it is clean); the others' is word1 bit 9.
        execPredClean_ = op == CfOpcode::CondExecPredClean || e.predicateClean;
        const bool ok = op == CfOpcode::CondExecPredClean ? CondExec(i, e) : op == CfOpcode::CondExecPred ? PredExec(i, e) : Exec(e, i);
        execPredClean_ = false;
        if (!ok)
            break;
        // Emit walks on past a terminal region's exec_end into the `else` (the jump-taken path).
        if (op == CfOpcode::ExecEnd && !(mode_ == Mode::Emit && inJump_))
            break;
    }
    if (mode_ == Mode::Emit)
        for (; elsesToEnd_; elsesToEnd_--)
        {
            indent_.resize(indent_.size() - 4);
            Line("}");
        }

    if (error_.empty() && mode_ == Mode::Analyze)
    {
        if (pixel_)
        {
            if (exported_[0] != 0xF)
                Fail("pixel shader does not write all of colour 0");
        }
        else
        {
            if (exported_[62] != 0xF)
                Fail("vertex shader does not write all of the position");
            for (uint32_t i = 0; i < 16; i++)
                if (exported_[i])
                    interpolators_ = i + 1;
            // Interpolators written in part (or skipped below the highest) keep 0 elsewhere: the
            // body starts from xe_interp_out[i] = vec4(0.0), as Xenia zeroes them.
        }
    }
    error = error_;
    return error_.empty();
}

void Translator::Finish(const std::vector<Translator>& analyses, ShaderTranslation& out)
{
    // Exports never sit inside a conditional exec; the only conditional exports are a pixel
    // shader's in a terminal jump region (colour 0, checked by each pass), so every vertex shader
    // pass exports the same registers and analyses[0]'s interpolator count holds for all.
    uint32_t interpolators = analyses[0].interpolators_;
    for (const Translator& a : analyses)
        preloadCount_ = std::max(preloadCount_, a.preloadCount_);
    if (pixel_)
        interpolators = preloadCount_;
    out.glsl = Assemble(interpolators);
    out.interpolatorCount = interpolators;
    out.vertexFetchSlots.assign(vertexSlots_.begin(), vertexSlots_.end());
    for (uint32_t slot : out.vertexFetchSlots)
    {
        out.vertexFetchStrides.push_back(vertexExtents_[slot].first);
        out.vertexFetchEnds.push_back(vertexExtents_[slot].second);
    }
    out.textureSlots.assign(textureSlots_.begin(), textureSlots_.end());
    out.textureFilters.clear();
    out.textureDimensions.clear();
    for (uint32_t s : textureSlots_)
    {
        out.textureFilters.push_back(textureFilters_[s]);
        out.textureDimensions.push_back(textureDimensions_[s]);
    }
    out.boolConstants = bools_;
    std::sort(out.boolConstants.begin(), out.boolConstants.end());
    const std::set<uint32_t> loops(loops_.begin(), loops_.end());
    out.loopConstants.assign(loops.begin(), loops.end());
}

constexpr const char* kAluHelpers = R"(// Direct3D 9 multiply: +-0 times anything is +0.
float xe_mul(float a, float b)
{
    precise float p = a * b;
    return (a == 0.0 || b == 0.0) ? 0.0 : p;
}
vec4 xe_mul(vec4 a, vec4 b)
{
    return vec4(xe_mul(a.x, b.x), xe_mul(a.y, b.y), xe_mul(a.z, b.z), xe_mul(a.w, b.w));
}
float xe_dp2add(vec4 a, vec4 b, vec4 c)
{
    precise float s = xe_mul(a.x, b.x) + xe_mul(a.y, b.y);
    precise float r = s + c.x;
    return r;
}
float xe_dp4(vec4 a, vec4 b)
{
    precise float s = xe_mul(a.x, b.x) + xe_mul(a.y, b.y);
    precise float t = s + xe_mul(a.z, b.z);
    precise float r = t + xe_mul(a.w, b.w);
    return r;
}
vec4 xe_max(vec4 a, vec4 b) { return mix(b, a, greaterThanEqual(a, b)); }
vec4 xe_min(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }
vec4 xe_seq(vec4 a, vec4 b) { return mix(vec4(0.0), vec4(1.0), equal(a, b)); }
vec4 xe_sgt(vec4 a, vec4 b) { return mix(vec4(0.0), vec4(1.0), greaterThan(a, b)); }
vec4 xe_sge(vec4 a, vec4 b) { return mix(vec4(0.0), vec4(1.0), greaterThanEqual(a, b)); }
vec4 xe_sne(vec4 a, vec4 b) { return mix(vec4(0.0), vec4(1.0), notEqual(a, b)); }
vec4 xe_cndeq(vec4 a, vec4 b, vec4 c) { return mix(c, b, equal(a, vec4(0.0))); }
vec4 xe_cndge(vec4 a, vec4 b, vec4 c) { return mix(c, b, greaterThanEqual(a, vec4(0.0))); }
vec4 xe_cndgt(vec4 a, vec4 b, vec4 c) { return mix(c, b, greaterThan(a, vec4(0.0))); }
// EXP_IEEE / LOG_IEEE with their exact cases spelled out (GLSL leaves them undefined or inexact).
float xe_exp2(float x)
{
    if (x == 0.0)
        return 1.0;
    if (isnan(x) || isinf(x))
        return x > 0.0 || isnan(x) ? x : 0.0;
    return exp2(x);
}
float xe_log2(float x)
{
    if (x == 1.0)
        return 0.0;
    if (x == 0.0)
        return uintBitsToFloat(0xFF800000u);
    if (x < 0.0)
        return uintBitsToFloat(0x7FC00000u);
    if (isnan(x) || isinf(x))
        return x;
    return log2(x);
}
float xe_sat(float x) { return x >= 0.0 ? min(x, 1.0) : 0.0; }
vec4 xe_sat(vec4 x) { return mix(vec4(0.0), min(x, vec4(1.0)), greaterThanEqual(x, vec4(0.0))); }
)";

// A Xenos binary16 vertex component (16_16_FLOAT, 16_16_16_16_FLOAT; h < 0x10000), decoded with
// integer operations only, so no float16 conversion or denormal flushing is involved: exponent
// 1-30 is (1 + m / 1024) * 2^(e - 15), exponent 0 is m * 2^-24 (from float(m), exact for m <
// 1024, with its exponent lowered by 24), the sign applies to zero too. Exponent 31 (IEEE Inf /
// NaN; [X] notes Xenos may treat it as extended range) has an UNKNOWN meaning: it sets bit 1 of
// xe_error for the host to stop on and reads 0.
constexpr const char* kHalfHelper = R"(float xe_half(uint h)
{
    uint e = (h >> 10) & 0x1Fu, m = h & 0x3FFu, s = (h & 0x8000u) << 16;
    if (e == 31u)
    {
        atomicOr(xe_error, 2u);
        return 0.0;
    }
    if (e == 0u)
        return uintBitsToFloat(m == 0u ? s : s | (floatBitsToUint(float(m)) - (24u << 23)));
    return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13));
}
)";
// Emitted only when used, so programs without them keep their text.
constexpr const char* kCubeDirHelper = R"(vec3 xe_cube_dir(vec3 c)
{
    float sc = c.x * 2.0 - 3.0, tc = c.y * 2.0 - 3.0;
    uint face = uint(c.z > 0.0 ? min(c.z, 5.0) : 0.0);
    bool neg = (face & 1u) != 0u;
    float sgn = neg ? -1.0 : 1.0;
    if ((face >> 1) == 0u)
        return vec3(sgn, -tc, neg ? sc : -sc);
    if ((face >> 1) == 1u)
        return vec3(sc, sgn, neg ? -tc : tc);
    return vec3(neg ? -sc : sc, -tc, sgn);
}
)";
constexpr const char* kCubeHelper = R"(vec4 xe_cube(vec4 s)
{
    float x = s.z, y = s.w, z = s.x;
    if (abs(z) >= abs(x) && abs(z) >= abs(y))
        return vec4(-y, z < 0.0 ? -x : x, 2.0 * z, z < 0.0 ? 5.0 : 4.0);
    if (abs(y) >= abs(x))
        return vec4(y < 0.0 ? -z : z, x, 2.0 * y, y < 0.0 ? 3.0 : 2.0);
    return vec4(-y, x < 0.0 ? z : -z, 2.0 * x, x < 0.0 ? 1.0 : 0.0);
}
)";
constexpr const char* kDp3Helper = R"(float xe_dp3(vec4 a, vec4 b)
{
    precise float s = xe_mul(a.x, b.x) + xe_mul(a.y, b.y);
    precise float r = s + xe_mul(a.z, b.z);
    return r;
}
)";
// FM4 Recomp: RECIP_CLAMP (scalar 17): RECIP_IEEE (so 1 -> 1, +-0 -> +-Inf), then -Inf -> -FLT_MAX
// and +Inf -> FLT_MAX; NaN passes through. REFERENCE_DERIVED [X] ucode.h kRcpc ("t = src0.a == 1.0 ?
// 1.0 : 1.0 / src0.a; if (t == -INF) t = -FLT_MAX; else if (t == INF) t = FLT_MAX") and
// spirv_shader_translator_alu.cc kRcpc (the clamp is on the quotient, so a finite operand whose
// reciprocal overflows clamps too). Emitted after kRcpHelper.
constexpr const char* kRcpcHelper = R"(float xe_rcpc(float x)
{
    float r = xe_rcp(x);
    if (isinf(r))
        return uintBitsToFloat(r < 0.0 ? 0xFF7FFFFFu : 0x7F7FFFFFu);
    return r;
}
)";
// RECIP_IEEE with its exact cases spelled out (GLSL leaves division by 0 or Inf undefined).
constexpr const char* kRcpHelper = R"(float xe_rcp(float x)
{
    if (x == 1.0)
        return 1.0;
    if (x == 0.0)
        return uintBitsToFloat((floatBitsToUint(x) & (1u << 31)) | 0x7F800000u);
    if (isinf(x))
        return uintBitsToFloat(floatBitsToUint(x) & (1u << 31));
    if (isnan(x))
        return x;
    precise float r = 1.0 / x;
    return r;
}
// RECIPSQ_IEEE: x == 1 -> 1 (REFERENCE_DERIVED [X] ucode.h); +-0 -> +-Inf, +Inf -> +0, negatives
// and NaN -> NaN (IEEE rsqrt, INFERRED; the NaN's bits APPROXIMATE); otherwise the host's
// 1 / sqrt (APPROXIMATE precision).
float xe_rsq(float x)
{
    if (x == 1.0)
        return 1.0;
    if (x == 0.0)
        return uintBitsToFloat((floatBitsToUint(x) & (1u << 31)) | 0x7F800000u);
    if (isinf(x) && x > 0.0)
        return 0.0;
    if (isnan(x) || x < 0.0)
        return uintBitsToFloat(0x7FC00000u);
    precise float s = sqrt(x);
    precise float r = 1.0 / s;
    return r;
}
)";

// Xenos piecewise-linear gamma, 8-bit gamma k <-> 10-bit linear n (see the header comment).
constexpr const char* kGammaHelpers = R"(vec3 xe_saturate3(vec3 x) { return mix(vec3(0.0), min(x, vec3(1.0)), greaterThanEqual(x, vec3(0.0))); }
vec3 xe_pwl_gamma_to_linear(vec3 gamma)
{
    vec3 g = xe_saturate3(gamma);
    bvec3 p1 = greaterThanEqual(g, vec3(64.0 / 255.0));
    bvec3 p2 = greaterThanEqual(g, vec3(96.0 / 255.0));
    bvec3 p3 = greaterThanEqual(g, vec3(192.0 / 255.0));
    vec3 scale = mix(mix(vec3(1.0 / 1024.0), vec3(2.0 / 1024.0), p1), mix(vec3(4.0 / 1024.0), vec3(8.0 / 1024.0), p3), p2);
    vec3 offset = mix(mix(vec3(0.0), vec3(-64.0), p1), mix(vec3(-256.0), vec3(-1024.0), p3), p2);
    precise vec3 l = g * (255.0 * 1024.0) * scale + offset;
    precise vec3 m = l + trunc(l * scale);
    precise vec3 r = m * (1.0 / 1023.0);
    return r;
}
vec3 xe_linear_to_pwl_gamma(vec3 linear)
{
    vec3 l = xe_saturate3(linear);
    bvec3 p1 = greaterThanEqual(l, vec3(64.0 / 1023.0));
    bvec3 p2 = greaterThanEqual(l, vec3(128.0 / 1023.0));
    bvec3 p3 = greaterThanEqual(l, vec3(512.0 / 1023.0));
    vec3 scale = mix(mix(vec3(1023.0), vec3(1023.0 / 2.0), p1), mix(vec3(1023.0 / 4.0), vec3(1023.0 / 8.0), p3), p2);
    vec3 offset = mix(mix(vec3(0.0), vec3(32.0 / 255.0), p1), mix(vec3(64.0 / 255.0), vec3(128.0 / 255.0), p3), p2);
    precise vec3 k = trunc(l * scale) * (1.0 / 255.0) + offset;
    return k;
}
)";

constexpr const char* kVertexHeader = R"(layout(std430, set = 0, binding = 0) readonly buffer XeVertexData { uint xe_vertex_words[]; };
layout(std140, set = 0, binding = 1) uniform XeSystem
{
    uvec4 xe_fetch[48];
    uvec4 xe_vertex_base[24];
    vec4 xe_viewport_scale;
    vec4 xe_viewport_offset;
    uint xe_flags;
};
#ifdef XE_RECT_LIST
layout(std430, set = 0, binding = 4) readonly buffer XeIndices { uint xe_indices[]; };
#endif
// Endian swap of a fetched dword: 1 8in16, 2 8in32, 3 16in32.
uint xe_swap(uint v, uint e)
{
    if (e == 1u || e == 2u)
        v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u || e == 3u)
        v = (v << 16) | (v >> 16);
    return v;
}
// Bit 0 set: a vertex fetch fell outside its slot's data (the host stops the run).
layout(std430, set = 0, binding = 5) buffer XeErrors { uint xe_error; };
// Dword `offset` of vertex `index` (`stride` dwords apart) of a fetch slot whose data starts at
// xe_vertex_words[base], with the slot's fetch constant word 1 (size in dwords bits 2-25,
// endian bits 0-1). Outside the slot's data (or a negative index) it flags xe_error and reads 0.
uint xe_vertex_word(uint base, int index, uint stride, int offset, uint word1)
{
    uint size = (word1 >> 2) & 0xFFFFFFu;
    bool inside = index >= 0 && offset >= 0 && (stride == 0u || uint(index) <= size / stride);
    uint at = inside ? uint(index) * stride + uint(offset) : 0u;
    if (!inside || at >= size)
    {
        atomicOr(xe_error, 1u);
        return 0u;
    }
    return xe_swap(xe_vertex_words[base + at], word1 & 3u);
}
)";

// The texture declarations; the host rewrites each binding by matching this exact text.
std::string Translator::SamplerDeclarations() const
{
    std::string g;
    for (uint32_t s : textureSlots_)
        g += "layout(set = 0, binding = " + Num(8 + s) + ") uniform " +
             (textureDimensions_.at(s) == kDimension3DOrStacked ? "sampler3D"
              : textureDimensions_.at(s) == kDimensionCube      ? "samplerCube"
                                                                : "sampler2D") +
             " xe_texture_" + Num(s) + ";\n";
    return g;
}

std::string Translator::Assemble(uint32_t interpolators) const
{
    std::string g = "#version 450\n// Translated from Xenos microcode by FM4 Recomp (runtime/shader/shader_glsl.cpp).\n";
    std::string temps;
    for (uint32_t r = 0; r < 64; r++)
        if (usedTemps_ >> r & 1)
        {
            std::string init = "vec4(0.0)";
            if (pixel_ && r < preloadCount_)
                init = "xe_interp_" + Num(r);
            else if (!pixel_ && r == 0)
                init = "vec4(float(xe_index), 0.0, 0.0, 0.0)";
            temps += "    vec4 r" + Num(r) + " = " + init + ";\n";
        }
    // The previous scalar result; never read before a scalar op sets it (checked), so the
    // initial value is not observable.
    if (readsPs_)
        temps += "    float xe_ps = 0.0;\n";
    // The predicate p0; likewise never read before a predicate set sets it (checked).
    if (!predSets_.empty())
        temps += "    bool xe_p0 = false;\n";
    // The register LOD; never read before setTextureLod sets it (checked).
    if (lodWritten_)
        temps += "    float xe_lod = 0.0;\n";
    // Set by kill ops; the pixel is discarded at the end of main.
    if (kills_)
        temps += "    bool xe_kill = false;\n";
    const std::string helpers = std::string(kAluHelpers) + (needDp3_ ? kDp3Helper : "") + (needRcp_ ? kRcpHelper : "") +
                                (needRcpc_ ? kRcpcHelper : "") +
                                (needCube_ ? kCubeHelper : "") + (needCubeDir_ ? kCubeDirHelper : "");

    // Bool constants follow the float constants (std140 offset 4096) when a conditional exec or
    // a loop reads them; loop constants follow the bools (offset 4128) when a loop reads them.
    const std::string extra = !loops_.empty() ? " uvec4 xe_bool[2]; uvec4 xe_loop[8];" : !bools_.empty() ? " uvec4 xe_bool[2];" : "";
    const std::string constants = std::string("layout(std140, set = 0, binding = ") + (pixel_ ? "3) uniform XePsConstants" : "2) uniform XeVsConstants") +
                                  " { vec4 xe_c[256];" + extra + " };\n";
    if (pixel_)
    {
        g += constants;
        g += SamplerDeclarations();
        for (uint32_t i = 0; i < interpolators; i++)
            g += "layout(location = " + Num(i) + ") in vec4 xe_interp_" + Num(i) + ";\n";
        g += "layout(location = 0) out vec4 xe_color0;\n";
        g += helpers;
        g += kGammaHelpers;
        g += "#ifdef XE_ALPHA_TEST\nbool xe_alpha_pass(float a, float r)\n{\n"
             "    switch (XE_ALPHA_TEST)\n    {\n    case 0: return false;\n    case 1: return a < r;\n    case 2: return a == r;\n"
             "    case 3: return a <= r;\n    case 4: return a > r;\n    case 5: return a != r;\n    case 6: return a >= r;\n"
             "    default: return true;\n    }\n}\n#endif\n";
        g += "void main()\n{\n" + temps + body_;
        // A killed pixel is discarded once the program has run (see the kill ops in AluOps).
        if (kills_)
            g += "    if (xe_kill)\n        discard;\n";
        // Alpha test (RB_COLORCONTROL; the host defines the function 0-7, xenos::CompareFunction,
        // and the reference RB_ALPHA_REF): colour 0's alpha as exported, before any output
        // conversion; a failing pixel is discarded (REFERENCE_DERIVED: Xenia's translator).
        g += "#ifdef XE_ALPHA_TEST\n    if (!xe_alpha_pass(xe_color0.w, XE_ALPHA_REF))\n        discard;\n#endif\n";
        g += "#ifdef XE_COLOR0_GAMMA\n    xe_color0.xyz = xe_linear_to_pwl_gamma(xe_color0.xyz);\n#endif\n";
        g += "#ifdef XE_COLOR0_7E3\n    xe_color0 = mix(vec4(0.0), min(xe_color0, vec4(31.875, 31.875, 31.875, 1.0)), "
             "greaterThan(xe_color0, vec4(0.0)));\n#endif\n";
        // XE_COLOR0_SCALE (the host's 2^(bias - 5) for a 16_16_16_16 target held as SNORM: Xenia
        // vulkan_command_processor.cc 3679-3695, colour exponent bias): the output, after alpha test.
        g += "#ifdef XE_COLOR0_SCALE\n    xe_color0 *= XE_COLOR0_SCALE;\n#endif\n}\n";
        return g;
    }

    const std::string n = Num(std::max(interpolators, 1u));
    g += kVertexHeader;
    g += constants;
    g += SamplerDeclarations();
    for (uint32_t i = 0; i < interpolators; i++)
        g += "layout(location = " + Num(i) + ") out vec4 xe_interp_" + Num(i) + ";\n";
    g += helpers;
    if (needHalf_)
        g += kHalfHelper;
    g += "void xe_vertex(uint xe_index, out vec4 xe_position_out, out vec4 xe_interp_out[" + n + "])\n{\n";
    g += "    xe_position_out = vec4(0.0);\n";
    g += "    for (int i = 0; i < " + n + "; i++)\n        xe_interp_out[i] = vec4(0.0);\n";
    g += temps + body_ + "}\n";
    // The W-format fix-up (FM4 Recomp; shader_glsl.h): Xenia 67f16c4 spirv_shader_translator.cc:1482-1554
    // CompleteVertexOrTessEvalShaderInMain, every operation NoContraction (precise). xe_flags bits: 1 XY divided
    // by W (PA_CL_VTE_CNTL vtx_xy_fmt), 2 Z divided by W (vtx_z_fmt), 4 W not reciprocal (vtx_w0_fmt).
    g += R"(vec4 xe_fixup(vec4 p)
{
    precise float w = (xe_flags & 4u) != 0u ? p.w : 1.0 / p.w;
    precise vec2 xy = (xe_flags & 1u) != 0u ? p.xy * w : p.xy;
    precise float z = (xe_flags & 2u) != 0u ? p.z * w : p.z;
    return vec4(xy, z, w);
}
)";
    g += "void xe_emit(vec4 q, vec4 interp[" + n + "])\n{\n";
    // The viewport fold (:1556-1575, NoContraction): q is the fixed-up position, q.w the real W.
    g += "    precise vec3 xe_ndc = q.xyz * xe_viewport_scale.xyz + xe_viewport_offset.xyz * q.w;\n";
    g += "    gl_Position = vec4(xe_ndc, q.w);\n";
    for (uint32_t i = 0; i < interpolators; i++)
        g += "    xe_interp_" + Num(i) + " = interp[" + Num(i) + "];\n";
    g += "}\n";
    g += "#ifndef XE_RECT_LIST\nvoid main()\n{\n    vec4 p;\n    vec4 interp[" + n +
         "];\n    xe_vertex(uint(gl_VertexIndex), p, interp);\n    xe_emit(xe_fixup(p), interp);\n}\n#else\n";
    // Rect list: ForzaRecomp runtime/video/renderer.cpp ExpandRectList and Xenia's rectangle-list
    // geometry shader (vulkan_pipeline_cache.cc), on the exported (pre-fold) positions.
    g += R"(float xe_len2(vec4 a, vec4 b)
{
    precise float dx = b.x - a.x;
    precise float dy = b.y - a.y;
    precise float l = dx * dx + dy * dy;
    return l;
}
vec4 xe_mirror(vec4 v0, vec4 v1, vec4 v2)
{
    precise vec4 d = v1 - v0;
    precise vec4 r = d + v2;
    return r;
}
void main()
{
    uint rect = uint(gl_VertexIndex) / 6u, corner = uint(gl_VertexIndex) % 6u;
)";
    g += "    vec4 p0, p1, p2;\n    vec4 i0[" + n + "], i1[" + n + "], i2[" + n + "];\n";
    g += R"(    xe_vertex(xe_indices[3u * rect + 0u], p0, i0);
    xe_vertex(xe_indices[3u * rect + 1u], p1, i1);
    xe_vertex(xe_indices[3u * rect + 2u], p2, i2);
    // The W-format fix-up first (Xenia expands rectangles after its vertex shader's epilogue); the
    // fold is linear in the 4-vector, so folding before or after the mirror is the same.
    p0 = xe_fixup(p0);
    p1 = xe_fixup(p1);
    p2 = xe_fixup(p2);
    // Squared edge lengths: [0] edge 12, [1] edge 20, [2] edge 01; v0 is opposite the longest.
    float len0 = xe_len2(p1, p2), len1 = xe_len2(p2, p0), len2 = xe_len2(p0, p1);
    uint first = (len0 > len1 && len0 > len2) ? 0u : (len1 > len2 ? 1u : 2u);
)";
    g += "    vec4 a = p0, b = p1, c = p2;\n    vec4 ai[" + n + "] = i0, bi[" + n + "] = i1, ci[" + n + "] = i2;\n";
    g += R"(    if (first == 1u)
    {
        a = p1; b = p2; c = p0; ai = i1; bi = i2; ci = i0;
    }
    else if (first == 2u)
    {
        a = p2; b = p0; c = p1; ai = i2; bi = i0; ci = i1;
    }
    // Triangles (v0, v1, v2) and (v1, v3, v2), v3 = (v1 - v0) + v2.
)";
    g += "    vec4 p = a;\n    vec4 interp[" + n + "] = ai;\n";
    g += R"(    if (corner == 1u || corner == 3u)
    {
        p = b; interp = bi;
    }
    else if (corner == 2u || corner == 5u)
    {
        p = c; interp = ci;
    }
    else if (corner == 4u)
    {
        p = xe_mirror(a, b, c);
)";
    g += "        for (int i = 0; i < " + n + "; i++)\n            interp[i] = xe_mirror(ai[i], bi[i], ci[i]);\n";
    g += "    }\n    xe_emit(p, interp);\n}\n#endif\n";
    return g;
}

}  // namespace

bool TranslateShader(const uint32_t* words, size_t count, bool pixel, ShaderTranslation& out, std::string& error)
{
    out = ShaderTranslation{};
    error.clear();
    if (!words || count == 0 || count % 3 != 0 || count > 3 * 4096)
    {
        error = "shader size of " + std::to_string(count) + " words is not a whole number of 3-word slots";
        return false;
    }
    Translator emit(words, uint32_t(count), pixel, Mode::Emit);
    if (!emit.Run(error))
        return false;
    // One analysis per assignment of the bool constants the conditional execs test, of each
    // loop's body running (count >= 1) or not (count 0) and of the p0 each predicate set produces.
    std::vector<Translator> analyses;
    for (uint32_t a = 0; a < (1u << emit.Dimensions()); a++)
    {
        analyses.emplace_back(words, uint32_t(count), pixel, Mode::Analyze, emit.Bools(), emit.Loops(), emit.PredSets(), a);
        if (!analyses.back().Run(error))
            return false;
    }
    emit.Finish(analyses, out);
    return true;
}

}  // namespace runtime::shader
