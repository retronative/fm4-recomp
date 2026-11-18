// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (diagnostic, FM4_DIAG_DRAIN=1): where the time of each "CPU sync" drain goes (SyncGuestWrites -> FlushSlot(0, Sync):
// the command processor ends the open part, submits it and waits for every submitted part to retire). Each drain is split into
//   (1) cp -> hand:    the command processor ends the open part (render pass, end timestamp, vkEndCommandBuffer) until it hands the
//                      drain's last part to the submission (SubmitSlot, before GpuGuard::Submit);
//   (2) submit:        queueing on the submit worker (FM4_ASYNC_SUBMIT: earlier parts' submits, the presenter's queue lock) and the
//                      last part's vkResetFences + vkQueueSubmit (MoltenVK's Metal encoding);
//   (3) gpu:           vkQueueSubmit returned -> vkWaitForFences returned for the last part (the completion worker with
//                      FM4_ASYNC_RETIRE, else the command processor);
//   (4) fence -> cp:   the fence observed -> the command processor's GpuGuard::Retire of the last part returned (worker jobs:
//                      the read guard's completion and the resolve write-backs; the wake-up), then the rest of RetireSlot
//                      (deferred stores and the interrupts they raise, the xe_error scan);
// plus, from the command processor's own record of the memory WAIT_REG_MEM that synced (command_processor.h SyncedMemWaitAround),
// the time before the flush and the poll tail after it (the interrupt service, the guest callback that clears the word, the poll
// interval). Behaviour is unchanged; only "[diag drain]" lines are printed. Off: one cached getenv per hook, nothing recorded.
// Timestamps: std::chrono::steady_clock nanoseconds, the command processor's clock (command_processor.cpp NowNs).
#pragma once
#include <cstdint>
#include <vector>

namespace runtime::video::drain {

bool Enabled();  // FM4_DIAG_DRAIN=1
uint64_t NowNs();

// Per part slot (any thread). The command processor, in SubmitSlot before GpuGuard::Submit: the part's serial and draws; clears the
// slot's later stamps (a slot is submitted again only after it retired, so they belong to this submission).
void PartHanded(uint32_t slot, uint64_t serial, uint64_t draws);
// The submit worker (FM4_ASYNC_SUBMIT), its queue-lock callback: the time it waited for the presenter's queue lock just before the
// next Submit; taken by the next SubmitBegin.
void QueueLockWaited(uint64_t ns);
// VulkanBackend::Submit (the submit worker, or the command processor without FM4_ASYNC_SUBMIT): around vkResetFences + vkQueueSubmit.
void SubmitBegin(uint32_t slot);
void SubmitEnd(uint32_t slot);
// VulkanBackend::WaitFence (the completion worker with FM4_ASYNC_RETIRE, else the command processor): around vkWaitForFences.
void FenceWaitBegin(uint32_t slot);
void FenceObserved(uint32_t slot);
// FM4_ASYNC_RETIRE: the completion worker's write-back job of the part with this serial.
void WriteBackBegin(uint64_t serial);
void WriteBackEnd(uint64_t serial);

// The command processor (SyncGuestWrites): one drain. `lastSlot` is the slot recording the open part, `openDraws` its draws,
// `submittedSlots` the parts submitted and not yet retired by the command processor.
void FlushBegin(uint32_t opcode, uint32_t lastSlot, uint64_t openDraws, const std::vector<uint32_t>& submittedSlots);
void RetireObserved();      // RetireSlot, after its GpuGuard::Retire loop (inside a drain only)
void GpuSpan(uint64_t ns);  // RetireSlot, FM4_GPU_TIMESTAMPS: the timed submission's GPU span (inside a drain only)
void FlushEnd();

// The command processor at the swap's timing lines: the swap's drains, individually for race swaps (>= 1000 draws; the first
// FM4_DIAG_DRAIN_DETAIL, default 30), then one summary line.
void Swap(uint64_t swap, uint64_t drawsSinceSwap);

}  // namespace runtime::video::drain
