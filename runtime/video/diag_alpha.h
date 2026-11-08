// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (diagnostics behind environment flags, in separate files; never a default):
// FM4_DIAG_ALPHA_QUANT=<levels>[:<16 hex digits>] (levels 1-65535; optionally one pixel shader hash) adds
// XE_ALPHA_TEST_QUANT <levels>.0 to the pixel shader variant of every alpha-tested draw (one whose defines hold
// XE_ALPHA_TEST), so shader_glsl.cpp's xe_alpha_pass compares round(clamp(alpha, 0, 1) * levels) / levels instead of the
// float alpha. Purpose: one run shows whether
// FM4's logo glow (ps_93879bb4564bce04, alpha = texel.w^2, test "greater than 0") stops writing depth over the welcome text
// when its faint alpha is quantized as an 8-bit (255) or 2-bit (3, the 2_10_10_10 target) value would be. Not a rendering
// mode: the console's alpha-test precision is UNRESOLVED (Xenia compares the float, spirv_shader_translator_rb.cc:526-600).
// Unset or empty: nothing (one getenv at the first draw); a malformed value stops (fail closed).
#pragma once
#include "shader_cache.h"
#include <cstdint>

namespace runtime::video {

// True (and the define added) when the diagnostic applies to this draw.
bool DiagAlphaQuantize(uint64_t psHash, ShaderVariantDefines& psDefines);

}  // namespace runtime::video
