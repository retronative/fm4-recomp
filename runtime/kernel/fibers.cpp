// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The guest's fiber switch (the body of SwitchToFiber that config/fm4.toml names switch_fiber; patch 0024 hands it to
// the runtime as PpcRuntime_switch_fiber) on host stacks.
//
// What the guest routine does (FM4 image, the local capture switch_fiber (not distributed); the store/load shapes are surveyed in
// game/survey/context_routines.json): with r4 = the current KTHREAD (PCR +0x100) and r5 = its fiber (KTHREAD +0x164),
// it stores r1 (+48, 64 bits), LR (+28), CR (+296), r14-r31 (+152 + 8i), FPSCR as mffs leaves it (+304), f14-f31
// (+424 + 8i) and vector registers into the current fiber's block; loads the same from the target block (r3); writes
// KTHREAD +0x164 = target; and tail-calls KeSetCurrentStackPointers(target +48, KTHREAD, +4, +8, +12), which (Xenia
// xenia-canary 67f16c4 xboxkrnl_threading.cc:275-298, X_KTHREAD/X_KPCR in xthread.h) writes the KTHREAD's
// stack_alloc_base (+0xD0), stack_base (+0x5C) and stack_limit (+0x60), the PCR's stack_base_ptr (+0x70) and
// stack_end_ptr (+0x74), sets r1 and returns to the loaded LR. CreateFiber (the local capture create_fiber (not distributed)) writes
// a new block's +0 (parameter), +4/+8 (stack top), +12 (stack limit), +48 (r1), +288 (r31: the start routine) and +28
// (LR: its start trampoline).
//
// Translated code returns through host frames, so each fiber keeps a host stack: a switch saves the running fiber's
// registers (all of PPCContext; the guest-visible fields are also written to its block), and resumes the target either
// where it switched away (its snapshot restored, the switch returning to its caller) or, the first time, by calling
// the translated function at the block's LR with the registers the guest routine loads from the block. Vector registers
// of a new fiber are not loaded from its block (CreateFiber leaves those slots unwritten; their offsets are not
// verified: llvm-objdump misdecodes the VMX128 forms): a new fiber starts with the switching fiber's. Xenia re-enters
// at LR on the same host stack instead (a JIT's dispatch), which translated code cannot do.
//
// All fibers of one guest thread run on its host thread (thread-local host state, signal stacks and FPCR stay with
// it). A guest thread that ends on a fiber (ExitCurrentGuestThread's exception) has the exception carried to the
// thread's own stack and rethrown there. DeleteFiber's MmDeleteKernelStack forgets the fiber whose guest stack it frees
// (DiscardFiber) and unmaps its host stack, so a later fiber block at the same address starts afresh.
#include "imports.h"
#include "thread_state.h"
#include "indirect.h"
#include "fibers.h"
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

// arm64 (AAPCS64) context swap: saves the callee-saved state (x19-x28, fp, lr, d8-d15) on the current stack, stores
// sp to *save, switches to `load` and restores what it saved there. fm4_fiber_boot is the first return target of a
// new stack: x19 holds its Fiber.
extern "C" void fm4_fiber_swap(void** save, void* load);
extern "C" void fm4_fiber_boot();
extern "C" [[noreturn]] void fm4_fiber_entry(void* fiber);

asm(R"(
    .text
    .p2align 2
    .globl _fm4_fiber_swap
_fm4_fiber_swap:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret

    .p2align 2
    .globl _fm4_fiber_boot
_fm4_fiber_boot:
    mov x0, x19
    bl _fm4_fiber_entry
    brk #1
)");

