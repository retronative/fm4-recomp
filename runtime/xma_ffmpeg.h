// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FFmpeg's "xmaframes" decoder (tools/ffmpeg-xma, patches/ffmpeg/0001) as the XMA frame decoder (kernel/xma.h).
#pragma once
#include "kernel/xma_pass.h"

namespace runtime::xma {
// Checks the linked libavcodec (the headers' version, an LGPL licence, the xmaframes decoder), logs it, and returns
// the factory; stops (9) if the library is not the pinned build.
apu::XmaDecoderFactory FfmpegDecoderFactory();
}  // namespace runtime::xma
