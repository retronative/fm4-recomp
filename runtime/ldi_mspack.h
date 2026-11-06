// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// libmspack's LZX decoder as the LDI decoder (kernel/ldi.h).
#pragma once
#include "kernel/ldi.h"

namespace runtime::ldi {
Factory MspackDecoderFactory();
}  // namespace runtime::ldi
