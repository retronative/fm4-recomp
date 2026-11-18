// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (diagnostic, opt-in): FM4_DIAG_TRIGGER=<path> probes a live session (the see-through road, 2026-11-15). Every 30 swaps,
// at a point where every submission is confirmed complete (renderer.cpp, beside EvictIdleTextures), the file is read if it exists,
// one command per line, then renamed to <path>.done:
//   stale-textures   every cached texture is re-hashed (and re-uploaded if its guest bytes differ) at its next bind
//   stale-vertices   the vertex cache forgets its windows (copied again at their next draw)
// Off without the variable (nothing read).
#pragma once
#include <functional>
#include <string>

namespace runtime::video::diag {
void TriggerAtRetire(uint64_t swap, const std::function<void(const std::string&)>& run);
}  // namespace runtime::video::diag