namespace runtime {
namespace {

// The fiber block (FM4 image; see above).
namespace block { constexpr uint32_t kStackAllocBase = 4, kStackBase = 8, kStackLimit = 12, kLr = 28, kSp = 48, kGpr14 = 152,
                                     kCr = 296, kFpscr = 304, kFpr14 = 424; }
constexpr uint32_t kKthreadStackAllocBase = 0xD0;  // Xenia X_KTHREAD stack_alloc_base
constexpr size_t kHostStackSize = size_t(256) << 20;  // as a guest thread's host stack (threads.cpp): translated code nests deeply
constexpr size_t kBootFrame = 160;                    // fm4_fiber_swap's frame

struct ThreadFibers;

struct Fiber
{
    uint32_t block = 0;
    ThreadFibers* owner = nullptr;
    void* sp = nullptr;         // the host stack pointer while suspended
    bool started = false;
    uint32_t stackTop = 0;      // its guest stack (block +8 when first seen): how MmDeleteKernelStack names it
    void* host = nullptr;       // its host stack mapping (none for the thread's own stack)
    size_t hostSize = 0;
    PPCContext saved{};         // its registers while suspended
};

struct ThreadFibers
{
    std::map<uint32_t, std::unique_ptr<Fiber>> fibers;  // by block
    Fiber* root = nullptr;     // the thread's own host stack (the fiber it converted itself into)
    Fiber* current = nullptr;
    PPCContext* ctx = nullptr; // the guest thread's register file, shared by its fibers
    uint8_t* base = nullptr;
    std::exception_ptr ending; // a fiber ended the thread: rethrown on the root
};

std::mutex lock;
std::map<uint32_t, ThreadFibers> threads;  // by KTHREAD; fiber maps change under `lock` (DeleteFiber may run on another thread)

PPCRegister PPCContext::* const kGprs[18] = {&PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
                                            &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
                                            &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
                                            &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
PPCRegister PPCContext::* const kFprs[18] = {&PPCContext::f14, &PPCContext::f15, &PPCContext::f16, &PPCContext::f17, &PPCContext::f18,
                                            &PPCContext::f19, &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23,
                                            &PPCContext::f24, &PPCContext::f25, &PPCContext::f26, &PPCContext::f27, &PPCContext::f28,
                                            &PPCContext::f29, &PPCContext::f30, &PPCContext::f31};
PPCCRRegister PPCContext::* const kCrs[8] = {&PPCContext::cr0, &PPCContext::cr1, &PPCContext::cr2, &PPCContext::cr3,
                                            &PPCContext::cr4, &PPCContext::cr5, &PPCContext::cr6, &PPCContext::cr7};

uint64_t Load64(const uint8_t* base, uint32_t address) { return uint64_t(Load32(base, address)) << 32 | Load32(base, address + 4); }
void Store64(uint8_t* base, uint32_t address, uint64_t value) { Store32(base, address, uint32_t(value >> 32)), Store32(base, address + 4, uint32_t(value)); }

// mfcr / mtcr as the translation models them (XenonRecomp recompiler.cpp PPC_INST_MFCR/MTCR): bit 31 - i is field i/4's
// lt, gt, eq, so.
uint32_t CrWord(const PPCContext& ctx)
{
    uint32_t word = 0;
    for (int f = 0; f < 8; f++)
    {
        const PPCCRRegister& cr = ctx.*kCrs[f];
        word |= uint32_t(cr.lt != 0) << (31 - 4 * f) | uint32_t(cr.gt != 0) << (30 - 4 * f) | uint32_t(cr.eq != 0) << (29 - 4 * f) |
                uint32_t(cr.so != 0) << (28 - 4 * f);
    }
    return word;
}
void SetCrWord(PPCContext& ctx, uint32_t word)
{
    for (int f = 0; f < 8; f++)
    {
        PPCCRRegister& cr = ctx.*kCrs[f];
        cr.lt = (word >> (31 - 4 * f)) & 1, cr.gt = (word >> (30 - 4 * f)) & 1, cr.eq = (word >> (29 - 4 * f)) & 1, cr.so = (word >> (28 - 4 * f)) & 1;
    }
}

// The guest routine's store half (vector registers excepted, see above).
void StoreToBlock(PPCContext& ctx, uint8_t* base, uint32_t b)
{
    Store64(base, b + block::kSp, ctx.r1.u64);
    Store32(base, b + block::kLr, uint32_t(ctx.lr));
    Store32(base, b + block::kCr, CrWord(ctx));
    for (int i = 0; i < 18; i++)
        Store64(base, b + block::kGpr14 + 8 * i, (ctx.*kGprs[i]).u64);
    Store64(base, b + block::kFpscr, ctx.fpscr.loadFromHost());  // mffs (recompiler.cpp PPC_INST_MFFS)
    for (int i = 0; i < 18; i++)
        Store64(base, b + block::kFpr14 + 8 * i, (ctx.*kFprs[i]).u64);
}

// The guest routine's load half for a fiber that has not run: r14-r31, CR, LR, FPSCR (mtfsf), f14-f31, then the
// registers KeSetCurrentStackPointers is entered with (r3 = sp, r4 = KTHREAD, r5-r7 = the stack fields) and r1.
void LoadFromBlock(PPCContext& ctx, const uint8_t* base, uint32_t b, uint32_t kthread)
{
    for (int i = 0; i < 18; i++)
        (ctx.*kGprs[i]).u64 = Load64(base, b + block::kGpr14 + 8 * i);
    SetCrWord(ctx, Load32(base, b + block::kCr));
    ctx.lr = Load32(base, b + block::kLr);
    ctx.fpscr.storeFromGuest(uint32_t(Load64(base, b + block::kFpscr)));  // mtfsf (recompiler.cpp PPC_INST_MTFSF)
    for (int i = 0; i < 18; i++)
        (ctx.*kFprs[i]).u64 = Load64(base, b + block::kFpr14 + 8 * i);
    ctx.r3.u64 = Load64(base, b + block::kSp);
    ctx.r4.u64 = kthread;
    ctx.r5.u64 = Load32(base, b + block::kStackAllocBase);
    ctx.r6.u64 = Load32(base, b + block::kStackBase);
    ctx.r7.u64 = Load32(base, b + block::kStackLimit);
    ctx.r1.u64 = ctx.r3.u64;
}

// KeSetCurrentStackPointers' stores (Xenia xboxkrnl_threading.cc:284-288) for the target block.
void SetStackPointers(uint8_t* base, uint32_t pcr, uint32_t kthread, uint32_t b)
{
    const uint32_t stackBase = Load32(base, b + block::kStackBase), stackLimit = Load32(base, b + block::kStackLimit);
    Store32(base, kthread + kKthreadStackAllocBase, Load32(base, b + block::kStackAllocBase));
    Store32(base, kthread + kthread::kStackBase, stackBase);
    Store32(base, kthread + kthread::kStackLimit, stackLimit);
    Store32(base, pcr + pcr::kStackBase, stackBase);
    Store32(base, pcr + pcr::kStackEnd, stackLimit);
}

ThreadFibers& FibersOf(uint32_t kthread)
{
    std::lock_guard guard(lock);
    return threads[kthread];
}

// A host stack for a new fiber, its first swap returning into fm4_fiber_boot with x19 = the fiber.
void* NewHostStack(Fiber& f)
{
    const size_t page = size_t(getpagesize());
    void* memory = mmap(nullptr, kHostStackSize + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED)
        Stop(StopCode::Setup, "cannot map a host stack for the fiber at %08X", f.block);
    mprotect(memory, page, PROT_NONE);  // a guard below the stack
    f.host = memory, f.hostSize = kHostStackSize + page;
    auto* frame = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(memory) + page + kHostStackSize - kBootFrame);
    std::memset(frame, 0, kBootFrame);
    frame[0] = reinterpret_cast<uint64_t>(&f);                       // x19
    frame[11] = reinterpret_cast<uint64_t>(&fm4_fiber_boot);         // x30 (x29 = 0 ends frame-pointer walks)
    return frame;
}

[[gnu::noinline]] void Switch(PPCContext& ctx, uint8_t* base)
{
    const uint32_t pcr = ctx.r13.u32, kthread = CurrentThread(ctx, base);
    const uint32_t from = Load32(base, kthread + kthread::kFiber), to = ctx.r3.u32;
    if (!from || !to)
        Stop(StopCode::Unimplemented, "fiber switch from %08X to %08X: no current fiber or no target (lr=%08X)", from, to, uint32_t(ctx.lr));
    ThreadFibers& t = FibersOf(kthread);
    t.ctx = &ctx, t.base = base;
    auto fiberOf = [&](uint32_t b) -> Fiber& {
        std::lock_guard guard(lock);
        std::unique_ptr<Fiber>& f = t.fibers[b];
        if (!f)
            f = std::make_unique<Fiber>(), f->block = b, f->owner = &t, f->stackTop = Load32(base, b + block::kStackBase);
        return *f;
    };
    if (!t.current)  // the thread's first switch: it runs on its own stack, as the fiber it converted itself into
    {
        t.root = t.current = &fiberOf(from);
        t.root->started = true;
    }
    Fiber& cur = *t.current;
    if (cur.block != from)
    {
        // The guest converted the running fiber back to a thread and then to a fiber again (FM4's ConvertFiberToThread
        // clears KTHREAD +0x164 and frees the block; ConvertThreadToFiber allocates a new one):
        // the new block names the stack this host stack runs. Accepted only when the block is such a conversion (its
        // +4/+8/+12 are the KTHREAD's current stack fields, as ConvertThreadToFiber copies them) and no fiber of this
        // thread holds it; anything else is an inconsistency and stops.
        const bool reconverted = Load32(base, from + block::kStackAllocBase) == Load32(base, kthread + kKthreadStackAllocBase) &&
                                 Load32(base, from + block::kStackBase) == Load32(base, kthread + kthread::kStackBase) &&
                                 Load32(base, from + block::kStackLimit) == Load32(base, kthread + kthread::kStackLimit);
        std::lock_guard guard(lock);
        if (!reconverted || t.fibers.count(from))
            Stop(StopCode::Unimplemented, "fiber switch: the thread's fiber is %08X, but %08X is running on this host stack (lr=%08X)", from,
                 cur.block, uint32_t(ctx.lr));
        std::unique_ptr<Fiber> moved = std::move(t.fibers[cur.block]);
        t.fibers.erase(cur.block);
        moved->block = from, moved->stackTop = Load32(base, from + block::kStackBase);
        t.fibers[from] = std::move(moved);
    }
    Fiber& next = fiberOf(to);

    ctx.fpscr.csr = ctx.fpscr.getcsr();
    StoreToBlock(ctx, base, from);
    Store32(base, kthread + kthread::kFiber, to);
    SetStackPointers(base, pcr, kthread, to);
    if (&next == &cur)  // the guest reloads what it stored
        return;
    cur.saved = ctx;
    if (!next.started)
    {
        LoadFromBlock(ctx, base, to, kthread);
        next.sp = NewHostStack(next);
        next.started = true;
    }
    t.current = &next;
    fm4_fiber_swap(&cur.sp, next.sp);

    // Resumed: another fiber of this thread switched back to this one.
    ctx = cur.saved;
    ctx.fpscr.setcsr(ctx.fpscr.csr);
    if (t.ending && &cur == t.root)
    {
        const std::exception_ptr ending = t.ending;
        {
            std::lock_guard guard(lock);
            for (const auto& [block, f] : t.fibers)
                if (f->host)
                    munmap(f->host, f->hostSize);  // nothing runs on them any more
            threads.erase(kthread);
        }
        std::rethrow_exception(ending);
    }
}

}  // namespace

bool DiscardFiber(uint32_t stackTop)
{
    std::lock_guard guard(lock);
    for (auto& [kthread, t] : threads)
        for (auto f = t.fibers.begin(); f != t.fibers.end(); ++f)
            if (f->second->stackTop == stackTop && f->second.get() != t.root)
            {
                if (f->second.get() == t.current)
                    Stop(StopCode::Unimplemented, "the running fiber %08X (KTHREAD %08X) is being deleted", f->first, kthread);
                if (f->second->host)
                    munmap(f->second->host, f->second->hostSize);  // suspended frames are abandoned, as the guest abandons them
                t.fibers.erase(f);
                return true;
            }
    return false;
}

}  // namespace runtime

// A new fiber's first code: the translated function at its block's LR (the guest returns there from the switch), with
// the registers Switch loaded. Its start trampoline ends the thread rather than return; a return stops.
extern "C" [[noreturn]] void fm4_fiber_entry(void* fiber)
{
    runtime::Fiber& f = *static_cast<runtime::Fiber*>(fiber);
    runtime::ThreadFibers& t = *f.owner;
    try
    {
        PpcCallIndirect(*t.ctx, t.base, uint32_t(t.ctx->lr));
        Stop(StopCode::Unimplemented, "the fiber at %08X returned from its start (the guest has no frame to return to)", f.block);
    }
    catch (...)
    {
        t.ending = std::current_exception();
    }
    // The thread ends: back to its own stack, never to return here.
    t.current = t.root;
    fm4_fiber_swap(&f.sp, t.root->sp);
    __builtin_trap();
}

PPC_FUNC(PpcRuntime_switch_fiber)
{
    runtime::Switch(ctx, base);
}
