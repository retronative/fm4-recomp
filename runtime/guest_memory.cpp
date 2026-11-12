// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/guest_memory.cpp (GPL-3.0-or-later, same owner), reviewed and re-tested here.
// FM4 Recomp changes: M3: MapPhysicalMemory from ForzaRecomp guest_memory.cpp (macOS branch only);
// ForzaRecomp's later OnTrap filter (symbol name) is not taken: our trap stop stays for every trap.
// M4 unit 7: OnFault asks the write-watch hook first (SetGuestWriteFaultHook; texture_NOTES.md §5 step 1).
// M7 (FM4_GUEST_MEMORY_GPU): the GPU read guard's wait before the write-watch hook (SetGpuReadFaultHook), and a dedicated host view
// of physical memory for the GPU import (MapPhysicalHostView).
#include "guest_memory.h"
#include "stops.h"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

namespace runtime {
namespace {

uint8_t* guestBase = nullptr;
DescribeAddress describeAddress = nullptr;
std::atomic<WriteFaultHook> writeFaultHook{nullptr};
std::atomic<const GpuReadFaultHooks*> gpuReadFaultHooks{nullptr};
// FM4 Recomp (FM4_GUEST_MEMORY_GPU=1): the physical memory's Mach entry, kept for MapPhysicalHostView (deallocated as before when
// the option is off).
mach_port_t physicalEntry = MACH_PORT_NULL;
uint32_t physicalEntrySize = 0;

void OnFault(int signal, siginfo_t* info, void* context)
{
    const auto address = reinterpret_cast<uintptr_t>(info->si_addr);
    const auto start = reinterpret_cast<uintptr_t>(guestBase);
    if (guestBase == nullptr || address < start || address - start >= PPC_MEMORY_SIZE)
    {
        ::signal(signal, SIG_DFL);  // a host fault: crash as usual when the instruction re-runs
        return;
    }
    const auto guest = uint32_t(address - start);
    // FM4_GUEST_MEMORY_GPU: enter returns once no unretired GPU read uses the page (or stops the run), holding the guard's gate
    // until the write watch has made the page writable and recorded the write (runtime/video/gpu_read_guard.h G8).
    const GpuReadFaultHooks* gpu = gpuReadFaultHooks.load(std::memory_order_acquire);
    const bool gated = gpu != nullptr && gpu->enter(guest);
    const WriteFaultHook hook = writeFaultHook.load(std::memory_order_acquire);
    const bool handled = hook != nullptr && hook(guest);
    if (gated)
        gpu->leave();
    if (handled)
        return;  // a watched page, writable again: the store re-runs
    const char* what = describeAddress ? describeAddress(guest) : nullptr;
    // The host pc and lr (arm64 x30) and the executable's load address: `atos -o <binary> -l <image> <pc> <lr>` on the
    // run's binary names the translated function (sub_<guest address>) that faulted and its caller.
    const auto* uc = static_cast<const ucontext_t*>(context);
    const uintptr_t pc = uc ? uintptr_t(uc->uc_mcontext->__ss.__pc) : 0, lr = uc ? uintptr_t(uc->uc_mcontext->__ss.__lr) : 0;
    StopFromSignal(StopCode::GuestFault, "guest access to unmapped or read-only %08X%s%s (host pc %#lx lr %#lx, image %#lx)", guest,
                   what ? ": " : "", what ? what : "", (unsigned long)pc, (unsigned long)lr,
                   (unsigned long)reinterpret_cast<uintptr_t>(_dyld_get_image_header(0)));
}

void OnTrap(int signal, siginfo_t* info, void*)
{
    // __builtin_trap() in the translation: a path the guest would also stop on (an illegal
    // instruction, or an index past a proven switch bound).
    StopFromSignal(StopCode::Trap, "translated trap (signal %d at host %p)", signal, info->si_addr);
}

}  // namespace

uint8_t* ReserveGuestMemory()
{
    void* memory = mmap(nullptr, PPC_MEMORY_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED)
        Stop(StopCode::Setup, "cannot reserve the 4 GB guest address space");
    return static_cast<uint8_t*>(memory);
}

void MapPhysicalMemory(uint8_t* base, const uint32_t* views, size_t count, uint32_t size)
{
    // One shared memory object, mapped at each view with no access (the heaps grant it per page).
    // A forked child (the tests' stop checks) gets a private copy of the views. The approach follows
    // ReCarbon (runtime/kernel/memory.cpp MapPhysicalMirrors, from Unleashed Recompiled;
    // GPL-3.0-or-later): a Mach named memory entry (macOS only, decision D4 pending).
    memory_object_size_t entrySize = size;
    mach_port_t entry = MACH_PORT_NULL;
    if (mach_make_memory_entry_64(mach_task_self(), &entrySize, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &entry,
                                  MACH_PORT_NULL) != KERN_SUCCESS)
        Stop(StopCode::Setup, "cannot create the guest physical memory");
    for (size_t i = 0; i < count; i++)
    {
        vm_address_t address = vm_address_t(base + views[i]);
        if (vm_map(mach_task_self(), &address, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry, 0, FALSE, VM_PROT_NONE,
                   VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_COPY) != KERN_SUCCESS)
            Stop(StopCode::Setup, "cannot map the guest physical memory at %08X", views[i]);
    }
    // FM4 Recomp (FM4_GUEST_MEMORY_GPU=1): kept for the GPU's host view (MapPhysicalHostView); otherwise the mappings keep the
    // memory alive as before.
    const char* gpu = std::getenv("FM4_GUEST_MEMORY_GPU");
    if (physicalEntry != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), physicalEntry), physicalEntry = MACH_PORT_NULL;
    if (gpu != nullptr && gpu[0] == '1' && gpu[1] == '\0')
        physicalEntry = entry, physicalEntrySize = size;
    else
        mach_port_deallocate(mach_task_self(), entry);
}

