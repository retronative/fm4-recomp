// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The headless command processor (M3: CPU only, no GPU work): it reads the guest's ring buffer and executes
// PM4 packets as far as they matter for the guest to progress (register writes, memory writes, waits, fences,
// interrupts), counts draws without drawing, and stops at the first swap. Semantics: Xenia xenia-canary
// 67f16c4e315f (src/xenia/gpu/: pm4_command_processor_implement.h,
// command_processor.cc, xenos.h); design: reference/gpu/gpu_path.md.
#pragma once
#include <cstdint>

namespace runtime::gpu {

// Physical memory as the GPU addresses it (Xenia TranslatePhysical, 29 bits): the guest address of the view whose
// allocation holds the frame (runtime::PhysicalToGuest; the E0000000 view has its own memory, so the view
// matters). A frame no view holds stops (7), naming the address.
uint32_t PhysicalToGuest(uint32_t physical);

// A 32-bit word at a GPU address whose low 2 bits select the swap (xenos.h Endian: 0 none, 1 8in16, 2 8in32,
// 3 16in32), as Xenia's GpuSwap applies it to the host-order value (pm4 REG_TO_MEM, MEM_WRITE, EVENT_WRITE_SHD).
uint32_t GpuSwap(uint32_t value, uint32_t endian);
uint32_t LoadGpu(const uint8_t* base, uint32_t address);
void StoreGpu(uint8_t* base, uint32_t address, uint32_t value);

// Executor state shared by the ring and its indirect buffers.
struct CpState
{
    uint64_t binMask = ~0ull, binSelect = ~0ull;
    uint64_t packets = 0, draws = 0, swaps = 0;
    uint32_t lastHeaders[16] = {}, lastCount = 0;  // the hang report
    uint32_t waitingOn = 0;                        // the address or register a WAIT_REG_MEM polls (0: none)
};

// The renderer's hook points (M4; runtime/video/renderer.h), called from the command processor's thread. Null
// (the default: headless, and the runtime tests, which build kernel/ without Vulkan) changes nothing: the
// renderer never changes the command processor's control flow, so headless and GPU runs execute the same
// packets, draws, swaps and fences (execution model §3).
struct RendererHooks
{
    // XE_SWAP, after the slot's type-0 write of the front buffer's fetch constant (registers 4800-4805, host order).
    void (*present)(const uint8_t* base, const uint32_t fetch[6], uint32_t physical, uint32_t width, uint32_t height, uint64_t swap);
    // DRAW_INDX, DRAW_INDX_2, after the packet's words are in VGT_DRAW_INITIATOR, VGT_DMA_BASE and VGT_DMA_SIZE (as Xenia
    // writes them, pm4_command_processor_implement.h:1003-1034); `vizQueryToken` is DRAW_INDX's first word, 0 for DRAW_INDX_2.
    void (*draw)(const uint8_t* base, uint32_t opcode, uint32_t vizQueryToken);
    void (*loadShader)(uint32_t type, const uint32_t* words, uint32_t count);  // IM_LOAD(_IMMEDIATE), host order
};
void SetRendererHooks(const RendererHooks* hooks);

// The run stops (11) at swap number `n` (fm4recomp --stop-at-swap n, required; M3's evidence is n = 1).
void SetStopAtSwap(uint32_t n);

// Executes `words` 32-bit words from a buffer starting at guest address `start` that is `size` words long and
// read from word `from` with wraparound (a ring; an indirect buffer has size == words and from 0). Stops (11) at
// an XE_SWAP packet; stops (7) at an opcode not modelled, a packet longer than the data, or a wait that is never
// satisfied (10 s).
void Execute(uint8_t* base, uint32_t start, uint32_t size, uint32_t from, uint32_t words, CpState& state);

// Writes a register as the command processor does: scratch registers enabled in SCRATCH_UMSK are also written
// big-endian to SCRATCH_ADDR + 4 n (Xenia command_processor.cc:448-459); a write to COHER_STATUS_HOST sets its
// status bit 31 (:465-467); a write to DC_LUT_RW_INDEX..DC_LUT_30_COLOR runs the gamma ramp's state machine (:469-574,
// graphics.h OnGammaRampRegisterWritten).
void CpWriteRegister(uint8_t* base, uint32_t index, uint32_t value);

// The ring (VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack; Xenia command_processor.cc:340-358): the
// first call zeroes the ring and starts the worker, which executes from its read index to CP_RB_WPTR and then
// writes the read index big-endian to the write-back address. A second ring stops (7).
void InitializeRing(uint8_t* base, uint32_t physical, uint32_t sizeLog2);
void EnableReadPointerWriteBack(uint32_t physical, uint32_t blockSizeLog2);
// The command processor's state for the hang report.
void ReportCommandProcessor();

}  // namespace runtime::gpu
