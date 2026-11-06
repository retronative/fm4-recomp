// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The audible audio sink (audio_sdl.cpp).
#pragma once

namespace runtime::audio {
// Opens the default playback device (SDL3), logs the driver and device, starts pulling frames; stops (9) on failure.
void StartSdlAudio();
}  // namespace runtime::audio
