# GPU path brief: VdInitializeEngines to the first VdSwap, headless

Facts from an earlier FM4 port attempt that reached its first guest swap, and from a reference Xbox 360 port. Nothing
built or run for this brief.

## Imports (OBSERVED in the earlier attempt's video imports)
VdInitializeEngines (returns 1, starts 60 Hz vblank); VdSetGraphicsInterruptCallback; VdInitializeRingBuffer;
VdEnableRingBufferRPtrWriteBack; VdSetSystemCommandBufferGpuIdentifierAddress (records);
VdCallGraphicsNotificationRoutines (0; stops unless arg 1); VdIsHSIOTrainingSucceeded, VdRetrainEDRAM(Worker);
VdGetSystemCommandBuffer (0xBEEF0000/0001); VdSwap; VdSetDisplayMode (1280x720 RGB only);
VdGetCurrentDisplayInformation (0x58 bytes); VdInitializeScalerCommandBuffer (type-2 no-ops, count);
VdPersistDisplay. Others needed by then: VdGetCurrentDisplayGamma, VdQueryVideoFlags,
ExRegisterTitleTerminateNotification, RtlFillMemoryUlong, MmFreePhysicalMemory, KeWaitForSingleObject, KeInitializeDpc,
KeSetBasePriorityThread, KeEnter/LeaveCriticalRegion, NtYieldExecution, spinlocks, KiApcNormalRoutineNop.
Before that: aliased physical views 0xA0000000/0xC0000000 + MmGetPhysicalAddress. Swap 1 there: front buffer phys
1B441000, 1280x720, 148 packets, 48 draws, 5/5 runs. FM4 calls VdInitializeRingBuffer twice (second after swap 1).
Order between engines and swap: not recorded (let the strict stops drive it).

## Headless model (OBSERVED in both precedents)
- Register page 0x7FC80000 (guest stores to GPU registers); CP polls CP_RB_WPTR (0x1C5). Seed 0x0F00=08100748,
  0x0F01=0000200E, 0x194C=2D0, 0x1951=1, 0x1961=050002D0 (Xenia ReadRegister). Ours: unmapped at the time.
- Ring worker: read index -> CP_RB_WPTR with wrap; on catch-up writes the read index big-endian to the write-back address.
- Interrupts: callback(r3 = source, r4 = user data); source 0 = vblank on CPU 2 every 16.667 ms; source 1 from PM4
  INTERRUPT per CPU bit. The earlier attempt: one guest service thread, synchronous; sets PCR/KTHREAD current CPU first.
- VdSwap: the guest passes a reserved 64-word ring slot; VdSwap writes a type-0 SHADER_CONSTANT_FETCH_00_0 packet
  (fetch constant, physical address), XE_SWAP (0x64): "SWAP" 0x53574150, address, width, height; type-2 fill.
- Packets: type 0/1 registers (SCRATCH_REG with SCRATCH_UMSK bit -> memory BE); type 2, ME_INIT, NOP, INVALIDATE_STATE
  skip; INDIRECT_BUFFER(_PFD) execute; WAIT_REG_MEM poll (COHER_STATUS_HOST busy cleared); REG_RMW, REG_TO_MEM, MEM_WRITE,
  COND_WRITE with endian swap; EVENT_WRITE_SHD fence or vblank counter (bit 31); INTERRUPT source 1; SET_BIN_MASK/SELECT;
  IM_LOAD_IMMEDIATE, DRAW_INDX(_2) count; XE_SWAP stop; others stop with opcode; short packets stop.

## Precedent structure and pitfalls
A PM4 header, a command processor, the video imports and a graphics header; hand-built PM4 tests; 15 mutants.
Bugs found: COHER busy bit, big-endian scratch write-back; test bugs: type-1 header, 13-bit REG_RMW indices, ring
overwrite; TSan: ring config atomic. The reference port observed its second ring init stalling ~1 run in 2 (a CP ring race).

## M4 safeguards in the precedents
A watchdog launcher that refuses with GPU_STOP / other GPU work / thermal; mute; SHA-256; a resource limit; syslog +
gpuEvent report check; GPU_STOP on lockup; a lock directory. In-process fence timeout (a submission gate); a per-draw
loop-work guard (1<<26); a capped 16-slot dump ring. Lesson: a zsh `status` variable once disabled a watchdog silently.

## Risks / order (INFERENCE)
Synchronous dispatch deadlocks; unbounded WAIT_REG_MEM; ring re-init; vblank timing out of stop lines; physical views.
Order: pure Vd stubs; register page; pm4 decoder; ExecuteBuffer; ring worker + write-back; interrupts/vblank; VdSwap +
first-swap stop; CP state in the deadline report.
