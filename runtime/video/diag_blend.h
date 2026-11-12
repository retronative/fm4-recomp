// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (diagnostics behind environment flags, in separate files; never a default).
// Purpose: find the translucent pass that darkens a rectangle under the player car.
//   FM4_DIAG_BLEND_LOG=<vblank>: from that vblank on, the first draw of every distinct (vs, ps, colour 0 blend and write mask,
//     depth, stencil, cull, topology) state that blends colour 0 is logged once, "[diag blend]": the draw, both hashes, the
//     target format, the blend factors and ops, depth used/write/func/bias, stencil, cull, topology.
//   FM4_DIAG_PS_SKIP=<16 hex digits>[,<hash>...] (up to 8 pixel shader hashes): draws whose pixel shader matches are not recorded
//     (the run log names the first). A rendering change for one diagnostic run: never a mode.
//   FM4_DIAG_TEX_USERS=<8 hex digits>[,...] (up to 4 texture base addresses, fetch dword 1 & 0xFFFFF000): every distinct
//     (vs, ps, slot) that binds a texture with that base is logged once, "[diag tex-user]": the draw, both hashes, the slot and the
//     six fetch words.
// Unset or empty: nothing (one getenv at the first draw). A malformed value stops the run (fail closed).
#pragma once
#include "pipeline_key.h"
#include <cstdint>

namespace runtime::video {

// Logs the draw's blended state when FM4_DIAG_BLEND_LOG asks for it.
void DiagBlendLog(uint64_t draw, const PipelineKey& key, uint64_t vblank);
// True when FM4_DIAG_PS_SKIP names this pixel shader: the caller does not record the draw.
bool DiagSkipDraw(uint64_t psHash, uint64_t draw);
// Logs a texture bind when FM4_DIAG_TEX_USERS names its base.
void DiagTextureUser(uint64_t draw, uint64_t vsHash, uint64_t psHash, uint32_t slot, const uint32_t (&words)[6]);

}  // namespace runtime::video