uint8_t* MapPhysicalHostView()
{
    if (physicalEntry == MACH_PORT_NULL)
        return nullptr;
    vm_address_t address = 0;
    if (vm_map(mach_task_self(), &address, physicalEntrySize, 0, VM_FLAGS_ANYWHERE, physicalEntry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
               VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE) != KERN_SUCCESS)
        return nullptr;
    return reinterpret_cast<uint8_t*>(address);
}

void CommitGuestMemory(uint8_t* base, uint64_t guest, uint64_t size)
{
    const uint64_t page = uint64_t(getpagesize());
    const uint64_t start = guest & ~(page - 1), end = (guest + size + page - 1) & ~(page - 1);
    if (end > PPC_MEMORY_SIZE || mprotect(base + start, end - start, PROT_READ | PROT_WRITE) != 0)
        Stop(StopCode::Setup, "cannot commit guest memory %08llX+%llX", (unsigned long long)guest, (unsigned long long)size);
}

void InstallGuestFaultHandler(uint8_t* base, DescribeAddress describe)
{
    guestBase = base;
    describeAddress = describe;
    UseFaultStackOnThisThread();
    struct sigaction action {};
    action.sa_sigaction = OnFault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    action.sa_sigaction = OnTrap;
    sigaction(SIGTRAP, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
}

void SetGuestWriteFaultHook(WriteFaultHook hook) { writeFaultHook.store(hook, std::memory_order_release); }
void SetGpuReadFaultHooks(const GpuReadFaultHooks* hooks) { gpuReadFaultHooks.store(hooks, std::memory_order_release); }

void ProtectGuestMemoryReadOnly(uint8_t* base, uint64_t guest, uint64_t size)
{
    if (mprotect(base + guest, size, PROT_READ) != 0)
        Stop(StopCode::Setup, "cannot protect guest memory %08llX+%llX", (unsigned long long)guest, (unsigned long long)size);
}

void UseFaultStackOnThisThread()
{
    static thread_local uint8_t alternate[64 * 1024];
    stack_t stack{alternate, sizeof(alternate), 0};
    if (sigaltstack(&stack, nullptr) != 0)
        Stop(StopCode::Setup, "cannot set the fault-handler stack");
}

}  // namespace runtime
