// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M9 experiment, opt-in FM4_THREAD_QOS=interactive | initiated; off by default): the quality-of-service class of the
// game's own threads (the guest threads, the GPU command processor, the retire and submit workers). Without it they have none
// (macOS then schedules them as default work). HYPOTHESIS under test: the system's CPU and GPU performance controllers follow the
// class of the threads that do and submit the work (OBSERVED 2026-11-21: the GPU stays at 338-636 of 1578 MHz during a race).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace runtime {

// On the thread itself, at its start. Nothing unless the variable names a class; another spelling is named once and ignored.
inline void ApplyThreadQos()
{
    static const int chosen = [] {
        const char* v = std::getenv("FM4_THREAD_QOS");
        if (v == nullptr || *v == '\0')
            return 0;
        const int c = std::strcmp(v, "interactive") == 0 ? 1 : std::strcmp(v, "initiated") == 0 ? 2 : -1;
        std::fprintf(stderr, c > 0 ? "[threads] FM4_THREAD_QOS=%s: the game's threads take that quality-of-service class\n"
                                   : "[threads] FM4_THREAD_QOS '%s' ignored: use interactive or initiated\n", v);
        return c;
    }();
    if (chosen > 0)
        pthread_set_qos_class_self_np(chosen == 1 ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
}

}  // namespace runtime
