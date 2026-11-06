// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XOVERLAPPED (Xenia's XAM_OVERLAPPED) completion for asynchronous XAM calls. REFERENCE_DERIVED from Xenia
// (xenia-canary 67f16c4e315f: xbox.h:182-231 layout and accessors; kernel/kernel_state.cc:763-809
// CompleteOverlapped, CompleteOverlappedEx, CompleteOverlappedImmediate(Ex); kernel/xam/xam_msg.cc:63-64 the
// "complete, then return ERROR_IO_PENDING" shape), re-derived over objects.h and dispatcher.h.
//
// Differences from Xenia, by design (each a compatibility policy, POLICY.md):
// - Completion is always immediate: the fields are written and the event set before the import returns. Xenia's
//   CompleteOverlappedDeferred (kernel_state.cc:811-878: result ERROR_IO_PENDING, event reset, a dispatch thread
//   that sleeps 100 ms and then completes) is not modelled. ReCarbon (runtime/kernel/xam.cpp XamComplete)
//   and ForzaRecomp (imports_user.cpp, which FM4 ran past) complete immediately too.
// - A completion routine (an APC queued to the requesting thread, kernel_state.cc:782-789) is not modelled: the
//   runtime queues no APC anywhere (threads.h). A non-zero routine stops BEFORE any field is written, so the guest
//   state at the stop is the state the title passed in.
// - The event handle must name an event object; anything else stops (Xenia asserts, then skips the Set).
// - The context field (+8): Xenia's Immediate form writes the current thread's handle (kernel_state.cc:807), used
//   only to route a completion routine, which stops here: the field is left as the title set it (nothing invented).
#pragma once
#include <cstdint>

struct PPCContext;

namespace runtime {

namespace overlapped {
// XAM_OVERLAPPED, big-endian (xbox.h:183-191).
constexpr uint32_t kResult = 0x0, kLength = 0x4, kContext = 0x8, kEvent = 0xC, kCompletionRoutine = 0x10,
                   kCompletionContext = 0x14, kExtendedError = 0x18, kSize = 0x1C;
// Win32 codes (xbox.h:101-102).
constexpr uint32_t kErrorIoIncomplete = 0x3E4, kErrorIoPending = 0x3E5;
}  // namespace overlapped

// Stops (7) if the XOVERLAPPED at `address` (non-zero) cannot be completed by this runtime: a completion routine,
// or an event field that is not 0 and not an event handle. Writes nothing. An import calls it before any side
// effect (output buffers, notifications), so a stop leaves the guest's state as it was.
void CheckOverlapped(const PPCContext& ctx, const uint8_t* base, uint32_t address, const char* import);

// Xenia CompleteOverlappedEx (kernel_state.cc:767-791) (without ImmediateEx's context write, 802-809): checks
// (as CheckOverlapped), writes the result, extended error and length, then sets the event. The fields are written before the event is set, so a thread woken by the event reads the completed state.
void CompleteOverlappedEx(const PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t result, uint32_t extendedError,
                          uint32_t length, const char* import);

// Xenia CompleteOverlappedImmediate (kernel_state.cc:793-800): extended error = result; length 0 on success, else
// 0xFFFFFFFF (Xenia's note: "there are games that check 'length' of overlapped as an indication of success ...
// setting length to -1 when not success seems to be helping": an unexplained workaround, kept only because an
// import citing Xenia's use of this form must reproduce what Xenia wrote; new imports pass explicit values to Ex).
void CompleteOverlappedImmediate(const PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t result, const char* import);

// The value an asynchronous XAM import returns (Xenia xam_msg.cc:63-64, xam_content.cc:282-297 pattern): without an
// XOVERLAPPED, `result`; with one, the XOVERLAPPED is completed (Ex form, explicit values) and the import returns
// ERROR_IO_PENDING (0x3E5), the guest reading the result through XamGetOverlappedResult or the fields.
uint32_t FinishOverlapped(const PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t result, uint32_t extendedError,
                          uint32_t length, const char* import);

}  // namespace runtime
