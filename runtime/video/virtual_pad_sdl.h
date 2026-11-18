// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (test only, opt-in FM4_TEST_VIRTUAL_PAD): a virtual SDL gamepad driven from a named pipe (virtual_pad_sdl.cpp).
#pragma once

namespace runtime::video::virtual_pad {
void Start();  // after SDL_Init with the gamepad subsystem; nothing without FM4_TEST_VIRTUAL_PAD
void Poll();   // every window loop turn: the pipe's pending lines
}  // namespace runtime::video::virtual_pad
