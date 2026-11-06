// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/clock.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Guest clocks: time since boot (the runtime's start) and the kernel's KeTimeStampBundle, which
// the kernel refreshes every millisecond. REFERENCE_DERIVED from Xenia (xenia-canary
// 67f16c4e315f: kernel_state.{h,cc} X_TIME_STAMP_BUNDLE, UpdateKeTimestampBundle), unscaled.
#pragma once
#include <cstdint>

namespace runtime {

// The time base (mftb) frequency, also what KeQueryPerformanceFrequency reports: 49,875,000 Hz,
// the Xenon clock (3.192 GHz) / 64. REFERENCE_DERIVED from ReCarbon/Unleashed Recompiled
// (runtime/cpu/guest_time.h kTimebaseHz); Xenia uses 50,000,000 (emulator.cc), a round value.
constexpr uint64_t kTimebaseHz = 49875000;
uint64_t GuestTimebase();  // time base ticks since boot

uint64_t GuestInterruptTime();  // 100 ns intervals since boot
uint32_t GuestUptimeMs();       // milliseconds since boot

constexpr uint32_t kTimeStampBundleSize = 24;  // interrupt time, system time, tick count, padding
void WriteTimeStampBundle(uint8_t* base, uint32_t bundle);
// Writes the bundle now and keeps it current (every millisecond) for the rest of the run.
void StartTimeStampBundle(uint8_t* base, uint32_t bundle);

}  // namespace runtime
