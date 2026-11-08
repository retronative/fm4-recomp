// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (diagnostics behind environment flags, in separate files; never a
// default): FM4_DIAG_PS_NO_DEPTH=<16 hex digits>[,<hash>...] (up to 8 pixel shader hashes) turns the depth test and
// depth write off in the pipelines of draws whose pixel shader matches (key.depthWrite 0, key.depthFunc 7 = always:
// pipeline_cache.cpp then leaves depthTestEnable off). The depth attachment, the stencil state and everything else stay
// as decoded, so the pipeline stays compatible with the same render pass. Purpose: one run shows whether FM4's text
// (ps_3b0d169e4a23847a, z GEQUAL with write) is lost to the depth test. The run log names the setting at the first
// affected draw. Unset or empty: nothing (one getenv at the first draw); a malformed value stops (fail closed).
#pragma once
#include "pipeline_key.h"
#include <cstdint>

namespace runtime::video {

// True (and the key changed) when the diagnostic applies to this draw.
bool DiagNoDepth(uint64_t psHash, PipelineKey& key);

}  // namespace runtime::video
