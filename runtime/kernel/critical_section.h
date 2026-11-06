// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/critical_section.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Critical sections (M3-T03): the 28-byte XDK layout, every field big-endian
// (critical_section.cpp).
#pragma once
#include <cstdint>

namespace runtime {

// Initializes the critical section at guest address `cs` (RtlInitializeCriticalSectionAndSpinCount).
void InitializeCriticalSection(uint8_t* base, uint32_t cs, uint32_t spinCount);

}  // namespace runtime
