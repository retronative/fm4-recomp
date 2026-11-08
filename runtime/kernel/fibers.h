// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The guest fiber switch's host side (fibers.cpp).
#pragma once
#include <cstdint>

namespace runtime {

// Forgets the suspended fiber whose guest stack has this top (DeleteFiber frees it through MmDeleteKernelStack) and
// unmaps its host stack; false if no fiber has it. Deleting the running fiber stops (7).
bool DiscardFiber(uint32_t stackTop);

}  // namespace runtime
