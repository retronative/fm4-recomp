// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (diagnostic, FM4_DIAG_AV_LATENCY=1): one time base
// (steady_clock) for an A press as the guest reads it (XamInputGetState), the swaps that follow (the UI transition), and the
// audio frame the guest submits after it with the largest level rise (the sound's onset), with the queue ahead of it and the
// time the output clock pulls it. Behaviour is unchanged; only "[av]" lines are printed, for 2 s after each press.
#pragma once
#include <cstdint>

namespace runtime::av {

bool Enabled();
// Nanoseconds on the shared clock.
uint64_t NowNs();
// XamInputGetState saw the A button go down (`vblank`: the guest's vblank count then).
void APressed(uint64_t vblank);
// A swap was presented (renderer), with its draw count: printed while a press is being followed.
void Swap(uint64_t swap, uint64_t draws);
// The guest submitted audio frame `sequence` (1-based; frames are played in submission order, so the n-th frame pulled is
// sequence n) at level `rms` (0..1), with `queuedAhead` frames already waiting.
void AudioSubmitted(uint64_t sequence, double rms, uint32_t queuedAhead);
// The output clock pulled frame `sequence` (1-based, in submission order), with the device queue in ms (-1: mute).
void AudioPulled(uint64_t sequence, double deviceQueueMs);

}  // namespace runtime::av
