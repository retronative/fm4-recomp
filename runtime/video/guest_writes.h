// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Guest memory written by a path other than a guest CPU store: the one contract between the units that
// write guest memory from the GPU side (the EDRAM unit's resolves, M4 unit 6; render_targets_vk.h plan 6)
// and the texture unit (unit 7, texture_vk.cpp, which implements these functions). All of them run on the
// command processor's thread (the renderer's; render_hooks.h).
//
//   MarkGuestWritten   the EDRAM unit calls it when it RECORDS a resolve into the open submission, with the
//                      resolve's guest range (ResolveInfo.destExtentStart / Length, physical). From then on a
//                      texture over any byte of the range is stale, and it is *pending*: its bytes in guest
//                      memory are only valid once the submission that writes them is confirmed complete.
//                      A draw that binds a texture over a pending range is refused (named), never sampled
//                      from guest memory the GPU has not written yet. The EDRAM unit must also make the
//                      written bytes visible to the host before the fence signals (a buffer barrier to
//                      HOST / HOST_READ after the compute write; host-coherent memory, or an invalidate).
//   GuestWritesPending true while some marked range waits for its submission (diagnostics; the order does
//                      not depend on it: the renderer submits and retires the open slot after EVERY resolve,
//                      renderer.cpp Draw DrawKind::Resolve, so the next draw's texture load reads the resolved
//                      bytes whether or not this was called; the mark only moves the watch's sequences and
//                      arms the pending refusal as a safety net). One bounded wait per resolve in unit 4's
//                      single slot; unit 5 replaces it (GPU-side reload, or slots).
//   GuestWritesRetired the renderer calls it from RetireSlot after a CONFIRMED completion (never after a
//                      stranding): every pending range is complete. Unit 4 has one slot, so "the open
//                      submission" is slot 0's; unit 5 must give the mark a slot (open question).
#pragma once
#include <cstdint>

namespace runtime::video {

void MarkGuestWritten(uint32_t physical, uint32_t bytes);
bool GuestWritesPending();
void GuestWritesRetired();
// FM4 Recomp (FM4_RESOLVE_DEFER): whether [physical, physical + bytes) shares a byte with a pending range.
bool GuestWritesOverlap(uint32_t physical, uint32_t bytes);

}  // namespace runtime::video
