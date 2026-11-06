// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/thread_state.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// The guest-visible state of a thread and its process (M3-T01): the process block, and per thread
// a PCR (r13), a KTHREAD (PCR+0x100), a TLS block (PCR+0) and a stack. Layouts and initial values
// are REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f: kernel/xthread.{h,cc} X_KPCR,
// X_KTHREAD, XThread::InitializeGuestObject; kernel/kernel_state.{h,cc} ProcessInfoBlock),
// re-derived here; field meanings Xenia marks unknown stay unknown. FM4 corroborates the TLS
// model: its code reads the static TLS data at PCR[0] + 0x1810..0x181C, inside the executable's
// 0x1820-byte TLS data. Every multi-byte field is big-endian.
#pragma once
#include <cstdint>
#include <vector>

namespace runtime {

// The executable's TLS directory (XEX_HEADER_TLS_INFO).
struct TlsInfo { uint32_t slotCount, rawDataAddress, dataSize, rawDataSize; };

constexpr uint32_t kProcessBlockSize = 0x60, kPcrSize = 0x2D8, kKthreadSize = 0xAB0;
constexpr uint8_t kProcessTypeTitle = 1;  // Xenia X_PROCTYPE_TITLE

// Offsets used by the runtime and its tests.
namespace pcr { constexpr uint32_t kTls = 0x0, kSelf = 0x30, kStackBase = 0x70, kStackEnd = 0x74,
                                   kCurrentIrql = 0x18, kCurrentThread = 0x100, kCurrentCpu = 0x10C, kDpcActive = 0x150; }
namespace kthread { constexpr uint32_t kStackBase = 0x5C, kStackLimit = 0x60, kTls = 0x68,
                                       kPriority = 0x70, kProcess = 0x84, kApcDisableCount = 0xB0, kCurrentCpu = 0xBF, kThreadId = 0x14C,
                                       kStartAddress = 0x150, kLastError = 0x160, kFiber = 0x164,
                                       kCreationFlags = 0x16C; }
namespace process { constexpr uint32_t kTlsDataSize = 0x24, kTlsRawDataSize = 0x28, kTlsSlotSize = 0x2C,
                                       kProcessType = 0x2F; }

struct ThreadState { uint32_t pcr, kthread, tls, stackLimit, stackBase; };

// The process block of the title process (one per run).
uint32_t CreateProcessBlock(uint8_t* base, const TlsInfo& tls);

// A thread's PCR, KTHREAD, TLS (static data copied from the executable's template, then the
// dynamic slots) and stack, initialised as the reference does for a new thread.
ThreadState CreateThreadState(uint8_t* base, const TlsInfo& tls, uint32_t process, uint32_t stackSize,
                              uint32_t threadId, uint32_t startAddress, uint32_t creationFlags, uint8_t cpu);

// Every thread created so far (their KTHREADs), for operations over all threads.
std::vector<uint32_t> AllThreads();
// The state of the thread with this KTHREAD, if the runtime created it.
bool FindThread(uint32_t kthread, ThreadState& state);

// The guest system time (Windows FILETIME: 100 ns intervals since 1601-01-01 UTC) from the host clock.
uint64_t GuestSystemTime();

}  // namespace runtime
