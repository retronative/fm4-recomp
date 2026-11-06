// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/dispatcher.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Dispatcher objects in guest memory (M3-T05): an X_DISPATCH_HEADER (type byte at +0, signal
// state big-endian at +4) is waited on and signalled under one host lock, so the guest-visible
// signal state stays the source of truth. REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f:
// kernel/xobject.h X_DISPATCH_HEADER; xboxkrnl_threading.cc KeSetEvent/KeWaitForSingleObject),
// re-derived. Event types: 0 notification (stays signalled), 1 synchronization (auto-reset: one
// waiter consumes the signal); timers (8 notification, 9 synchronization) behave alike.
#pragma once
#include <cstdint>

namespace runtime {

namespace dispatch { constexpr uint8_t kNotificationEvent = 0, kSynchronizationEvent = 1, kMutant = 2, kSemaphore = 5,
                                       kNotificationTimer = 8, kSynchronizationTimer = 9;
                     constexpr uint32_t kSignalState = 4, kSemaphoreLimit = 0x10; }

// Adds to a semaphore's count; false (count unchanged) if it would pass the limit. `previous`
// receives the count before.
bool ReleaseSemaphore(uint8_t* base, uint32_t header, int32_t count, int32_t& previous);

// Sets an event's signal state and wakes waiters; returns the previous state.
int32_t SetEvent(uint8_t* base, uint32_t header);
// Clears an event's signal state; returns the previous state.
int32_t ResetEvent(uint8_t* base, uint32_t header);
// Blocks until the event is signalled; a synchronization event is reset by the wait it ends.
void WaitForEvent(uint8_t* base, uint32_t header);

// Waits for a dispatcher object (an event, or a thread, signalled when it ends) until it is
// signalled or the timeout passes: in 100 ns units, negative relative, positive an absolute
// system time, null infinite. Returns true if signalled. A synchronization event is reset by the
// wait it satisfies. Other object types stop until implemented.
// `thread`: the waiting thread's KTHREAD, which a mutant needs (0: none known; a mutant then stops).
// A mutant (KMUTANT: signal state 1 when free, 1 - recursion count when owned; owner at +0x18) is
// signalled when free or owned by the waiting thread; a wait it satisfies takes one level of it
// for that thread (NT; INFERRED: Xenia uses a host mutex).
bool WaitForObject(uint8_t* base, uint32_t header, const int64_t* timeout, uint32_t thread = 0);

namespace wait { constexpr uint32_t kSuccess = 0, kTimeout = 0x102, kSemaphoreLimitExceeded = 0xC0000047; }

// Waits for several dispatcher objects: any (the first signalled, in list order, satisfies the
// wait; returns its index, i.e. STATUS_WAIT_0 + index) or all (all signalled at once, each then
// consumed; returns kSuccess); kTimeout if the timeout passes. An object listed twice in a wait
// for all stops (NT rejects it; not modelled).
uint32_t WaitForObjects(uint8_t* base, const uint32_t* headers, uint32_t count, bool all, const int64_t* timeout, uint32_t thread = 0);

// Signals one object (an event is set; a semaphore's count goes up by one) and waits for another,
// as one step under the dispatcher lock, so no other thread acts in between (NT's
// NtSignalAndWaitForSingleObject). Returns kSuccess, kTimeout, or kSemaphoreLimitExceeded (no
// signal, no wait) when the semaphore is at its limit. Signalling other object types stops.
uint32_t SignalAndWaitForObject(uint8_t* base, uint32_t signal, uint32_t header, const int64_t* timeout, uint32_t thread = 0);

// Releases one level of a mutant held by `thread`: false (nothing changed) if the thread does not
// own it (STATUS_MUTANT_NOT_OWNED); the owner is cleared when the last level is released.
// `previous` receives the signal state before.
bool ReleaseMutant(uint8_t* base, uint32_t header, uint32_t thread, int32_t& previous);
constexpr uint32_t kMutantOwner = 0x18;

}  // namespace runtime
