// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
#include "thread_cpu.h"
#include <mach/mach.h>
#include <pthread.h>
#include <mutex>
#include <unordered_map>

namespace runtime {

int ThreadRole(std::string_view name)
{
    if (name.starts_with("fm4 guest"))
        return 0;
    if (name == "fm4 gpu cp")
        return 1;
    return 2;
}

ThreadCpu ThreadCpuSince()
{
    static std::mutex lock;
    static std::unordered_map<uint64_t, uint64_t> last;  // thread id -> CPU nanoseconds at the previous call
    std::lock_guard guard(lock);
    ThreadCpu out;
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return out;
    for (mach_msg_type_number_t i = 0; i < count; i++)
    {
        thread_basic_info_data_t info;
        mach_msg_type_number_t size = THREAD_BASIC_INFO_COUNT;
        thread_identifier_info_data_t id;
        mach_msg_type_number_t idSize = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &size) == KERN_SUCCESS &&
            thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&id), &idSize) == KERN_SUCCESS)
        {
            const uint64_t ns = (uint64_t(info.user_time.seconds) + uint64_t(info.system_time.seconds)) * 1000000000ull +
                                (uint64_t(info.user_time.microseconds) + uint64_t(info.system_time.microseconds)) * 1000ull;
            uint64_t& previous = last[id.thread_id];
            const double ms = (ns - std::min(ns, previous)) / 1e6;
            previous = ns;
            char name[64] = {};
            if (pthread_t p = pthread_from_mach_thread_np(threads[i]); p != nullptr)
                pthread_getname_np(p, name, sizeof name);
            switch (ThreadRole(name))
            {
            case 0: out.guestMs += ms, out.guestThreads++; break;
            case 1: out.cpMs += ms; break;
            default: out.otherMs += ms; break;
            }
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), vm_address_t(threads), count * sizeof(thread_act_t));
    return out;
}

}  // namespace runtime
