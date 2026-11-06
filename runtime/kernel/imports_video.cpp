// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Video imports. Semantics: Xenia xenia-canary 67f16c4e315f:
// src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc, src/xenia/kernel/xam/xam_video.cc, src/xenia/xbox.h:264.
#include "imports.h"
#include <cstring>

namespace {

// X_VIDEO_MODE (xbox.h:264, 48 bytes, big-endian): width, height, interlaced, widescreen, high definition,
// refresh rate (float), video standard, two words Xenia sets to 0x4A and 0x01, three reserved words.
// Display policy: Xenia's defaults (xboxkrnl_video.cc:172 VdQueryVideoMode with
// internal_display_resolution 8): 1280x720, progressive, widescreen (1280/4 > 720/3), high definition,
// 60 Hz, NTSC (1).
void WriteVideoMode(uint8_t* base, uint32_t mode)
{
    float refresh = 60.0f;
    uint32_t refreshBits;
    std::memcpy(&refreshBits, &refresh, 4);
    const uint32_t words[12] = {1280, 720, 0, 1, 1, refreshBits, 1, 0x4A, 0x01, 0, 0, 0};
    for (int i = 0; i < 12; i++)
        runtime::Store32(base, mode + 4 * i, words[i]);
}

}  // namespace

GUEST_IMPORT(VdQueryVideoMode, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:172 VdQueryVideoMode (display policy: Xenia defaults)")
{
    WriteVideoMode(base, ctx.r3.u32);
}

GUEST_IMPORT(XGetVideoMode, "REFERENCE_DERIVED: Xenia xam_video.cc:21 XGetVideoMode (VdQueryVideoMode)")
{
    WriteVideoMode(base, ctx.r3.u32);
}
