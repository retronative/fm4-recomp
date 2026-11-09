// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (diagnostics behind environment flags, in separate files; never a default):
// FM4_DIAG_TEXTURE_UPLOADS=1 breaks the texture unit's uploads down per swap (the race frame's
// ~5 GB of uploads). texture_vk.cpp's counter adds plan.stagingBytes at every Upload, and with kTextureValidity =
// ReloadEveryBind every bind uploads. This diagnostic hashes each upload's staged bytes (64-bit FNV-1a over words) and
// classifies it against the same texture's previous upload (key: base address, format, width, height, levels) and the
// GPU writes (resolves, MarkGuestWritten) since then:
//   first        the texture's first upload in the run;
//   same-swap    unchanged bytes, already uploaded in this swap;
//   earlier      unchanged bytes, last uploaded in an earlier swap;
//   gpu-changed  changed bytes, a resolve wrote one of its ranges since its last upload (a GPU -> guest memory -> GPU
//                round trip: the resolve's read-back into guest memory, then this upload of it);
//   cpu-changed  changed bytes, no resolve wrote its ranges (a guest CPU write, or a write the log does not see).
// Per swap it prints the totals (bytes and uploads per class, the unique bytes: distinct (texture, hash) pairs), the
// uploads of textures a resolve has ever written, and the top 5 textures by bytes. Unset or empty: nothing (one getenv).
#pragma once
#include "texture_info.h"
#include <cstdint>
#include <string>

namespace runtime::video {

bool DiagTextureUploadsEnabled();
void DiagTextureUpload(const TextureDescription& d, const uint8_t* staging, uint64_t bytes);
void DiagTextureGpuWrite(uint32_t physical, uint32_t bytes);
// The swap's breakdown (empty when disabled or nothing was uploaded); resets the swap's counts.
std::string DiagTextureSwapSummary();

}  // namespace runtime::video
