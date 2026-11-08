// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (diagnostics behind environment flags, in separate files; never a default):
// FM4_DUMP_TEXTURE=<8 hex digits> names a texture's base physical address (fetch dword 1 & 0xFFFFF000, e.g. 117D8000).
// The first kMaxTextureDumps loads of a texture with that base write, into the run's capture directory
// (fm4recomp --gpu-capture, the evidence dir; renderer.cpp passes it):
//   texdump_<addr>_<n>_info.txt         the six fetch words, the description (format, size, endian, tiling, every level's
//                                        address, origin, blocks, pitch, extent, staging offset and bytes), the view's
//                                        component mapping, gamma, the sampler;
//   texdump_<addr>_<n>_level<k>_guest.bin  the guest bytes each level reads ([address, address + extent)), as in memory;
//   texdump_<addr>_<n>_staged.bin       the staged upload (what vkCmdCopyBufferToImage copies: levels at their offsets).
// A comparison script untiles the guest bytes independently (Xenia's formulas) and compares. Capped: at most
// kMaxTextureDumps dumps and kMaxTextureDumpBytes per dump (a larger one is skipped, logged). Unset or empty: nothing (one
// getenv at the first load); a malformed value stops (fail closed).
#pragma once
#include "texture_info.h"
#include "texture_upload.h"
#include <cstdint>

namespace runtime::video {

constexpr uint32_t kMaxTextureDumps = 4;
constexpr uint64_t kMaxTextureDumpBytes = 64ull << 20;

void SetTextureDumpDir(const char* dir);
// After FillStaging succeeded for a load (texture_vk.cpp Upload).
void MaybeDumpTexture(const uint32_t words[6], const TextureDescription& d, const TextureBinding& binding, const LoadPlan& plan, GuestBytesFn guest,
                      void* context, const uint8_t* staged);

}  // namespace runtime::video
