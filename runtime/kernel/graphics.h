// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The GPU as the guest sees it, headless (M3: no GPU work): its register page and the engine state the
// Vd* imports set up. Design: reference/gpu/gpu_path.md; semantics from Xenia xenia-canary 67f16c4e315f
// (src/xenia/gpu/graphics_system.cc, register_table.inc).
#pragma once
#include "gamma_ramp.h"
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
namespace reg { constexpr uint32_t kCpRbWptr = 0x1C5, kScratchUmsk = 0x1DC, kScratchAddr = 0x1DD, kScratchReg0 = 0x578, kWaitUntil = 0x5C8,
                                   kWritebackBase = 0xA04, kCoherStatusHost = 0xA31, kVgtEventInitiator = 0x21F9,
                                   kPaScVizQueryStatus0 = 0x0C44, kPaScVizQueryStatus1 = 0x0C45,  // FM4 Recomp (alpha): X register_table.inc:100-102
                                   kRbSampleCountAddr = 0x2325,  // FM4 Recomp: X register_table.inc:615
                                   kVgtDmaBase = 0x21FA, kVgtDmaSize = 0x21FB, kVgtDrawInitiator = 0x21FC,
                                   kShaderConstantFetch00 = 0x4800; }

// Commits the register page and seeds the registers Xenia reads as constants (graphics_system.cc:209-224):
// RB_EDRAM_TIMING 0F00 = 08100748, RB_BC_CONTROL 0F01 = 0000200E, D1MODE_V_COUNTER 194C = 2D0, the interrupt
// status 1951 = 1 (vblank), D1MODE_VIEWPORT_SIZE 1961 = 050002D0 (1280x720).
void InitializeRegisters(uint8_t* base);
uint32_t ReadRegister(const uint8_t* base, uint32_t index);
void WriteRegister(uint8_t* base, uint32_t index, uint32_t value);

// The display gamma ramp (gamma_ramp.h), as Xenia's command processor keeps it beside the register file
// (command_processor.cc:83-100, :469-574). Called after register `index` (DC_LUT_RW_INDEX..DC_LUT_30_COLOR, otherwise
// nothing) was written with `value`: runs the state machine on the register file's DC_LUT_RW_MODE, DC_LUT_WRITE_EN_MASK
// and DC_LUT_RW_INDEX (which it updates). Xenia runs it for the command processor's register writes only
// (CommandProcessor::WriteRegister :589-606; its MMIO handler, graphics_system.cc:235-251, stores the value without
// it); here a CPU device store (recomp/mmio.h) runs it too, since both reach the same hardware register, and each
// path is counted so a run shows which one FM4 uses. A data write in the other RW_MODE stops (7), naming it.
enum class LutWriter { CommandProcessor, Cpu };
void OnGammaRampRegisterWritten(uint8_t* base, uint32_t index, uint32_t value, LutWriter writer);
struct GammaRampView { gamma::Ramp ramp; uint64_t cpWrites = 0, cpuWrites = 0; };
GammaRampView CurrentGammaRamp();

// The engines (VdInitializeEngines): HLE; the runtime's command processor is the engine, so the microcode
// the guest passes is recorded but not run.
// The GPU stop flag (fm4recomp --gpu-stop, required): every GPU entry point checks it first (gpu/gpu_stop.h);
// VdInitializeEngines is the first, and the renderer (M4) will start from it.
void SetGpuStopPath(const char* path);
const char* GpuStopPath();

struct Engines { bool initialized = false; uint32_t callback = 0, callbackArg = 0, pfp = 0, me = 0, gpuIdentifierAddress = 0; };
Engines& EngineState();

// Interrupts (VdSetGraphicsInterruptCallback; Xenia graphics_system.cc:262-300). The guest callback runs as
// callback(source, user data) on one guest service thread on CPU 2 (Xenia DispatchInterruptCallback picks CPU
// 2), one interrupt at a time. Source 0 is the vblank: a 60 Hz host timer increments the
// vblank counter and queues it (Xenia MarkVblank, 60 Hz vsync worker :103-145); a vblank already waiting is not
// queued twice. Source 1 comes from a PM4 INTERRUPT packet. The first registration starts the thread and the
// timer; a later one replaces the callback.
void SetInterruptCallback(uint8_t* base, uint32_t callback, uint32_t userData);
// Queues an interrupt; with `wait`, returns after the callback has run. FM4 Recomp (FM4_GUEST_MEMORY_GPU): with `poll`, the wait
// calls it every 50 us without the interrupt lock (the callback may store to a page whose GPU read the caller must retire).
void RequestInterrupt(uint32_t source, bool wait, void (*poll)() = nullptr);
uint64_t VblankCount();
uint64_t InterruptsDelivered();

}  // namespace runtime::gpu
