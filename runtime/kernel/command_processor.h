// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The headless command processor (M3: CPU only, no GPU work): it reads the guest's ring buffer and executes
// PM4 packets as far as they matter for the guest to progress (register writes, memory writes, waits, fences,
// interrupts), counts draws without drawing, and stops at the first swap. Semantics: Xenia xenia-canary
// 67f16c4e315f (src/xenia/gpu/: pm4_command_processor_implement.h,
// command_processor.cc, xenos.h); design: reference/gpu/gpu_path.md.
#pragma once
#include <cstddef>
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
    // FM4 Recomp: before a packet the guest CPU can synchronise on (interrupt, REG_TO_MEM, MEM_WRITE, COND_WRITE, WAIT_REG_MEM,
    // EVENT_WRITE_SHD/EXT/ZPD): GPU writes still pending land in guest memory first (renderer.h SyncGuestWrites). Not before a
    // plain EVENT_WRITE: it sets VGT_EVENT_INITIATOR only and stores nothing the CPU can poll. Nullable.
    void (*syncGuestWrites)(uint32_t opcode);
    // FM4 Recomp (FM4_DEFER_FENCES, ReCarbon's model: memory stores of the command stream land at GPU completion): true when the
    // store of the 4 bytes at `physical` waits for the open submission (the renderer marked them pending); then the command
    // processor queues it (ApplyDeferredStores). Nullable.
    bool (*deferStore)(uint32_t physical);
    // FM4 Recomp: the ring is drained while deferred stores wait; the renderer submits and retires its open work, so a guest
    // thread polling one of them (a D3D fence) sees it. Nullable.
    void (*ringIdle)();
    // FM4 Recomp: whether a GPU write or a deferred store to [physical, physical + bytes) still waits for the open submission (a
    // memory WAIT_REG_MEM on other memory polls at once and syncs only if unsatisfied). Nullable: then every memory wait syncs.
    bool (*pendingAt)(uint32_t physical, uint32_t bytes);
    // FM4 Recomp (FM4_DEFER_FENCES): true while the renderer has recorded work not yet submitted; an INTERRUPT then waits in the
    // deferred queue (raised at the retire, after the stores before it; ReCarbon command_processor.cpp:480-496). Nullable.
    bool (*workOpen)();
    // FM4 Recomp (M7, FM4_GUEST_MEMORY_GPU=1; runtime/video/gpu_read_guard.h G5): a guest thread waits for GPU reads of a page it
    // writes; the renderer submits and retires up to the part it needs. Called between packets, in every WAIT_REG_MEM poll and
    // when the ring is idle (no command buffer handle is live at any of them). Nullable.
    void (*honourGpuReadGuard)();
    // FM4 Recomp (FM4_GUEST_MEMORY_GPU=1): before this thread stores to guest physical memory (a stream store, a scratch register's
    // write-back, the read pointer's write-back), so that a store over a range unretired GPU work reads flushes and waits here,
    // outside the signal handler. Nullable.
    void (*beforeCpWrite)(uint32_t physical, uint32_t bytes);
};
// FM4 Recomp (FM4_GUEST_MEMORY_GPU): whether the calling thread is the command processor's (async-signal-safe: pthread_self).
bool OnCommandProcessorThread();
// FM4 Recomp (FM4_ASYNC_RETIRE): the renderer's completion worker registers itself (runtime/video/async_retire.h); a store of its
// resolve write-backs that a later GPU part still reads stops at once, as the command processor's does (guest_memory_vk.cpp).
void SetCompletionWorkerThread();
bool OnCompletionWorkerThread();
// FM4 Recomp (FM4_DEFER_FENCES): the queued stores in packet order, into guest memory (the renderer, after the submission that
// was open when they were queued is confirmed complete and its resolve write-backs landed). Command-processor thread.
void ApplyDeferredStores();
bool DeferredStoresPending();
// FM4 Recomp (opt-in FM4_ASYNC_RETIRE=1; deferred_store_parts.h P1-P5): at each part's submission the entries queued since the
// previous one become that part's; once the renderer has retired parts up to `serial` (fence, write-backs), their stores land
// from the front of the queue, stopping at the first interrupt (raised by the full drain, ApplyDeferredStores, after every prior
// part), at the first entry of a later or unsubmitted part, and at the first store `canStore(physical)` refuses (null: none
// refused). Returns the stores applied. Command-processor thread, never while a command buffer handle is live.
void MarkDeferredStoresPart(uint64_t serial);
size_t ApplyDeferredStoresThrough(uint64_t serial, bool (*canStore)(uint32_t physical));
// FM4 Recomp (FM4_ASYNC_RETIRE): called between packets, in every WAIT_REG_MEM poll and when the ring is idle, where
// honourGpuReadGuard is (the renderer reaps the parts its completion worker retired). Null (the default) changes nothing.
void SetReapHook(void (*reap)());
// FM4 Recomp (diagnostic): nanoseconds the command processor spent with nothing to execute (the ring drained), since start.
uint64_t CpIdleNs();
// FM4 Recomp (FM4_DEFER_FENCES): WAIT_REG_MEM packets a queued store of the stream satisfied without a flush, since start.
uint64_t SelfWaits();
// FM4 Recomp: memory WAIT_REG_MEM packets satisfied at the first poll with nothing pending at their word (no flush), since start.
uint64_t ImmediateWaits();
// FM4 Recomp: memory WAIT_REG_MEM packets that were not satisfied at once, their total time, and how many of them synced
// (submitted and waited for the open work), since start.
uint64_t MemWaits();
uint64_t MemWaitNs();
uint64_t SyncedWaits();
// FM4 Recomp (FM4_DIAG_DRAIN=1, diagnostic; runtime/video/diag_drain.h): the start and end (steady_clock ns) of the latest of the last
// 64 memory WAIT_REG_MEM packets that synced whose span holds `ns`; false when none does or the diagnostic is off. Command-processor
// thread only (the waits are recorded there).
bool SyncedMemWaitAround(uint64_t ns, uint64_t& startNs, uint64_t& endNs);
// FM4 Recomp (FM4_IMMEDIATE_INTERRUPTS): INTERRUPT packets raised when reached, and CPU-word waits that synced after the grace.
uint64_t ImmediateInterrupts();
uint64_t GraceSyncs();
// Overrides FM4_IMMEDIATE_INTERRUPTS (tests).
void SetImmediateInterrupts(bool on);
// Overrides FM4_DEFER_EVENTS (tests).
void SetDeferEvents(bool on);
// Tests: FM4_BULK_REGISTERS on or off (command_processor.cpp WriteRegisterRun).
void SetBulkRegisters(bool on);
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
