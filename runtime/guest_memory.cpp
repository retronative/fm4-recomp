// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/guest_memory.cpp (GPL-3.0-or-later, same owner), reviewed and re-tested here.
// FM4 Recomp changes: M3: MapPhysicalMemory from ForzaRecomp guest_memory.cpp (macOS branch only);
// ForzaRecomp's later OnTrap filter (symbol name) is not taken: our trap stop stays for every trap.
#include "guest_memory.h"
#include "stops.h"
#include <csignal>
#include <cstdlib>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>

namespace runtime {
namespace {

uint8_t* guestBase = nullptr;
DescribeAddress describeAddress = nullptr;

void OnFault(int signal, siginfo_t* info, void*)
{
    const auto address = reinterpret_cast<uintptr_t>(info->si_addr);
    const auto start = reinterpret_cast<uintptr_t>(guestBase);
    if (guestBase == nullptr || address < start || address - start >= PPC_MEMORY_SIZE)
    {
        ::signal(signal, SIG_DFL);  // a host fault: crash as usual when the instruction re-runs
        return;
    }
    const auto guest = uint32_t(address - start);
    const char* what = describeAddress ? describeAddress(guest) : nullptr;
    Stop(StopCode::GuestFault, "guest access to unmapped or read-only %08X%s%s", guest, what ? ": " : "", what ? what : "");
}

void OnTrap(int signal, siginfo_t* info, void*)
{
    // __builtin_trap() in the translation: a path the guest would also stop on (an illegal
    // instruction, or an index past a proven switch bound).
    Stop(StopCode::Trap, "translated trap (signal %d at host %p)", signal, info->si_addr);
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
    mach_port_deallocate(mach_task_self(), entry);  // the mappings keep it alive
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
