// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, diagnostic FM4_DIAG_THREAD_CPU=1): CPU time of the process's threads by role since the previous call, from the
// kernel's per-thread accounting (Mach thread_info THREAD_BASIC_INFO: user + system time). The role comes from the thread name:
// "fm4 guest" (threads.cpp: every guest thread), "fm4 gpu cp" (command_processor.cpp: the ring's thread, which also runs the
// renderer), anything else "other" (audio, Metal, the main thread). A thread that exited between two calls loses its last part.
#pragma once
#include <cstdint>
#include <string_view>

namespace runtime {

struct ThreadCpu
{
    double guestMs = 0, cpMs = 0, otherMs = 0;
    uint32_t guestThreads = 0;
};
ThreadCpu ThreadCpuSince();
// The role of a thread name (ThreadCpuSince's classification): 0 guest, 1 command processor, 2 other.
int ThreadRole(std::string_view name);

}  // namespace runtime
