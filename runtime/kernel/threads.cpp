// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/threads.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
#include "threads.h"
#include "dispatcher.h"
#include "guest_memory.h"
#include "imports.h"
#include "indirect.h"
#include "thread_state.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <memory>
#include <pthread.h>
#include <cstdio>

namespace runtime {
namespace {

struct ThreadExit { uint32_t code; };

uint32_t titleProcess = 0, defaultStack = 0;
TlsInfo titleTls{};
std::atomic<uint32_t> nextThreadId{2};  // 1 is the main thread
std::mutex suspendLock;
std::condition_variable resumed;
std::set<uint32_t> startedThreads;  // KTHREADs past their start (under suspendLock)

struct Start { uint8_t* base; ThreadState state; uint32_t xapiStartup, start, context; };

void* Run(void* argument)
{
    const Start s = *static_cast<Start*>(argument);
    delete static_cast<Start*>(argument);
    UseFaultStackOnThisThread();
    const uint32_t k = s.state.kthread;
    {
        char name[32];  // FM4 Recomp: "fm4 guest <id>" (thread_cpu.h roles; visible in sample and Instruments)
        std::snprintf(name, sizeof name, "fm4 guest %08X", k);
        pthread_setname_np(name);
    }
    {
        std::unique_lock guard(suspendLock);
        resumed.wait(guard, [&] { return s.base[k + kthread_state::kSuspendCount] == 0; });
        startedThreads.insert(k);
    }
    PPCContext ctx{};
    ctx.r1.u64 = s.state.stackBase - 0x100;
    ctx.r13.u64 = s.state.pcr;
    uint32_t code = 0;
    RegisterGuestContext(&ctx, k);  // the hang report
    try
    {
        // With an XAPI startup routine it runs startup(start, context); else start(context),
        // whose result is the exit code.
        if (s.xapiStartup)
        {
            ctx.r3.u64 = s.start;
            ctx.r4.u64 = s.context;
            PpcCallIndirect(ctx, s.base, s.xapiStartup);
        }
        else
        {
            ctx.r3.u64 = s.context;
            PpcCallIndirect(ctx, s.base, s.start);
            code = ctx.r3.u32;
        }
    }
    catch (const ThreadExit& exit)
    {
        code = exit.code;
    }
    UnregisterGuestContext(&ctx);
    s.base[k + kthread_state::kTerminated] = 1;
    Store32(s.base, k + kthread_state::kExitStatus, code);
    SetEvent(s.base, k);  // a thread object is signalled when it ends; waiters wake
    return nullptr;
}

}  // namespace

void SetTitleProcess(uint32_t process, const TlsInfo& tls, uint32_t defaultStackSize)
{
    titleProcess = process;
    titleTls = tls;
    defaultStack = defaultStackSize;
}

uint32_t CreateGuestThread(uint8_t* base, uint32_t stackSize, uint32_t xapiStartup, uint32_t start, uint32_t context,
                           uint32_t flags, uint32_t& threadId)
{
    // A zero size inherits the executable's default; sizes round to 4 KB, at least 16 KB (Xenia).
    uint32_t size = stackSize ? stackSize : defaultStack;
    size = (size + 0xFFF) & ~0xFFFu;
    if (size < 0x4000)
        size = 0x4000;
    const uint8_t mask = uint8_t(flags >> 24);
    const uint8_t cpu = mask ? uint8_t(7 - (__builtin_clz(uint32_t(mask)) - 24)) : 0;
    threadId = nextThreadId++;
    const ThreadState state = CreateThreadState(base, titleTls, titleProcess, size, threadId, start, flags, cpu);
    {
        std::lock_guard guard(suspendLock);
        base[state.kthread + kthread_state::kSuspendCount] = (flags & 1) ? 1 : 0;
    }
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, size_t(256) << 20);  // translated code nests host calls deeply
    pthread_t thread;
    if (pthread_create(&thread, &attributes, Run, new Start{base, state, xapiStartup, start, context}) != 0)
        Stop(StopCode::Setup, "cannot start a host thread for guest thread %u", threadId);
    pthread_detach(thread);
    return state.kthread;
}

uint32_t ResumeGuestThread(uint8_t* base, uint32_t kthread)
{
    std::lock_guard guard(suspendLock);
    const uint8_t previous = base[kthread + kthread_state::kSuspendCount];
    if (previous > 0)
        base[kthread + kthread_state::kSuspendCount] = uint8_t(previous - 1);
    resumed.notify_all();
    return previous;
}

uint32_t SuspendGuestThread(uint8_t* base, uint32_t kthread, uint32_t current, uint32_t& previous)
{
    std::unique_lock guard(suspendLock);
    previous = base[kthread + kthread_state::kSuspendCount];
    const bool self = kthread == current;
    if (!self && (startedThreads.count(kthread) || base[kthread + kthread_state::kTerminated]))
        Stop(StopCode::Unimplemented, "suspending the running thread %08X from another thread is not implemented", kthread);
    if (previous >= 127)
        return 0xC000004A;  // STATUS_SUSPEND_COUNT_EXCEEDED
    base[kthread + kthread_state::kSuspendCount] = uint8_t(previous + 1);
    if (self)
        resumed.wait(guard, [&] { return base[kthread + kthread_state::kSuspendCount] == 0; });
    return 0;
}

void StartServiceThread(uint8_t* base, uint8_t cpu, std::function<void(PPCContext&)> body)
{
    const uint32_t threadId = nextThreadId++;
    const ThreadState state = CreateThreadState(base, titleTls, titleProcess, 0x10000, threadId, 0, 0, cpu);
    struct Service { ThreadState state; std::function<void(PPCContext&)> body; };
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, size_t(256) << 20);  // translated code nests host calls deeply
    pthread_t thread;
    const auto run = [](void* argument) -> void* {
        std::unique_ptr<Service> service(static_cast<Service*>(argument));
        pthread_setname_np("fm4 guest service");  // FM4 Recomp: thread_cpu.h roles
        UseFaultStackOnThisThread();
        PPCContext ctx{};
        ctx.r1.u64 = service->state.stackBase - 0x100;
        ctx.r13.u64 = service->state.pcr;
        RegisterGuestContext(&ctx, service->state.kthread);  // the hang report
        try
        {
            service->body(ctx);
        }
        catch (const ThreadExit&)
        {
            Stop(StopCode::Unimplemented, "a runtime service thread was ended by guest code (ExTerminateThread)");
        }
        return nullptr;
    };
    if (pthread_create(&thread, &attributes, run, new Service{state, std::move(body)}) != 0)
        Stop(StopCode::Setup, "cannot start a host thread for service thread %u", threadId);
    pthread_detach(thread);
}

void ExitCurrentGuestThread(uint32_t exitCode) { throw ThreadExit{exitCode}; }

}  // namespace runtime
