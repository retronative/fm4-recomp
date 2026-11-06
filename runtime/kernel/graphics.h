// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The GPU as the guest sees it, headless (M3: no GPU work): its register page and the engine state the
// Vd* imports set up. Design: reference/gpu/gpu_path.md; semantics from Xenia xenia-canary 67f16c4e315f
// (src/xenia/gpu/graphics_system.cc, register_table.inc).
#pragma once
#include <cstdint>

namespace runtime::gpu {

// The GPU registers are a 64 KB window at 7FC80000: register r at 7FC80000 + 4r (Xenia graphics_system.cc:98-101
// AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF)). Here it is plain committed guest memory: the guest's
// stores land in it and the command processor reads them (big-endian, as the guest wrote them).
// The command processor's register file is larger than that window (0x5003 registers: ALU constants from
// 0x4000, fetch constants from 0x4800; Xenia register_table.inc), so the whole file is committed from
// 7FC80000: guest stores reach its first 64 KB.
constexpr uint32_t kRegisterCount = 0x5003, kRegisterBase = 0x7FC80000u, kRegisterBytes = 0x18000u;
constexpr uint32_t RegisterAddress(uint32_t index) { return kRegisterBase + 4 * index; }

// Register indices (Xenia register_table.inc).
namespace reg { constexpr uint32_t kCpRbWptr = 0x1C5, kScratchUmsk = 0x1DC, kScratchAddr = 0x1DD, kScratchReg0 = 0x578,
                                   kWritebackBase = 0xA04, kCoherStatusHost = 0xA31, kVgtEventInitiator = 0x21F9,
                                   kShaderConstantFetch00 = 0x4800; }

// Commits the register page and seeds the registers Xenia reads as constants (graphics_system.cc:209-224):
// RB_EDRAM_TIMING 0F00 = 08100748, RB_BC_CONTROL 0F01 = 0000200E, D1MODE_V_COUNTER 194C = 2D0, the interrupt
// status 1951 = 1 (vblank), D1MODE_VIEWPORT_SIZE 1961 = 050002D0 (1280x720).
void InitializeRegisters(uint8_t* base);
uint32_t ReadRegister(const uint8_t* base, uint32_t index);
void WriteRegister(uint8_t* base, uint32_t index, uint32_t value);

// The engines (VdInitializeEngines): HLE; the runtime's command processor is the engine, so the microcode
// the guest passes is recorded but not run.
struct Engines { bool initialized = false; uint32_t callback = 0, callbackArg = 0, pfp = 0, me = 0, gpuIdentifierAddress = 0; };
Engines& EngineState();

// Interrupts (VdSetGraphicsInterruptCallback; Xenia graphics_system.cc:262-300). The guest callback runs as
// callback(source, user data) on one guest service thread on CPU 2 (Xenia DispatchInterruptCallback picks CPU
// 2), one interrupt at a time. Source 0 is the vblank: a 60 Hz host timer increments the
// vblank counter and queues it (Xenia MarkVblank, 60 Hz vsync worker :103-145); a vblank already waiting is not
// queued twice. Source 1 comes from a PM4 INTERRUPT packet. The first registration starts the thread and the
// timer; a later one replaces the callback.
void SetInterruptCallback(uint8_t* base, uint32_t callback, uint32_t userData);
// Queues an interrupt; with `wait`, returns after the callback has run.
void RequestInterrupt(uint32_t source, bool wait);
uint64_t VblankCount();
uint64_t InterruptsDelivered();

}  // namespace runtime::gpu
