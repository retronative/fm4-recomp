// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp tests/runtime/test_runtime.cpp (GPL-3.0-or-later, same owner),
// reviewed and re-tested here. FM4 Recomp changes: built by tests/run_runtime_tests.sh against a
// synthetic layout (tests/runtime/ppc_config.h); the strict residual is our PpcResidualStop (patch
// 0035); new: the stop lines are checked (import name, caller and six argument registers; residual
// name, address and index), and the unimplemented runtime interfaces stop with 7.
//
// T0 tests of the runtime pieces that could fail silently: guard-page naming, the code-address
// check, CRC-32, big-endian slot writes, the checked indirect call and the guest fault stop.
// Each stopping case runs in a child process and must end with its stop's exit code.
#include "guest_memory.h"
#include "indirect.h"
#include "kernel/arena.h"
#include "kernel/file_system.h"
#include "kernel/graphics.h"
#include "kernel/command_processor.h"
#include "kernel/loader.h"
#include "kernel/critical_section.h"
#include "kernel/dispatcher.h"
#include "kernel/imports.h"
#include "kernel/module.h"
#include "kernel/thread_state.h"
#include "kernel/virtual_memory.h"
#include "kernel/objects.h"
#include "kernel/threads.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <vector>
#include "layout.h"
#include "stops.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

PPCFuncMapping PPCFuncMappings[] = {{0, nullptr}};  // the tests link no translation
PPC_EXTERN_FUNC(PpcRuntime_switch_fiber);    // defined by runtime/stops.cpp
PPC_EXTERN_FUNC(__imp__ExTerminateThread);
PPC_EXTERN_FUNC(__imp__KeQueryPerformanceFrequency);
PPC_EXTERN_FUNC(__imp__KeGetCurrentProcessType);
PPC_EXTERN_FUNC(__imp__KeQuerySystemTime);
PPC_EXTERN_FUNC(__imp__KeResetEvent);
PPC_EXTERN_FUNC(__imp__KeSetAffinityThread);
PPC_EXTERN_FUNC(__imp__KeSetEvent);
PPC_EXTERN_FUNC(__imp__KeTlsAlloc);
PPC_EXTERN_FUNC(__imp__KeTlsFree);
PPC_EXTERN_FUNC(__imp__KeTlsGetValue);
PPC_EXTERN_FUNC(__imp__KeTlsSetValue);
PPC_EXTERN_FUNC(__imp__KeWaitForMultipleObjects);
PPC_EXTERN_FUNC(__imp__KeWaitForSingleObject);
PPC_EXTERN_FUNC(__imp__NtClearEvent);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtCreateSemaphore);
PPC_EXTERN_FUNC(__imp__NtDuplicateObject);
PPC_EXTERN_FUNC(__imp__NtReleaseSemaphore);
PPC_EXTERN_FUNC(__imp__NtResumeThread);
PPC_EXTERN_FUNC(__imp__NtSetEvent);
PPC_EXTERN_FUNC(__imp__NtSignalAndWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtSuspendThread);
PPC_EXTERN_FUNC(__imp__NtWaitForMultipleObjectsEx);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__ObDereferenceObject);
PPC_EXTERN_FUNC(__imp__ObReferenceObjectByHandle);
PPC_EXTERN_FUNC(__imp__RtlEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlInitializeCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount);
PPC_EXTERN_FUNC(__imp__RtlLeaveCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlTryEnterCriticalSection);

PPC_EXTERN_FUNC(__imp__NtAllocateVirtualMemory);
PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemoryEx);
PPC_EXTERN_FUNC(__imp__MmSetAddressProtect);
PPC_EXTERN_FUNC(__imp__MmQueryAddressProtect);
PPC_EXTERN_FUNC(__imp__MmGetPhysicalAddress);
PPC_EXTERN_FUNC(__imp__MmFreePhysicalMemory);
PPC_EXTERN_FUNC(__imp__NtFreeVirtualMemory);
PPC_EXTERN_FUNC(PpcRuntime_restore_context);

namespace {

int failures = 0;

void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}

template <typename F>
int ExitCodeOf(F body)
{
    std::fflush(stdout);
    const pid_t child = fork();
    if (child == 0)
    {
        body();
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

PPC_FUNC(TerminatingThroughImport) { ctx.r3.u64 = 0x66; __imp__ExTerminateThread(ctx, base); }

// The stop line a body writes to stderr before it exits (the body runs in a child process).
template <typename F>
std::string StderrOf(F body)
{
    int pipes[2];
    if (pipe(pipes) != 0)
        return "";
    std::fflush(stdout);
    const pid_t child = fork();
    if (child == 0)
    {
        dup2(pipes[1], STDERR_FILENO);
        body();
        _exit(0);
    }
    close(pipes[1]);
    std::string text;
    char buffer[256];
    ssize_t n;
    while ((n = read(pipes[0], buffer, sizeof(buffer))) > 0)
        text.append(buffer, size_t(n));
    close(pipes[0]);
    waitpid(child, nullptr, 0);
    return text;
}

bool called = false;
// Guest-thread stand-ins (ForzaRecomp tests/runtime/test_runtime.cpp): host functions at test
// code addresses, called through the function table like translated ones.
std::atomic<uint32_t> threadSaw{0};
PPC_FUNC(ThreadBody) { (void)base; threadSaw = ctx.r3.u32; ctx.r3.u64 = 0x77; }               // start(context) -> 0x77
PPC_FUNC(XapiStartup) { (void)base; threadSaw = ctx.r3.u32 ^ ctx.r4.u32; ctx.r3.u64 = 0x99; } // startup(start, context)
PPC_FUNC(Terminating) { (void)ctx, (void)base; runtime::ExitCurrentGuestThread(0x55); }
PPC_FUNC(TerminatingThroughImport);
// A synthetic title module's functions (loader tests): its DllMain records its arguments and returns
// dllMainResult; a helper reached by an indirect call.
uint32_t dllMainArgs[3] = {}, dllMainCalls = 0, dllMainResult = 1, moduleHelperCalls = 0;
PPC_FUNC(ModuleDllMain)
{
    (void)base;
    dllMainArgs[0] = ctx.r3.u32, dllMainArgs[1] = ctx.r4.u32, dllMainArgs[2] = ctx.r5.u32;
    dllMainCalls++;
    ctx.r3.u64 = dllMainResult;
}
PPC_FUNC(ModuleHelper) { (void)ctx, (void)base; moduleHelperCalls++; }
PPC_FUNC(Translated)
{
    (void)ctx, (void)base;
    called = true;
}

}  // namespace

int main(int, char** argv)
{
    using namespace runtime;
    // Line-buffered, with a start line: a check's FAIL line survives a later stop (_exit) or the
    // runner's alarm, and the mutation gate sees that the tests ran (its `ran` pattern).
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("runtime tests: started\n");
    const size_t page = 16384;
    Expect(GuardIndex(GuardAddress(0, page), page, 13) == 0, "guard 0 names import 0");
    Expect(GuardsFit(13, page) && !GuardsFit((gpu::kRegisterBase - kGuardBase) / page + 1, page) && gpu::RegisterAddress(0x1C5) == 0x7FC80714u,
           "the guards end below the GPU register page 7FC80000; register 1C5 (CP_RB_WPTR) is at 7FC80714");
    Expect(GuardIndex(GuardAddress(12, page) + 0x20, page, 13) == 12, "a field access in guard 12 names import 12");
    Expect(GuardIndex(GuardAddress(13, page), page, 13) == -1, "negative: past the last guard is no import");
    Expect(GuardIndex(kGuardBase - 4, page, 13) == -1, "negative: below the guards is no import");
    Expect(GuardIndex(GuardAddress(5, 4096), page, 13) == 1, "negative control: a 4 KB stride names the wrong import");

    Expect(GuardsFit(64, page) && !GuardsFit(1025, page), "guards must fit below 80000000");
    const Section sections[] = {{0x10000, 0x12000, false}, {0x12000, 0x18000, true}, {0x18800, 0x19000, false}};
    Expect(PageWritable(0x10000, 0x4000, sections, 3) && !PageWritable(0x14000, 0x4000, sections, 3) &&
           PageWritable(0x18000, 0x4000, sections, 3) && !PageWritable(0x20000, 0x4000, sections, 3),
           "pages: data-overlapping writable, code-only and empty read-only");
    Expect(!PageWritable(0x14000, 0x4000, sections, 2) && PageWritable(0x10000, 0x4000, sections + 0, 1),
           "negative control: a page shared with data is writable only when the data section is listed");

    Expect(IsCodeAddress(0x1000, 0x1000, 0x100) && !IsCodeAddress(0x1002, 0x1000, 0x100) &&
           !IsCodeAddress(0x1100, 0x1000, 0x100) && !IsCodeAddress(0xFFC, 0x1000, 0x100), "code address: inside, aligned");
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    Expect(Crc32(check, sizeof(check)) == 0xCBF43926u, "CRC-32 check value (123456789)");

    uint8_t* base = ReserveGuestMemory();
    CommitGuestMemory(base, PPC_IMAGE_BASE, 0x1000);
    StoreBigEndian32(base, PPC_IMAGE_BASE, 0x12345678u);
    Expect(base[PPC_IMAGE_BASE] == 0x12 && base[PPC_IMAGE_BASE + 3] == 0x78, "slot written big-endian");

    CommitGuestMemory(base, PPC_IMAGE_BASE + PPC_IMAGE_SIZE, PPC_CODE_SIZE * 2);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE + 8) = Translated;
    PPCContext ctx{};
    PpcCallIndirect(ctx, base, PPC_CODE_BASE + 8);
    Expect(called, "indirect call to a translated function runs it");
    Expect(ExitCodeOf([&] { PpcCallIndirect(ctx, base, PPC_CODE_BASE + 12); }) == int(StopCode::IndirectCall),
           "indirect call to a code word with no function stops (6)");
    Expect(ExitCodeOf([&] { PpcCallIndirect(ctx, base, PPC_CODE_BASE + 10); }) == int(StopCode::IndirectCall),
           "indirect call to an unaligned address stops (6)");
    Expect(ExitCodeOf([&] { PpcCallIndirect(ctx, base, 0x10); }) == int(StopCode::IndirectCall),
           "indirect call outside the code stops (6)");

    InstallGuestFaultHandler(base, [](uint32_t) -> const char* { return "a guard"; });
    Expect(ExitCodeOf([&] { (void)*(volatile uint8_t*)(base + GuardAddress(3, page)); }) == int(StopCode::GuestFault),
           "a read of a data-import guard stops (5)");
    Expect(ExitCodeOf([&] {
               std::thread([&] {
                   UseFaultStackOnThisThread();
                   (void)*(volatile uint8_t*)(base + GuardAddress(4, page));
               }).join();
           }) == int(StopCode::GuestFault),
           "a guard read on another thread stops (5)");
    Expect(ExitCodeOf([&] { *(volatile uint8_t*)(base + 0x10) = 1; }) == int(StopCode::GuestFault),
           "a write near guest address 0 stops (5)");
    ProtectGuestMemoryReadOnly(base, PPC_IMAGE_BASE, 0x1000);
    Expect(base[PPC_IMAGE_BASE] == 0x12, "a read-only page is still readable");
    Expect(ExitCodeOf([&] { *(volatile uint8_t*)(base + PPC_IMAGE_BASE) = 1; }) == int(StopCode::GuestFault),
           "a write to a read-only guest page stops (5)");
    Expect(ExitCodeOf([] { __builtin_trap(); }) == int(StopCode::Trap), "a translated trap stops (10)");
    Expect(ExitCodeOf([&] { PpcImportStop("KeTest", ctx); }) == int(StopCode::Import), "an import stub stops (3)");
    Expect(ExitCodeOf([&] { PpcResidualStop(0x1234, "test", 13, 0x5678); }) == int(StopCode::Residual), "a residual stop stops (4)");
    Expect(StderrOf([&] { PpcResidualStop(0x40001234, "range_model", 13, 0x40005678); }) ==
               "[stop 4] residual stop range_model: switch at 40001234 has no proven bound (index 13, lr=40005678)\n",
           "the residual stop names the dispatch, its index and the caller");
    PPCContext regs{};
    regs.lr = 0x40001010;
    regs.r3.u32 = 3, regs.r4.u32 = 4, regs.r5.u32 = 5, regs.r6.u32 = 6, regs.r7.u32 = 7, regs.r8.u32 = 8;
    Expect(StderrOf([&] { PpcImportStop("KeTest", regs); }) ==
               "[stop 3] import KeTest called from 40001010 (r3=00000003 r4=00000004 r5=00000005 r6=00000006 r7=00000007 r8=00000008)\n",
           "an import stop names the import, the caller and six argument registers");
    // mftb: the guest time base, 49,875,000 Hz since boot (kernel/clock.h). Over a 20 ms sleep it advances by
    // about 20e-3 x 49,875,000 = 997,500 ticks; the bounds (1x .. 5x, the sleep may overrun) reject a
    // stopped clock, nanoseconds (20,000,000) and a kHz scale.
    const uint64_t tb0 = PpcQueryTimebase(), tb1 = PpcQueryTimebase();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t tbDelta = PpcQueryTimebase() - tb1;
    Expect(tb1 >= tb0 && tbDelta >= 997500 && tbDelta < 5 * 997500, "mftb: the time base advances at 49,875,000 Hz (about 997,500 ticks in 20 ms)");
    // The hang report: a registered context prints its registers; an unregistered one does not.
    PPCContext hung{};
    hung.lr = 0x40001234, hung.ctr.u64 = 0x40005678, hung.r1.u64 = 0x70001000, hung.r3.u64 = 3, hung.r6.u64 = 6;
    RegisterGuestContext(&hung, 0x7E000100);
    const std::string report = StderrOf([] { ReportGuestContexts(); });
    UnregisterGuestContext(&hung);
    Expect(report == "[hang] thread KTHREAD 7E000100: lr=40001234 ctr=40005678 r1=70001000 r3=00000003 r4=00000000 r5=00000000 r6=00000006\n" &&
               StderrOf([] { ReportGuestContexts(); }).find("7E000100") == std::string::npos,
           "the hang report prints each registered guest thread's lr, ctr, r1, r3-r6; unregistered threads disappear");
    Expect(ExitCodeOf([] { (void)PpcJumpBuffer(0x40002000); }) == int(StopCode::Unimplemented), "setjmp/longjmp stops until M3 (7)");
    Expect(ExitCodeOf([&] { PpcRuntime_switch_fiber(ctx, base); }) == int(StopCode::Unimplemented), "a fiber switch stops until M3 (7)");
    Expect(ExitCodeOf([&] { PpcRuntime_restore_context(ctx, base); }) == int(StopCode::Unimplemented), "a context restore stops until M3 (7)");

    // ---- M3 unit A: arena, thread state, module, data exports, RtlImageXexHeaderField, critical
    // sections, events. Expected values from Xenia canary 67f16c4:
    // xthread.h X_KPCR/X_KTHREAD, kernel_state.cc ProcessInfoBlock, user_module.cc:314 GetOptHeader,
    // xboxkrnl_module.cc / xboxkrnl_video.cc data exports, xboxkrnl_rtl.cc critical sections.
    auto be32 = [&](uint32_t a) { return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3]; };
    auto be16 = [&](uint32_t a) { return uint32_t(base[a]) << 8 | base[a + 1]; };
    const uint32_t a1 = ArenaAlloc(base, 10), a2 = ArenaAlloc(base, 4, 64);
    Expect(a1 == kArenaBase && a2 == kArenaBase + 64 && base[a1] == 0 && base[a1 + 9] == 0, "arena: 16-byte then 64-byte aligned, zeroed");
    Expect(PlaceStack(0x70000000, 0x10000, 0x4000).limit == 0x70004000 && PlaceStack(0x70000000, 0x10001, 0x4000).base == 0x70004000 + 0x14000,
           "stack placement: a guard page below, the size rounded up to pages");

    // A TLS template in the image (0x10 bytes 0x01..0x10), data size 0x20 (zero after the template), 4 slots.
    CommitGuestMemory(base, PPC_IMAGE_BASE + 0x2000, 0x1000);
    for (uint32_t i = 0; i < 0x10; i++)
        base[PPC_IMAGE_BASE + 0x2000 + i] = uint8_t(i + 1);
    const TlsInfo tls{4, uint32_t(PPC_IMAGE_BASE + 0x2000), 0x20, 0x10};
    const uint32_t process = CreateProcessBlock(base, tls);
    Expect(be32(process + process::kTlsDataSize) == 0x20 && be32(process + process::kTlsRawDataSize) == 0x10 &&
           be16(process + process::kTlsSlotSize) == 16 && base[process + process::kProcessType] == 1,
           "process block: TLS sizes, slot bytes (4 slots x 4) and type 1 (title) at +0x2F");
    const ThreadState t = CreateThreadState(base, tls, process, 0x10000, 7, 0x40001000, 1, 2);
    Expect(be32(t.pcr + pcr::kTls) == t.tls && be32(t.pcr + pcr::kSelf) == t.pcr && be32(t.pcr + pcr::kCurrentThread) == t.kthread &&
           be32(t.pcr + pcr::kStackBase) == t.stackBase && be32(t.pcr + pcr::kStackEnd) == t.stackLimit && base[t.pcr + pcr::kCurrentCpu] == 2,
           "PCR: TLS at +0, self at +0x30, stack at +0x70/+0x74, KTHREAD at +0x100, CPU at +0x10C");
    Expect(base[t.kthread] == 6 && be32(t.kthread + kthread::kTls) == t.tls && be32(t.kthread + kthread::kThreadId) == 7 &&
           be32(t.kthread + kthread::kStartAddress) == 0x40001000 && be32(t.kthread + kthread::kProcess) == process &&
           base[t.kthread + 0xBC] == 1 && be32(t.kthread + kthread::kCreationFlags) == 1,
           "KTHREAD: type 6, TLS +0x68, id +0x14C, start +0x150, process +0x84, suspended (CREATE_SUSPENDED) at +0xBC");
    bool tlsOk = true;
    for (uint32_t i = 0; i < 0x30; i++)
        tlsOk &= base[t.tls + i] == (i < 0x10 ? uint8_t(i + 1) : 0);
    Expect(tlsOk, "TLS: the template copied, zero up to the data size, then zeroed slots");
    ThreadState found{};
    Expect(FindThread(t.kthread, found) && found.pcr == t.pcr, "the thread is recorded");

    // A XEX header: magic, security info at +0x100 (image size at +4), 3 optional headers.
    std::vector<uint8_t> header(0x140, 0);
    auto put = [&](uint32_t at, uint32_t v) { header[at] = uint8_t(v >> 24); header[at + 1] = uint8_t(v >> 16); header[at + 2] = uint8_t(v >> 8); header[at + 3] = uint8_t(v); };
    header[0] = 'X', header[1] = 'E', header[2] = 'X', header[3] = '2';
    put(0x10, 0x100), put(0x104, 0x00F50000), put(0x14, 3);
    put(0x18, 0x00030000), put(0x1C, 0x12345678);   // inline value (low byte 0x00)
    put(0x20, 0x00018002), put(0x24, 0x120);        // checksum/timestamp: data at +0x120
    put(0x28, 0x00010001), put(0x2C, 0xCAFEF00D);   // low byte 0x01: the address of the value
    put(0x124, 0x5EED5EED);                         // the time-date stamp
    const Module m = CreateModule(base, header, 0x40000000, 0x40001000);
    Expect(XexHeaderField(base, m.header, 0x00030000) == 0x12345678 && XexHeaderField(base, m.header, 0x00010001) == m.header + 0x2C &&
           XexHeaderField(base, m.header, 0x00018002) == m.header + 0x120 && XexHeaderField(base, m.header, 0x00020200) == 0,
           "XexHeaderField: inline value, address of the value, header offset, absent 0 (Xenia user_module.cc:314)");
    Expect(be32(m.ldr + ldr::kXexHeaderBase) == m.header && be32(m.ldr + ldr::kFullImageSize) == 0x00F50000 &&
           be32(m.ldr + ldr::kImageBase) == 0x40000000 && be32(m.ldr + ldr::kEntryPoint) == 0x40001000 &&
           be32(m.ldr + ldr::kTimeDateStamp) == 0x5EED5EED,
           "loader entry: header base, image size from the security info, image base, entry, time-date stamp");
    std::vector<uint8_t> bad = header;
    bad[3] = '1';
    Expect(ExitCodeOf([&] { (void)CreateModule(base, bad, 0x40000000, 0x40001000); }) == int(StopCode::Setup), "a malformed XEX header stops (9)");

    auto exported = [&](const char* name) {
        const DataExport* e = FindDataExport(name);
        const uint32_t block = e ? ArenaAlloc(base, e->size) : 0;
        if (e) e->init(base, block, m);
        return block;
    };
    Expect(be32(exported("XexExecutableModuleHandle")) == m.ldr, "XexExecutableModuleHandle: the executable's loader entry");
    const uint32_t hw = exported("XboxHardwareInfo");
    Expect(be32(hw) == 0x20 && base[hw + 4] == 6, "XboxHardwareInfo: flags 0x20, 6 CPUs");
    Expect(be32(exported("VdGpuClockInMHz")) == 500 && be32(exported("VdGlobalDevice")) == 0 && be32(exported("KeDebugMonitorData")) == 0,
           "VdGpuClockInMHz 500; VdGlobalDevice and KeDebugMonitorData 0");
    const uint32_t lock = exported("VdHSIOCalibrationLock");
    Expect(base[lock] == 1 && base[lock + 1] == 40 && be32(lock + 0x10) == 0xFFFFFFFF && be32(lock + 0x14) == 0 && be32(lock + 0x18) == 0,
           "VdHSIOCalibrationLock: critical section (synchronization event, spin 10000 -> 40, lock count -1, big-endian)");
    const uint32_t bundle = exported("KeTimeStampBundle");
    const uint64_t systemTime = uint64_t(be32(bundle + 8)) << 32 | be32(bundle + 12);
    Expect(systemTime > 116444736000000000ull, "KeTimeStampBundle: the system time is after 1970 (FILETIME)");
    const uint32_t version = exported("XboxKrnlVersion");
    const uint8_t expectedVersion[8] = {0x00, 0x02, 0x00, 0x00, 0x44, 0x97, 0x80, 0x00};  // 2.0.17559 (0x4497), approval 0x80, QFE 0
    Expect(version && std::memcmp(base + version, expectedVersion, 8) == 0,
           "XboxKrnlVersion: 8 bytes 00 02 00 00 44 97 80 00 (policy 2.0.17559; Xenia xboxkrnl_module.cc:205-216 layout)");
    Expect(FindDataExport("ExLoadedCommandLine") == nullptr && FindDataExport("ExEventObjectType") == nullptr,
           "no documented value for ExLoadedCommandLine or ExEventObjectType: they stay guards");

    auto imp = [&](const char* name) { return FindImport(name); };
    PPCContext call{};
    call.r3.u32 = m.header, call.r4.u32 = 0x00030000;
    if (auto f = imp("RtlImageXexHeaderField")) f(call, base);
    Expect(imp("RtlImageXexHeaderField") && call.r3.u32 == 0x12345678, "RtlImageXexHeaderField through the import registry");
    // XexCheckExecutablePrivilege(n): bit n of the executable's system flags (0x00030000 = 0x12345678 here):
    // 0x12345678 >> 10 = 0x48D15 (odd: 1), >> 11 = 0x2468A (even: 0); n >= 32 stops (7).
    SetExecutableModule(m);
    PPCContext priv{};
    priv.r3.u32 = 10; imp("XexCheckExecutablePrivilege")(priv, base);
    const uint32_t bit10 = priv.r3.u32;
    priv.r3.u32 = 11; imp("XexCheckExecutablePrivilege")(priv, base);
    Expect(bit10 == 1 && priv.r3.u32 == 0, "XexCheckExecutablePrivilege: privilege 10 set, 11 clear (system flags 0x12345678)");
    Expect(ExitCodeOf([&] { PPCContext c{}; c.r3.u32 = 32; imp("XexCheckExecutablePrivilege")(c, base); }) == int(StopCode::Unimplemented),
           "XexCheckExecutablePrivilege(32) stops (7)");
    PPCContext av{};
    imp("XGetAVPack")(av, base);
    Expect(av.r3.u64 == 8, "XGetAVPack: 8 (Xenia's default AV pack, xam_info.cc:32)");
    PPCContext region{};
    imp("XGetGameRegion")(region, base);
    Expect(region.r3.u64 == 0xFF, "XGetGameRegion: 0x00FF for country 103 (Xenia xam_info.cc:231)");
    // XGetVideoMode / VdQueryVideoMode (xbox.h:264): 1280, 720, progressive 0, widescreen 1, HD 1, 60.0f =
    // 0x42700000 (IEEE 754: exponent 5 + 127 = 0x84, mantissa 0.875), NTSC 1, 0x4A, 0x01, three zero words.
    const uint32_t videoMode = ArenaAlloc(base, 48);
    const uint32_t expectedMode[12] = {1280, 720, 0, 1, 1, 0x42700000, 1, 0x4A, 0x01, 0, 0, 0};
    auto modeIs = [&] { for (int i = 0; i < 12; i++) if (be32(videoMode + 4 * i) != expectedMode[i]) return false; return true; };
    for (const char* name : {"XGetVideoMode", "VdQueryVideoMode"})
    {
        for (int i = 0; i < 12; i++) Store32(base, videoMode + 4 * i, 0xAAAAAAAA);
        PPCContext vm{};
        vm.r3.u32 = videoMode;
        imp(name)(vm, base);
        Expect(modeIs(), name);
    }
    // No launch data (a cold start; Xenia xam_info.cc:294-323): size 0 and ERROR_NOT_FOUND (0x490);
    // no size pointer: ERROR_INVALID_PARAMETER (0x57); the data: ERROR_NOT_FOUND.
    const uint32_t launchSize = ArenaAlloc(base, 4);
    Store32(base, launchSize, 0xAAAAAAAA);
    PPCContext ld{};
    ld.r3.u32 = launchSize;
    imp("XamLoaderGetLaunchDataSize")(ld, base);
    const bool sizeNotFound = ld.r3.u64 == 0x490 && be32(launchSize) == 0;
    ld.r3.u64 = 0;
    imp("XamLoaderGetLaunchDataSize")(ld, base);
    const bool noPointer = ld.r3.u64 == 0x57;
    ld.r3.u32 = launchSize, ld.r4.u32 = 16;
    imp("XamLoaderGetLaunchData")(ld, base);
    // RtlNtStatusToDosError (Xenia xboxkrnl_error.cc:974): table entries C0000034 -> 2 (:220), PENDING 103 -> 0x3E5 (:34), C0000011 -> 0x26
    // (:185), C00000BA -> 5 (:354); 0 and a customer code (bit 29: 0x2000ABCD) unchanged; HRESULT 8007:0057 -> 0x57;
    // 0xD0000034 (bits 28-29 cleared: C0000034) -> 2; 0xC0010005 -> 5; an unlisted status stops (7).
    auto dosError = [&](uint32_t status) { PPCContext c{}; c.r3.u64 = status; imp("RtlNtStatusToDosError")(c, base); return c.r3.u32; };
    Expect(dosError(0xC0000034) == 2 && dosError(0x103) == 0x3E5 && dosError(0xC0000011) == 0x26 && dosError(0xC00000BA) == 5 && dosError(0) == 0 &&
               ExitCodeOf([&] { _exit(dosError(0x2000ABCD) == 0x2000ABCD ? 0 : 1); }) == 0 &&
               ExitCodeOf([&] { _exit(int(dosError((0x8007u << 16) | 0x57))); }) == 0x57 &&
               ExitCodeOf([&] { _exit(int(dosError(0xD0000034))); }) == 2 && ExitCodeOf([&] { _exit(int(dosError(0xC0010005))); }) == 5 &&
               ExitCodeOf([&] { (void)dosError(0xC0000001); }) == int(StopCode::Unimplemented),
           "RtlNtStatusToDosError: table entries, rules for 0, customer codes, 8007xxxx, Dxxxxxxx, C001xxxx; an unlisted status stops (7)");
    // FscSetCacheElementCount(0, count) (Xenia xboxkrnl_io.cc:669): success for FM4's shape (0, 0x400);
    // another first argument, a zero count or one above 65536 stops (7).
    PPCContext fsc{};
    fsc.r3.u64 = 0, fsc.r4.u64 = 0x400;
    imp("FscSetCacheElementCount")(fsc, base);
    Expect(fsc.r3.u64 == 0 &&
               ExitCodeOf([&] { PPCContext c{}; c.r3.u64 = 1, c.r4.u64 = 0x400; imp("FscSetCacheElementCount")(c, base); }) == int(StopCode::Unimplemented) &&
               ExitCodeOf([&] { PPCContext c{}; c.r4.u64 = 0; imp("FscSetCacheElementCount")(c, base); }) == int(StopCode::Unimplemented) &&
               ExitCodeOf([&] { PPCContext c{}; c.r4.u64 = 0x10001; imp("FscSetCacheElementCount")(c, base); }) == int(StopCode::Unimplemented),
           "FscSetCacheElementCount(0, 0x400): success; (1, 0x400), (0, 0), (0, 0x10001) stop (7)");
    Expect(sizeNotFound && noPointer && ld.r3.u64 == 0x490,
           "XamLoaderGetLaunchDataSize: size 0, ERROR_NOT_FOUND (no pointer: INVALID_PARAMETER); XamLoaderGetLaunchData: ERROR_NOT_FOUND");

    // XexGetModuleHandle(name*, handle*) (Xenia xboxkrnl_modules.cc:44): null or "" -> the executable's
    // loader entry; a system module's name (any case) -> its entry; another name stops (7).
    auto guestString = [&](const char* text) {
        const uint32_t at = ArenaAlloc(base, uint32_t(std::strlen(text) + 1));
        std::memcpy(base + at, text, std::strlen(text) + 1);
        return at;
    };
    const uint32_t handleOut = ArenaAlloc(base, 4);
    // RtlInitAnsiString(string*, source*) (Xenia xboxkrnl_rtl.cc:224): game:\x (7 characters: g a m e : \ x)
    // gives length 7, maximum 8, the source address (big-endian 16, 16, 32 bits); a null source gives 0, 0, 0.
    const uint32_t ansi = ArenaAlloc(base, 8), ansiSource = guestString("game:\\x");
    Store32(base, ansi, 0xAAAAAAAA), Store32(base, ansi + 4, 0xAAAAAAAA);
    PPCContext rs{};
    rs.r3.u32 = ansi, rs.r4.u32 = ansiSource;
    imp("RtlInitAnsiString")(rs, base);
    const bool ansiOk = be32(ansi) == 0x00070008 && be32(ansi + 4) == ansiSource;
    rs.r3.u32 = ansi, rs.r4.u32 = 0;
    imp("RtlInitAnsiString")(rs, base);
    Expect(ansiOk && be32(ansi) == 0 && be32(ansi + 4) == 0, "RtlInitAnsiString: length 7, maximum 8, buffer; a null source gives 0, 0, 0");
    {
        // The disc (kernel/file_system.h; Xenia vfs). Paths, by hand from base/utf8.cc:684 canonicalize_path:
        Expect(CanonicalGuestPath("game:\\Media\\..\\x") == "game:\\x" && CanonicalGuestPath("game:\\..\\x") == "game:\\x" &&
                   CanonicalGuestPath("\\a\\.\\b\\") == "\\a\\b" && CanonicalGuestPath("a//b") == "a\\b" &&
                   CanonicalGuestPath("..\\a") == "a",
               "canonical guest paths: .. stops at a device, . and empty parts dropped, rooted kept");
        Expect(ValidGuestPath("game:\\Media\\x_1.bin") && !ValidGuestPath("game:\\a+b") && !ValidGuestPath("game:\\a*") &&
                   !ValidGuestPath("game:\\a?") && !ValidGuestPath("game:\\\x7f") && !ValidGuestPath("game:\\\x1f"),
               "valid guest paths: printable ASCII without reserved characters (Xenia xboxkrnl_io.cc:42)");
        // A synthetic disc next to the test binary: Media/Cars.bin (5000 bytes, byte i = 'A' + i % 26),
        // Media/Empty.bin (0 bytes).
        const std::string disc = std::string(argv[0]).substr(0, std::string(argv[0]).rfind('/')) + "/disc";
        mkdir(disc.c_str(), 0755);
        mkdir((disc + "/Media").c_str(), 0755);
        {
            std::string cars(5000, ' ');
            for (size_t i = 0; i < cars.size(); i++) cars[i] = char('A' + i % 26);
            std::ofstream(disc + "/Media/Cars.bin") << cars;
        }
        std::ofstream(disc + "/Media/Empty.bin");
        MountDisc(disc);
    gpu::InitializeRegisters(base);
    Expect(gpu::ReadRegister(base, 0x0F00) == 0x08100748 && gpu::ReadRegister(base, 0x0F01) == 0x0000200E &&
               gpu::ReadRegister(base, 0x194C) == 0x2D0 && gpu::ReadRegister(base, 0x1951) == 1 && gpu::ReadRegister(base, 0x1961) == 0x050002D0 &&
               base[0x7FC80000u + 4 * 0x1961] == 0x05 && gpu::ReadRegister(base, gpu::reg::kCpRbWptr) == 0,
           "GPU registers: Xenia's five constants (graphics_system.cc:209-224), big-endian; CP_RB_WPTR starts at 0");
        const uint32_t attrs = ArenaAlloc(base, 12), nameString = ArenaAlloc(base, 8), info = ArenaAlloc(base, 56);
        auto setPath = [&](const char* path, uint32_t root = 0) {  // the attributes and name string, no call
            const uint32_t text = guestString(path);
            Store32(base, nameString, uint32_t(std::strlen(path)) << 16 | uint32_t(std::strlen(path) + 1));
            Store32(base, nameString + 4, text);
            Store32(base, attrs, root), Store32(base, attrs + 4, nameString), Store32(base, attrs + 8, 0x40);
        };
        auto query = [&](const char* path, uint32_t root = 0) {
            setPath(path, root);
            for (uint32_t i = 0; i < 56; i += 4) Store32(base, info + i, 0xAAAAAAAA);
            PPCContext c{};
            c.r3.u32 = attrs, c.r4.u32 = info;
            imp("NtQueryFullAttributesFile")(c, base);
            return c.r3.u32;
        };
        auto be64 = [&](uint32_t at) { return uint64_t(be32(at)) << 32 | be32(at + 4); };
        // 1970-01-01 as FILETIME: 11644473600 s x 10^7 = 116444736000000000; 5000 bytes -> 3 sectors of 2048 = 6144.
        const bool timesOk = query("GAME:\\media\\CARS.BIN") == 0 && be64(info) == 116444736000000000ull && be64(info + 8) == 116444736000000000ull &&
                             be64(info + 16) == 116444736000000000ull && be64(info + 24) == 116444736000000000ull;
        Expect(timesOk && be64(info + 32) == 6144 && be64(info + 40) == 5000 && be32(info + 48) == 0x01 && be32(info + 52) == 0,
               "NtQueryFullAttributesFile(GAME:\\media\\CARS.BIN): 1970 times, allocation 6144, size 5000, read-only, any case");
        Expect(query("d:\\Media") == 0 && be32(info + 48) == 0x11 && be64(info + 40) == 0 && query("game:\\Media\\Empty.bin") == 0 &&
                   be64(info + 32) == 0 && be64(info + 40) == 0,
               "NtQueryFullAttributesFile: d:\\Media is a read-only directory (0x11); an empty file allocates 0");
        Expect(query("game:\\missing") == 0xC000000F && query("game:\\Media\\Cars.bin\\x") == 0xC000000F &&
                   query("\\Device\\Cdrom0\\Media\\..\\Media\\Cars.bin") == 0 && query("game:\\a+b") == 0xC0000033,
               "NtQueryFullAttributesFile: missing or under a file NO_SUCH_FILE; the device path and .. resolve; a reserved character OBJECT_NAME_INVALID");
        Expect(ExitCodeOf([&] { (void)query("cache:\\x"); }) == int(StopCode::Unimplemented) &&
                   ExitCodeOf([&] { (void)query("\\Device\\Harddisk0\\Partition1\\x"); }) == int(StopCode::Unimplemented) &&
                   ExitCodeOf([&] { (void)query("game:\\Media", 0x1234); }) == int(StopCode::Unimplemented) &&
                   query("game:\\Media", 0xFFFFFFFD) == 0,
               "NtQueryFullAttributesFile: another device or a root handle stops (7); ObDosDevices (FFFFFFFD) is accepted");
        // NtCreateFile(handle*, access, attributes*, io status*, allocation*, file attributes, share, disposition,
        // options on the stack at r1 + 0x54) (Xenia xboxkrnl_io.cc:98): FM4's read open (kReadAccess, FILE_OPEN,
        // synchronous non-directory 0x60) gives a File handle and io status {0, FILE_OPENED 1}.
        // GENERIC_READ (bit 31) | SYNCHRONIZE (0x100000) | FILE_READ_ATTRIBUTES (0x80): FM4's read open.
        constexpr uint32_t kReadAccess = (1u << 31) | 0x00100000u | 0x80u;
        const uint32_t fileOut = ArenaAlloc(base, 4), ioStatus = ArenaAlloc(base, 8), stack = ArenaAlloc(base, 0x60);
        auto create = [&](const char* path, uint32_t access, uint32_t disposition, uint32_t options, uint32_t attributesAt = 1) {
            setPath(path);
            Store32(base, fileOut, 0xAAAAAAAA), Store32(base, ioStatus, 0xAAAAAAAA), Store32(base, ioStatus + 4, 0xAAAAAAAA);
            Store32(base, stack + 0x54, options);
            PPCContext c{};
            c.r1.u32 = stack, c.r3.u32 = fileOut, c.r4.u32 = access, c.r5.u32 = attributesAt ? attrs : 0, c.r6.u32 = ioStatus;
            c.r8.u32 = 0x80, c.r9.u32 = 1, c.r10.u32 = disposition;
            imp("NtCreateFile")(c, base);
            return c.r3.u32;
        };
        const bool opened = create("game:\\Media\\Cars.bin", kReadAccess, 1, 0x60) == 0 && be32(ioStatus) == 0 && be32(ioStatus + 4) == 1;
        const auto fileObject = LookupHandle(be32(fileOut));
        const OpenFile* openFile = fileObject ? FindOpenFile(fileObject->header) : nullptr;
        Expect(opened && fileObject && fileObject->type == ObjectType::File && openFile && openFile->entry.size == 5000 && openFile->position == 0,
               "NtCreateFile(game:\\Media\\Cars.bin, read, FILE_OPEN): a File handle, io status {0, FILE_OPENED}, size 5000, position 0");
        Expect(create("game:\\Media\\nothing.bin", kReadAccess, 1, 0x60) == 0xC000000F && be32(ioStatus) == 0xC000000F &&
                   be32(ioStatus + 4) == 5 && be32(fileOut) == 0xFFFFFFFF &&
                   create("game:\\Media", kReadAccess, 1, 0x60) == 0xC00000BA && create("game:\\Media", kReadAccess, 1, 0x20) == 0 &&
                   create("game:\\Media\\Cars.bin", kReadAccess, 3, 0x60) == 0 && create("game:\\x", kReadAccess, 1, 0x60, 0) == 0xC000000D,
               "NtCreateFile: missing NO_SUCH_FILE {.., FILE_DOES_NOT_EXIST 5}, handle -1; a directory as a non-directory FILE_IS_A_DIRECTORY; OPEN_IF of an existing file opens; no attributes INVALID_PARAMETER");
        // No hard disk: FM4's open of \Device\Harddisk0\partition0 (access C0100000 =
        // GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE) gives STATUS_OBJECT_NAME_NOT_FOUND (C0000034), handle -1, io
        // status {C0000034, FILE_DOES_NOT_EXIST 5}; NtOpenFile the same. A name that only starts like the device
        // (\Device\Harddisk01) is another device: it stops; so does any other device.
        constexpr uint32_t kReadWrite = (1u << 31) | (1u << 30) | 0x00100000u;
        const bool hddAbsent = create("\\Device\\Harddisk0\\partition0", kReadWrite, 1, 0x60) == 0xC0000034 &&
                               be32(fileOut) == 0xFFFFFFFF && be32(ioStatus) == 0xC0000034 && be32(ioStatus + 4) == 5 &&
                               create("\\Device\\Harddisk0", kReadAccess, 1, 0x20) == 0xC0000034;
        setPath("\\Device\\Harddisk0\\Partition1\\x");
        PPCContext hddOpen{};
        hddOpen.r3.u32 = fileOut, hddOpen.r4.u32 = kReadAccess, hddOpen.r5.u32 = attrs, hddOpen.r6.u32 = ioStatus, hddOpen.r7.u32 = 0x60;
        imp("NtOpenFile")(hddOpen, base);
        Expect(hddAbsent && hddOpen.r3.u64 == 0xC0000034 && be32(fileOut) == 0xFFFFFFFF,
               "NtCreateFile/NtOpenFile on \\Device\\Harddisk0: OBJECT_NAME_NOT_FOUND, handle -1, io status {C0000034, 5} (no hard disk)");
        // No title update installed (file_system.h; Xenia kernel_state.cc:482-510): update:\media.zip (FM4's query)
        // and an open of UPDATE:\default.xexp give NO_SUCH_FILE; the open: handle -1, {C000000F, 5}.
        Expect(query("update:\\media.zip") == 0xC000000F && create("update:\\default.xexp", kReadAccess, 1, 0x60) == 0xC000000F &&
                   be32(fileOut) == 0xFFFFFFFF && be32(ioStatus) == 0xC000000F && be32(ioStatus + 4) == 5 &&
                   ExitCodeOf([&] { _exit(create("UPDATE:\\default.xexp", kReadAccess, 1, 0x60) == 0xC000000F ? 0 : 1); }) == 0,
               "update: (no title update installed): NtQueryFullAttributesFile and NtCreateFile give NO_SUCH_FILE, handle -1, {C000000F, 5}");
        Expect(StderrOf([&] { (void)create("\\Device\\Harddisk01\\x", kReadWrite, 1, 0x60); }).find("not on the disc") != std::string::npos &&
                   ExitCodeOf([&] { (void)create("cache:\\x", kReadAccess, 1, 0x60); }) == int(StopCode::Unimplemented),
               "NtCreateFile: another device (Harddisk01, cache:) stops naming it, not the read-only disc");
        Expect(ExitCodeOf([&] { (void)create("game:\\Media\\Cars.bin", 0x40000000, 1, 0x60); }) == int(StopCode::Unimplemented) &&
                   ExitCodeOf([&] { (void)create("game:\\Media\\New.bin", kReadAccess, 2, 0x60); }) == int(StopCode::Unimplemented) &&
                   ExitCodeOf([&] { (void)create("game:\\Media\\New.bin", kReadAccess, 3, 0x60); }) == int(StopCode::Unimplemented),
               "NtCreateFile: write access, FILE_CREATE, or OPEN_IF of a missing file stop (7): the disc is read-only");
        PPCContext openCall{};
        (void)query("game:\\Media\\Cars.bin");
        openCall.r3.u32 = fileOut, openCall.r4.u32 = kReadAccess, openCall.r5.u32 = attrs, openCall.r6.u32 = ioStatus, openCall.r7.u32 = 0x60;
        imp("NtOpenFile")(openCall, base);
        Expect(openCall.r3.u64 == 0 && be32(ioStatus + 4) == 1 && LookupHandle(be32(fileOut)).has_value(), "NtOpenFile: FILE_OPEN of an existing file");
        // NtQueryInformationFile(handle, io status*, info*, length, class) (Xenia xboxkrnl_io_info.cc:58): class 34
        // gives the same 56 bytes as above for the open Cars.bin, io status {0, 56}, and zeroes the rest of a
        // 64-byte buffer; class 14 the position 0 {0, 8}; 55 bytes INFO_LENGTH_MISMATCH; no file INVALID_HANDLE.
        const uint32_t openHandle = be32(fileOut), fileInfo = ArenaAlloc(base, 64);
        auto information = [&](uint32_t handle, uint32_t length, uint32_t infoClass) {
            for (uint32_t i = 0; i < 64; i += 4) Store32(base, fileInfo + i, 0xAAAAAAAA);
            Store32(base, ioStatus, 0xAAAAAAAA), Store32(base, ioStatus + 4, 0xAAAAAAAA);
            PPCContext c{};
            c.r3.u32 = handle, c.r4.u32 = ioStatus, c.r5.u32 = fileInfo, c.r6.u32 = length, c.r7.u32 = infoClass;
            imp("NtQueryInformationFile")(c, base);
            return c.r3.u32;
        };
        const bool networkOpen = information(openHandle, 64, 34) == 0 && be64(fileInfo) == 116444736000000000ull &&
                                 be64(fileInfo + 32) == 6144 && be64(fileInfo + 40) == 5000 && be32(fileInfo + 48) == 1 &&
                                 be32(fileInfo + 56) == 0 && be32(fileInfo + 60) == 0 && be32(ioStatus) == 0 && be32(ioStatus + 4) == 56;
        Expect(networkOpen && information(openHandle, 8, 14) == 0 && be64(fileInfo) == 0 && be32(ioStatus + 4) == 8,
               "NtQueryInformationFile: network open (34) of Cars.bin, the buffer's rest zeroed, io status {0, 56}; position (14) 0 {0, 8}");
        Expect(information(openHandle, 55, 34) == 0xC0000004 && be32(fileInfo) == 0xAAAAAAAA && information(0x1234, 64, 34) == 0xC0000008 &&
                   ExitCodeOf([&] { (void)information(openHandle, 64, 5); }) == int(StopCode::Unimplemented),
               "NtQueryInformationFile: 55 bytes INFO_LENGTH_MISMATCH (nothing written), no file INVALID_HANDLE, another class stops (7)");
        // NtReadFile(handle, event, APC, context, io status*, buffer*, length, offset*) (Xenia xboxkrnl_io.cc:184,
        // disc_image_file.cc:26), on the synchronous Cars.bin. Byte i is 'A' + i % 26: offset 30 -> 'E' (30 % 26 = 4),
        // 39 -> 'N' (13), 40 -> 'O', 45 -> 'T' (19), 4998 -> 'G' (4998 % 26 = 6).
        const uint32_t readBuffer = ArenaAlloc(base, 16), offsetValue = ArenaAlloc(base, 8);
        auto readFile = [&](uint32_t handle, uint32_t length, int64_t offset, uint32_t event = 0) {  // offset -2: none
            for (uint32_t i = 0; i < 16; i += 4) Store32(base, readBuffer + i, 0x2E2E2E2E);
            Store32(base, ioStatus, 0xAAAAAAAA), Store32(base, ioStatus + 4, 0xAAAAAAAA);
            Store32(base, offsetValue, uint32_t(uint64_t(offset) >> 32)), Store32(base, offsetValue + 4, uint32_t(offset));
            PPCContext c{};
            c.r3.u32 = handle, c.r4.u32 = event, c.r7.u32 = ioStatus, c.r8.u32 = readBuffer, c.r9.u32 = length;
            c.r10.u32 = offset == -2 ? 0 : offsetValue;
            imp("NtReadFile")(c, base);
            return c.r3.u32;
        };
        auto position = [&] { return FindOpenFile(LookupHandle(openHandle)->header)->position; };
        const bool at30 = readFile(openHandle, 10, 30) == 0 && base[readBuffer] == 'E' && base[readBuffer + 9] == 'N' &&
                          base[readBuffer + 10] == '.' && be32(ioStatus) == 0 && be32(ioStatus + 4) == 10 && position() == 40;
        const bool atPosition = readFile(openHandle, 5, -2) == 0 && base[readBuffer] == 'O' && position() == 45 &&
                                readFile(openHandle, 1, -1) == 0 && base[readBuffer] == 'T' && position() == 46;
        Expect(at30 && atPosition, "NtReadFile: 10 bytes at 30 (E..N), io status {0, 10}, position 40; then from the position (no offset, or -1)");
        const bool tail = readFile(openHandle, 10, 4998) == 0 && base[readBuffer] == 'G' && base[readBuffer + 2] == '.' &&
                          be32(ioStatus + 4) == 2 && position() == 5000;
        Expect(tail && readFile(openHandle, 10, 5000) == 0xC0000011 && be32(ioStatus) == 0xC0000011 && be32(ioStatus + 4) == 0 &&
                   readFile(openHandle, 0, 9999) == 0 && readFile(0x1234, 4, 0) == 0xC0000008,
               "NtReadFile: 2 bytes at 4998, then END_OF_FILE {C0000011, 0}; zero length succeeds; no file INVALID_HANDLE");
        // Asynchronous (Xenia xboxkrnl_io.cc:184-262): a file opened without FILE_SYNCHRONOUS_IO_*, an event: the read
        // completes at once, io status {0, 4}, the event set, PENDING (0x103); at the end: END_OF_FILE, event set;
        // no offset stops (7); an event handle that is no event INVALID_HANDLE; an APC stops (7).
        const uint32_t asyncHandle = create("game:\\Media\\Cars.bin", kReadAccess, 1, 0x40) == 0 ? be32(fileOut) : 0;
        const uint32_t eventHandle = CreateObject(ObjectType::Event, ArenaAlloc(base, 16), "");
        const uint32_t eventBody = LookupHandle(eventHandle)->header;
        base[eventBody] = 0, Store32(base, eventBody + 4, 0);  // a notification event, not signalled
        const bool pending = readFile(asyncHandle, 4, 26, eventHandle) == 0x103 && base[readBuffer] == 'A' && be32(ioStatus) == 0 &&
                             be32(ioStatus + 4) == 4 && be32(eventBody + 4) == 1;
        Store32(base, eventBody + 4, 0);
        const bool atEnd = readFile(asyncHandle, 4, 5000, eventHandle) == 0xC0000011 && be32(eventBody + 4) == 1;
        Expect(asyncHandle && pending && atEnd && readFile(openHandle, 4, 0, openHandle) == 0xC0000008 &&
                   ExitCodeOf([&] { (void)readFile(asyncHandle, 4, -2); }) == int(StopCode::Unimplemented),
               "NtReadFile asynchronous: completes at once, {0, 4}, event set, PENDING; END_OF_FILE at the end; a non-event INVALID_HANDLE; no offset stops (7)");
        PPCContext withApc{};
        withApc.r3.u32 = openHandle, withApc.r5.u32 = 0x40001000, withApc.r7.u32 = ioStatus, withApc.r8.u32 = readBuffer, withApc.r9.u32 = 4;
        Expect(ExitCodeOf([&] { imp("NtReadFile")(withApc, base); }) == int(StopCode::Unimplemented), "NtReadFile with an APC routine stops (7)");
        // The loader (kernel/loader.h) with a synthetic module: image 94000000 (0x20000 bytes), code 94010000 (0x100),
        // its function table after the image (94020000, 2 bytes per code byte); DllMain at 94010000, a helper at
        // 94010010; a function slot at +0x100 (thunk 94010010) and a data import at +0x104 shared by name with the
        // executable's (RecordDataImport). Its file Mod.xex on the disc: the XEX header above (0x140 bytes) + 0x40.
        {
            const std::string modules = disc + "/../modules";
            mkdir(modules.c_str(), 0755), mkdir((modules + "/testmod").c_str(), 0755), mkdir((modules + "/testmod/survey").c_str(), 0755);
            std::vector<uint8_t> moduleImage(0x20000);
            for (size_t i = 0; i < moduleImage.size(); i++) moduleImage[i] = uint8_t(i * 7);
            // Its XEX export table at +0x200 (xex2_info.h:614): image base 9401 (<< 16), 2 exports from ordinal 1:
            // offsets 0x0010 (ordinal 1 -> 94010010) and 0x0000 (ordinal 2 -> 94010000).
            const uint32_t exportWords[][2] = {{0x220, 0x9401}, {0x224, 2}, {0x228, 1}, {0x22C, 0x10}, {0x230, 0}};
            for (const auto& w : exportWords)
                for (int b = 0; b < 4; b++) moduleImage[w[0] + b] = uint8_t(w[1] >> (24 - 8 * b));
            std::ofstream(modules + "/testmod/survey/image.bin", std::ios::binary).write(reinterpret_cast<const char*>(moduleImage.data()), long(moduleImage.size()));
            std::vector<uint8_t> moduleFile = header;
            moduleFile.resize(0x180, 0x5A);
            std::ofstream(disc + "/Mod.xex", std::ios::binary).write(reinterpret_cast<const char*>(moduleFile.data()), long(moduleFile.size()));
            RecordDataImport("TestData", 0x7E001230);
            static const Section sections[] = {{0x94000000u, 0x94010000u, false}, {0x94010000u, 0x94010100u, true}};
            static const ModuleImport imports[] = {{0x94000100u, false, 0x94010010u, "__imp__KeA"}, {0x94000104u, true, 0, "__imp__TestData"}};
            static const PPCFuncMapping mappings[] = {{0x94010000u, ModuleDllMain}, {0x94010010u, ModuleHelper}, {0, nullptr}};
            static ModuleImage images[2];
            images[0] = ModuleImage{"testmod", "Mod.xex", 0x94000000u, 0x20000u, 0x94010000u, 0x94010000u, 0x100u,
                                    Crc32(moduleImage.data(), moduleImage.size()), 0x140u, Crc32(moduleFile.data(), 0x140),
                                    uint32_t(moduleFile.size()), Crc32(moduleFile.data(), moduleFile.size()), 0x94000200u,
                                    sections, 2, imports, 2, mappings};
            images[1] = images[0];
            images[1].fileName = "Cars.bin", images[1].name = "badcrc";  // on the disc, but its CRC does not match
            SetModuleImages(images, 2, modules.c_str());
            auto load = [&](const char* name) {
                PPCContext c{};
                c.r3.u32 = guestString(name), c.r4.u32 = 9, c.r6.u32 = handleOut;
                Store32(base, handleOut, 0xAAAAAAAA);
                imp("XexLoadImage")(c, base);
                return c.r3.u32;
            };
            const bool loadedOk = load("game:\\Mod.xex") == 0 && dllMainCalls == 1;
            const uint32_t ldr = be32(handleOut);
            Expect(loadedOk && ldr != 0 && dllMainArgs[0] == ldr && dllMainArgs[1] == 1 && dllMainArgs[2] == 0 &&
                       base[ldr + 0x40] == 0 && base[ldr + 0x41] == 1 && be32(ldr + ldr::kImageBase) == 0x94000000u,
                   "XexLoadImage(game:\\Mod.xex): loaded, DllMain(loader entry, PROCESS_ATTACH 1, 0) once, load count 1");
            Expect(base[0x94000000u + 5] == uint8_t(35) && be32(0x94000100u) == 0x94010010u && be32(0x94000104u) == 0x7E001230u &&
                       FindFunction(base, 0x94010010u) == ModuleHelper && FindFunction(base, 0x94010020u) == nullptr,
                   "the module: image copied, function slot = thunk, data import shared with the executable, its table");
            PPCContext indirect{};
            PpcCallIndirect(indirect, base, 0x94010010u);
            Expect(moduleHelperCalls == 1 && ExitCodeOf([&] { base[0x94010000u] = 1; }) == int(StopCode::GuestFault) &&
                       ExitCodeOf([&] { base[0x94020000u] = 1; }) == int(StopCode::GuestFault),
                   "an indirect call reaches the module's function; its code and table are read-only");
            Expect(load("MOD") == 0 && be32(handleOut) == ldr && dllMainCalls == 1 && base[ldr + 0x41] == 2,
                   "XexLoadImage(MOD) again: the same entry, no second DllMain, load count 2");
            PPCContext byName{};
            byName.r3.u32 = guestString("mod.xex"), byName.r4.u32 = handleOut;
            imp("XexGetModuleHandle")(byName, base);
            PPCContext section{};
            section.r3.u32 = ldr, section.r4.u32 = guestString("dbgstr"), section.r5.u32 = fileInfo, section.r6.u32 = fileInfo + 4;
            imp("XexGetModuleSection")(section, base);
            Expect(byName.r3.u64 == 0 && be32(handleOut) == ldr && section.r3.u64 == 0xC0000225,
                   "XexGetModuleHandle(mod.xex) gives its entry; XexGetModuleSection on it: no resources, STATUS_NOT_FOUND");
            // XexGetProcedureAddress on the module (Xenia xex_module.cc:144): ordinal 1 -> 94010010, 2 -> 94010000;
            // 3 (past the count) and 0 (below the base: 0xFFFF in 16 bits) DRIVER_ENTRYPOINT_NOT_FOUND with 0.
            auto exportOf = [&](uint32_t ordinal) {
                PPCContext c{};
                c.r3.u32 = ldr, c.r4.u32 = ordinal, c.r5.u32 = fileInfo;
                Store32(base, fileInfo, 0xAAAAAAAA);
                imp("XexGetProcedureAddress")(c, base);
                return c.r3.u32;
            };
            const bool ordinals = exportOf(1) == 0 && be32(fileInfo) == 0x94010010u && exportOf(2) == 0 && be32(fileInfo) == 0x94010000u;
            Expect(ordinals && exportOf(3) == 0xC0000263 && be32(fileInfo) == 0 && exportOf(0) == 0xC0000263,
                   "XexGetProcedureAddress(module, 1 / 2): its export table; 3 and 0 DRIVER_ENTRYPOINT_NOT_FOUND, 0");
            Expect(load("game:\\Missing.xex") == 0xC000000F && be32(handleOut) == 0 &&
                       ExitCodeOf([&] { (void)load("game:\\Media\\Empty.bin"); }) == int(StopCode::Unimplemented) &&
                       ExitCodeOf([&] { (void)load("game:\\Media\\Cars.bin"); }) == int(StopCode::Setup) &&
                       StderrOf([&] { (void)load("game:\\Media\\Cars.bin"); }).find("is not the file badcrc was translated from") != std::string::npos,
                   "XexLoadImage: not on the disc NO_SUCH_FILE, handle 0; an untranslated file stops (7); a file whose CRC differs stops (9)");
            ModuleImage outside = images[0];
            outside.base = 0x70000000u;
            ModuleImage unaligned = images[0];
            unaligned.base = 0x94008000u;
            Expect(ModuleFits(images[0]) && !ModuleFits(outside) && !ModuleFits(unaligned) && ModulesOverlap(0x9401F000u, 0x94030000u) &&
                       !ModulesOverlap(0x94030000u, 0x94040000u) &&
                       ExitCodeOf([&] { (void)MapModule(base, images[0], moduleImage, std::vector<uint8_t>(moduleFile.begin(), moduleFile.begin() + 0x140)); }) == int(StopCode::Setup),
                   "ModuleFits: 80000000-A0000000, 64 KB aligned; a mapped span overlaps; mapping it again stops (9)");
            dllMainResult = 0;
            ModuleImage second = images[0];
            second.base = 0x96000000u, second.entry = 0x96010000u, second.codeBase = 0x96010000u;
            static const Section sections2[] = {{0x96000000u, 0x96010000u, false}, {0x96010000u, 0x96010100u, true}};
            static const ModuleImport imports2[] = {{0x96000104u, true, 0, "__imp__Unknown"}};
            static const PPCFuncMapping mappings2[] = {{0x96010000u, ModuleDllMain}, {0, nullptr}};
            second.sections = sections2, second.imports = imports2, second.importCount = 1, second.mappings = mappings2;
            Expect(ExitCodeOf([&] { (void)MapModule(base, second, moduleImage, std::vector<uint8_t>(moduleFile.begin(), moduleFile.begin() + 0x140)); }) == int(StopCode::Setup),
                   "a module importing data the executable does not stops (9)");
            second.importCount = 0;
            images[1] = second, images[1].fileName = "Mod.xex", images[0].fileName = "Other.xex";
            Expect(ExitCodeOf([&] { (void)load("game:\\Mod.xex"); }) == int(StopCode::Unimplemented), "a DllMain returning FALSE stops (7)");
            dllMainResult = 1;

            // VdInitializeEngines (Xenia xboxkrnl_video.cc:235; HLE): FM4's shape (r3 511C0000, a translated callback,
            // its argument, PFP and ME microcode in the executable's image 40000000-40F50000 here) -> 1, recorded.
            auto engines = [&](uint32_t callback, uint32_t pfp, uint32_t me) {
                PPCContext c{};
                c.r3.u32 = 0x511C0000, c.r4.u32 = callback, c.r5.u32 = 0x1234, c.r6.u32 = pfp, c.r7.u32 = me;
                imp("VdInitializeEngines")(c, base);
                return c.r3.u32;
            };
            Expect(ExitCodeOf([&] { (void)engines(0x94010004u, 0x40001000, 0x40002000); }) == int(StopCode::Unimplemented) &&
                       ExitCodeOf([&] { (void)engines(0x94010000u, 0x50001000, 0x40002000); }) == int(StopCode::Unimplemented) &&
                       ExitCodeOf([&] { (void)engines(0x94010000u, 0x40001000, 0x3FFFFFFC); }) == int(StopCode::Unimplemented),
                   "VdInitializeEngines: a callback that is no translated function, or microcode outside the image, stops (7)");
            const bool first = engines(0x94010000u, 0x40001000, 0x40002000) == 1;
            const gpu::Engines& e = gpu::EngineState();
            Expect(first && e.initialized && e.callback == 0x94010000u && e.callbackArg == 0x1234 && e.pfp == 0x40001000 && e.me == 0x40002000 &&
                       ExitCodeOf([&] { (void)engines(0x94010000u, 0x40001000, 0x40002000); }) == int(StopCode::Unimplemented),
                   "VdInitializeEngines: 1, the callback, argument and microcode recorded; a second call stops (7)");
            // ExRegisterTitleTerminateNotification({routine, priority}, create) (Xenia xboxkrnl_modules.cc:206): add
            // {94010000, 1}, add {94010010, 2}, remove 94010000 -> [{94010010, 2}]; a routine that is no function stops.
            const uint32_t registration = ArenaAlloc(base, 16);
            auto terminate = [&](uint32_t routine, uint32_t priority, uint32_t create) {
                Store32(base, registration, routine), Store32(base, registration + 4, priority);
                PPCContext c{};
                c.r3.u32 = registration, c.r4.u32 = create;
                imp("ExRegisterTitleTerminateNotification")(c, base);
            };
            terminate(0x94010000u, 1, 1), terminate(0x94010010u, 2, 1), terminate(0x94010000u, 1, 0);
            const auto notifications = TitleTerminateNotifications();
            Expect(notifications.size() == 1 && notifications[0].first == 0x94010010u && notifications[0].second == 2 &&
                       ExitCodeOf([&] { terminate(0x94010004u, 1, 1); }) == int(StopCode::Unimplemented),
                   "ExRegisterTitleTerminateNotification: added and removed by routine; a routine that is no function stops (7)");
        }
        symlink((disc + "/Media").c_str(), (disc + "/Link").c_str());
        Expect(ExitCodeOf([&] { (void)query("game:\\Link"); }) == int(StopCode::Setup), "a symlink on the disc stops (9)");
        unlink((disc + "/Link").c_str());
    }
    PPCContext gm{};
    gm.r4.u32 = handleOut;
    imp("XexGetModuleHandle")(gm, base);
    const bool nullName = gm.r3.u64 == 0 && be32(handleOut) == m.ldr;
    gm.r3.u32 = guestString(""), gm.r4.u32 = handleOut;
    Store32(base, handleOut, 0);
    imp("XexGetModuleHandle")(gm, base);
    Expect(nullName && gm.r3.u64 == 0 && be32(handleOut) == m.ldr, "XexGetModuleHandle: a null or empty name gives the executable's loader entry");
    gm.r3.u32 = guestString("XAM.XEX"), gm.r4.u32 = handleOut;
    imp("XexGetModuleHandle")(gm, base);
    Expect(gm.r3.u64 == 0 && be32(handleOut) == SystemModule(base, "xam.xex") && be32(handleOut) != m.ldr && be32(handleOut) != 0,
           "XexGetModuleHandle: XAM.XEX gives xam's loader entry");
    const uint32_t otherName = guestString("default.xex");
    Expect(ExitCodeOf([&] { PPCContext c{}; c.r3.u32 = otherName, c.r4.u32 = handleOut; imp("XexGetModuleHandle")(c, base); }) == int(StopCode::Unimplemented),
           "XexGetModuleHandle: a title module name stops (7) until modules are loaded");

    // XexLoadImage(name*, flags, version, handle*) (Xenia xboxkrnl_modules.cc:94): xam.xex is always loaded:
    // its loader entry, success, and its load count (+0x40, big-endian 16 bits) goes 0 -> 1 -> 2.
    const uint32_t xam = SystemModule(base, "xam.xex");
    PPCContext li{};
    li.r3.u32 = guestString("xam.xex"), li.r4.u32 = 9, li.r6.u32 = handleOut;
    Store32(base, handleOut, 0);
    imp("XexLoadImage")(li, base);
    const bool firstLoad = li.r3.u64 == 0 && be32(handleOut) == xam && base[xam + 0x40] == 0 && base[xam + 0x41] == 1;
    li.r3.u32 = guestString("XAM.XEX");
    imp("XexLoadImage")(li, base);
    Expect(firstLoad && li.r3.u64 == 0 && be32(handleOut) == xam && base[xam + 0x40] == 0 && base[xam + 0x41] == 2,
           "XexLoadImage(xam.xex): its loader entry, success, load count 1 then 2 (any case)");

    // XexGetProcedureAddress(module, ordinal, out*) (Xenia xboxkrnl_modules.cc:160): xam ordinal 0xAFF is
    // XamPartyGetUserList (xam_table.inc:1686): a trampoline address, the same each time, outside the
    // executable's code; an indirect call to it stops like an unimplemented import (3) naming the export.
    const uint32_t procOut = ArenaAlloc(base, 4);
    auto procedure = [&](uint32_t module, uint32_t ordinal) {
        PPCContext c{};
        c.r3.u32 = module, c.r4.u32 = ordinal, c.r5.u32 = procOut;
        Store32(base, procOut, 0);
        imp("XexGetProcedureAddress")(c, base);
        return c.r3.u32;
    };
    const bool procFound = procedure(xam, 0xAFF) == 0;
    const uint32_t trampoline = be32(procOut);
    Expect(procFound && trampoline != 0 && !IsCodeAddress(trampoline, PPC_CODE_BASE, PPC_CODE_SIZE) && procedure(xam, 0xAFF) == 0 &&
               be32(procOut) == trampoline,
           "XexGetProcedureAddress(xam, 0xAFF): a trampoline, the same address each time");
    Expect(procedure(xam, 0x305) == 0 && be32(procOut) != trampoline && be32(procOut) != 0,
           "XexGetProcedureAddress(xam, 0x305 XamShowPartyUI): its own trampoline");
    PPCContext viaPointer{};
    viaPointer.lr = 0x40001234;
    Expect(StderrOf([&] { PpcCallIndirect(viaPointer, base, trampoline); }).find("[stop 3] import XamPartyGetUserList called from 40001234") == 0 &&
               ExitCodeOf([&] { PpcCallIndirect(viaPointer, base, trampoline); }) == int(StopCode::Import),
           "calling the trampoline stops like an unimplemented import (3), naming XamPartyGetUserList");
    Expect(ExitCodeOf([&] { (void)procedure(xam, 0xB01); }) == int(StopCode::Unimplemented) &&
               ExitCodeOf([&] { (void)procedure(xam, guestString("XamPartyGetUserList")); }) == int(StopCode::Unimplemented) &&
               ExitCodeOf([&] { (void)procedure(0, 1); }) == int(StopCode::Unimplemented) &&
               ExitCodeOf([&] { (void)procedure(SystemModule(base, "xboxkrnl.exe"), 0xAFF); }) == int(StopCode::Unimplemented),
           "XexGetProcedureAddress: an unknown ordinal, a name, the executable's exports, or xam's ordinal asked of xboxkrnl stop (7)");
    Expect(procedure(0x1234, 0xAFF) == 0xC0000008 && be32(procOut) == 0, "XexGetProcedureAddress: a handle that is no module is STATUS_INVALID_HANDLE");

    // XexGetModuleSection(module, name*, data*, size*) (Xenia user_module.cc:260): resource info 0x000002FF
    // at +0x13C = size 0x24 (4 + 2 x 16), then the entries "dbgstr" (40010000, 0x200) and "rsrc1234" (40020000, 0x40).
    std::vector<uint8_t> withResources = header;
    withResources.resize(0x160, 0);
    auto putR = [&](uint32_t at, uint32_t v) { for (int i = 0; i < 4; i++) withResources[at + i] = uint8_t(v >> (24 - 8 * i)); };
    putR(0x14, 4), putR(0x30, 0x000002FF), putR(0x34, 0x13C);
    putR(0x13C, 0x24);
    std::memcpy(&withResources[0x140], "dbgstr\0\0", 8), putR(0x148, 0x40010000), putR(0x14C, 0x200);
    std::memcpy(&withResources[0x150], "rsrc1234", 8), putR(0x158, 0x40020000), putR(0x15C, 0x40);
    const Module res = CreateModule(base, withResources, 0x40000000, 0x40001000);
    SetExecutableModule(res);
    const uint32_t dataOut = ArenaAlloc(base, 8), sizeOut = dataOut + 4;
    auto moduleSection = [&](uint32_t module, const char* name) {
        PPCContext c{};
        c.r3.u32 = module, c.r4.u32 = guestString(name), c.r5.u32 = dataOut, c.r6.u32 = sizeOut;
        Store32(base, dataOut, 0), Store32(base, sizeOut, 0);
        imp("XexGetModuleSection")(c, base);
        return c.r3.u32;
    };
    Expect(moduleSection(res.ldr, "dbgstr") == 0 && be32(dataOut) == 0x40010000 && be32(sizeOut) == 0x200,
           "XexGetModuleSection: dbgstr at 40010000, 0x200 bytes");
    Expect(moduleSection(res.ldr, "rsrc1234") == 0 && be32(dataOut) == 0x40020000 && be32(sizeOut) == 0x40,
           "XexGetModuleSection: an 8-character name (no terminator in the entry)");
    Expect(moduleSection(res.ldr, "rsrc12345") == 0xC0000225 && moduleSection(res.ldr, "dbg") == 0xC0000225 && be32(dataOut) == 0,
           "XexGetModuleSection: a 9-character name or a prefix is STATUS_NOT_FOUND, nothing written");
    Expect(moduleSection(0x1234, "dbgstr") == 0xC0000008 && moduleSection(m.ldr, "dbgstr") == 0xC0000008,
           "XexGetModuleSection: a handle that is no loaded module is STATUS_INVALID_HANDLE");
    const uint32_t xamLdr = SystemModule(base, "xam.xex");
    Expect(ExitCodeOf([&] { (void)moduleSection(xamLdr, "dbgstr"); }) == int(StopCode::Unimplemented),
           "XexGetModuleSection on a system module stops (7)");
    SetExecutableModule(m);

    // ExGetXConfigSetting(category, setting, buffer*, size, required*) (Xenia xboxkrnl_xconfig.cc:44):
    // values from Xenia's table; a too-small buffer is BUFFER_TOO_SMALL (C0000023) with required 0;
    // no buffer with a size is INVALID_PARAMETER_3 (C00000F1); an unmodelled setting stops (7).
    const uint32_t xbuf = ArenaAlloc(base, 8), xreq = ArenaAlloc(base, 4);
    auto xconfig = [&](uint32_t category, uint32_t setting, uint32_t buffer, uint32_t size) {
        PPCContext c{};
        c.r3.u32 = category, c.r4.u32 = setting, c.r5.u32 = buffer, c.r6.u32 = size, c.r7.u32 = xreq;
        Store32(base, xbuf, 0xAAAAAAAA), Store32(base, xbuf + 4, 0xAAAAAAAA), Store32(base, xreq, 0xAAAAAAAA);
        imp("ExGetXConfigSetting")(c, base);
        return c.r3.u32;
    };
    Expect(xconfig(3, 1, xbuf, 4) == 0 && be32(xbuf) == 0 && base[xreq] == 0 && base[xreq + 1] == 4,
           "ExGetXConfigSetting(3, 1): time-zone bias 0, required size 4 (big-endian 16 bits)");
    Expect(xconfig(3, 9, xbuf, 4) == 0 && be32(xbuf) == 1, "ExGetXConfigSetting(3, 9): language 1 (English)");
    Expect(xconfig(3, 0x0A, xbuf, 4) == 0 && be32(xbuf) == 0x00040000, "ExGetXConfigSetting(3, 0x0A): video flags 0x00040000");
    Expect(xconfig(2, 2, xbuf, 4) == 0 && be32(xbuf) == 0x00001000, "ExGetXConfigSetting(2, 2): AV region 0x00001000 (USA/Canada)");
    Expect(xconfig(3, 0x0E, xbuf, 1) == 0 && base[xbuf] == 103 && base[xbuf + 1] == 0xAA && base[xreq + 1] == 1,
           "ExGetXConfigSetting(3, 0x0E): country 103 (US), one byte written, required 1");
    Expect(xconfig(3, 1, xbuf, 2) == 0xC0000023 && be32(xbuf) == 0xAAAAAAAA && base[xreq] == 0 && base[xreq + 1] == 0,
           "ExGetXConfigSetting: a 2-byte buffer for a 4-byte setting is BUFFER_TOO_SMALL, nothing written, required 0");
    Expect(xconfig(3, 1, 0, 4) == 0xC00000F1 && xconfig(3, 1, 0, 0) == 0 && base[xreq + 1] == 4,
           "ExGetXConfigSetting: no buffer with a size is INVALID_PARAMETER_3; no buffer, size 0 asks the size (4)");
    Expect(ExitCodeOf([&] { (void)xconfig(3, 0x0B, xbuf, 4); }) == int(StopCode::Unimplemented) &&
               ExitCodeOf([&] { (void)xconfig(1, 1, xbuf, 4); }) == int(StopCode::Unimplemented),
           "ExGetXConfigSetting: an unmodelled setting or category stops (7)");

    // Critical sections on the thread above (r13 = its PCR).
    PPCContext cs{};
    cs.r13.u32 = t.pcr;
    const uint32_t section = ArenaAlloc(base, 28);
    cs.r3.u32 = section; imp("RtlInitializeCriticalSection")(cs, base);
    Expect(base[section] == 1 && be32(section + 0x10) == 0xFFFFFFFF, "RtlInitializeCriticalSection: synchronization event, lock count -1");
    cs.r3.u32 = section; imp("RtlEnterCriticalSection")(cs, base);
    cs.r3.u32 = section; imp("RtlEnterCriticalSection")(cs, base);
    Expect(be32(section + 0x10) == 1 && be32(section + 0x14) == 2 && be32(section + 0x18) == t.kthread,
           "entered twice by its owner: lock count 1, recursion 2, owner the KTHREAD");
    cs.r3.u32 = section; imp("RtlLeaveCriticalSection")(cs, base);
    cs.r3.u32 = section; imp("RtlLeaveCriticalSection")(cs, base);
    Expect(be32(section + 0x10) == 0xFFFFFFFF && be32(section + 0x14) == 0 && be32(section + 0x18) == 0, "left twice: free again");
    Expect(ExitCodeOf([&] { PPCContext o{}; o.r13.u32 = t.pcr; o.r3.u32 = section; imp("RtlLeaveCriticalSection")(o, base); }) ==
               int(StopCode::GuestFault), "leaving a critical section it does not own stops (5)");

    // Events: a synchronization event is consumed by the wait it ends; a notification event stays set.
    const uint32_t ev = ArenaAlloc(base, 16);
    base[ev] = dispatch::kSynchronizationEvent;
    const int64_t poll = 0;
    Expect(!WaitForObject(base, ev, &poll), "an unset event: a poll times out");
    SetEvent(base, ev);
    Expect(WaitForObject(base, ev, &poll) && !WaitForObject(base, ev, &poll), "a synchronization event wakes exactly one wait");
    base[ev] = dispatch::kNotificationEvent;
    SetEvent(base, ev);
    Expect(WaitForObject(base, ev, &poll) && WaitForObject(base, ev, &poll), "a notification event stays signalled");

    // ---- M3 unit B: virtual memory (kernel/virtual_memory.*, imports_memory.cpp). Taken from
    // ForzaRecomp tests/runtime/test_runtime.cpp (its expected values, worked out by hand
    // from Xenia 67f16c4 memory.cc / xboxkrnl_memory.cc), re-run here. FM4 Recomp changes: the address
    // outside the virtual heaps is 0x90000000 (this test layout's image), not a title address.
    InitializeHeaps(base);
    const uint32_t args = ArenaAlloc(base, 8);
    auto allocate = [&](uint32_t address, uint32_t size, uint32_t type, uint32_t protect) {
        StoreBigEndian32(base, args, address);
        StoreBigEndian32(base, args + 4, size);
        PPCContext c{};
        c.r3.u64 = args; c.r4.u64 = args + 4; c.r5.u64 = type; c.r6.u64 = protect;
        __imp__NtAllocateVirtualMemory(c, base);
        return c.r3.u32;
    };
    Expect(allocate(0, 0x100000, 0x60002000, 4) == 0 && be32(args) == 0x40000000 && be32(args + 4) == 0x100000,
           "reserve 1 MB with large pages: the first 64 KB-page address, size kept");
    Expect(allocate(0x40000000, 0x10000, 0x60001000, 4) == 0 && be32(args) == 0x40000000 && be32(args + 4) == 0x10000,
           "commit 64 KB at the reservation's base");
    base[0x40000000] = 0x5A;
    Expect(base[0x40000000] == 0x5A && base[0x4000FFFF] == 0, "committed memory is writable and zero");
    Expect(allocate(0x40200000, 0x10000, mem::kCommit, 4) == 0xC0000017, "negative: committing unreserved memory fails (no memory)");
    Expect(allocate(0, 0x1000, mem::kFree, 4) == 0xC000000D && allocate(0x90000000, 0x1000, mem::kCommit, 4) == 0xC000000D,
           "invalid type and an address outside the virtual heaps are invalid parameters");
    Expect(allocate(0, 0x1000, mem::kReserve | mem::kCommit, 4) == 0 && be32(args) == 0x00010000 && be32(args + 4) == 0x10000,
           "a 4 KB-page allocation skips the reserved first 64 KB; its size rounds to 64 KB");
    Expect(allocate(0, 0x10000, mem::kReserve | mem::kCommit | mem::kTopDown | mem::kLargePages, 4) == 0 &&
           be32(args) == 0x6FFF0000, "top-down large-page allocation comes from the top of the heap");
    Heap* heap = LookupHeap(0x40000000);
    RegionInfo info{};
    Expect(heap && heap->Query(0x40000000, info) && info.state == mem::kCommit && info.size == 0x10000 &&
           info.allocationBase == 0x40000000 && heap->Query(0x40010000, info) && info.state == mem::kReserve &&
           info.size == 0xF0000, "query: committed then reserved regions of one allocation");
    uint32_t released = 0;
    Expect(heap->Release(0x40000000, &released) && released == 0x100000 && !heap->Release(0x40000000, nullptr),
           "release frees the whole allocation once");
    Expect(allocate(0, 0x10000, 0x60003000, 4) == 0 && be32(args) == 0x40000000 && base[0x40000000] == 0,
           "a released page committed again reads zero");
    // NtFreeVirtualMemory(base*, size*, type, debug): a new two-page allocation at 0x40010000;
    // decommitting 0x10001 bytes covers both 64 KB pages.
    auto freeMemory = [&](uint32_t address, uint32_t size, uint32_t type) {
        StoreBigEndian32(base, args, address);
        StoreBigEndian32(base, args + 4, size);
        PPCContext c{};
        c.r3.u64 = args; c.r4.u64 = args + 4; c.r5.u64 = type;
        __imp__NtFreeVirtualMemory(c, base);
        return c.r3.u32;
    };
    Expect(allocate(0x40010000, 0x20000, mem::kReserve | mem::kCommit, 4) == 0, "a second allocation of two 64 KB pages");
    base[0x40010000] = 7;
    Expect(freeMemory(0x40010000, 0xFFFF0000, mem::kDecommit) == 0xC0000001 && !heap->Decommit(0x40010000, 0xFFFFFFFF) &&
           heap->Query(0x40010000, info) && info.state == mem::kCommit,
           "a decommit size that runs past the heap (wrapping, also to zero pages) fails and frees nothing");
    Expect(freeMemory(0x40010000, 0x10001, mem::kDecommit) == 0 && be32(args) == 0x40010000 && be32(args + 4) == 0x20000 &&
           heap->Query(0x40010000, info) && info.state == mem::kReserve && info.size == 0x20000,
           "NtFreeVirtualMemory decommit: the size rounds up to pages, which become reserved");
    Expect(freeMemory(0x40010000, 0, mem::kRelease) == 0 && be32(args + 4) == 0x20000 && heap->Query(0x40010000, info) && info.state == mem::kFree,
           "release: the whole allocation, its size written back");
    Expect(freeMemory(0x40010000, 0, mem::kRelease) == 0xC0000001 && freeMemory(0, 0, mem::kRelease) == 0xC00000A0 &&
           freeMemory(0xBC800000u, 0, mem::kRelease) == 0xC000000D,
           "a second release fails (UNSUCCESSFUL); a null base is NOT_ALLOCATED; a physical heap is an invalid parameter");
    Expect(ExitCodeOf([&] { freeMemory(0x40000000, 0, mem::kDecommit); }) == int(StopCode::Unimplemented) &&
           ExitCodeOf([&] { freeMemory(0x40000000, 0x10000, mem::kRelease); }) == int(StopCode::Unimplemented) &&
           ExitCodeOf([&] { freeMemory(0x40000100, 0x100, mem::kDecommit); }) == int(StopCode::Unimplemented) &&
           ExitCodeOf([&] { freeMemory(0x40000000, 0x10000, mem::kDecommit | mem::kRelease); }) == int(StopCode::Unimplemented),
           "decommit of size 0, release with a size, a base inside a page, both types: stop (7)");
    uint32_t old = 0;
    Expect(heap->Protect(0x40000000, 0x10000, page::kReadOnly, &old) && old == 4 && base[0x40000000] == 0,
           "protect read-only: readable");

    Expect(ExitCodeOf([&] { base[0x40000000] = 1; }) == int(StopCode::GuestFault), "a write to a read-only guest page stops (5)");

    // MmAllocatePhysicalMemoryEx(flags, size, protect, lowest, highest, alignment): whole views
    // only, highest first. A0000000 (64 KB pages) is 0x20000000 bytes, so 56 MB from the top
    // starts at C0000000 - 0x3800000 (physical 0x1C800000). The C0000000 view (16 MB pages) maps
    // the same physical memory: its top pages hold physical 0x1F000000, 0x1E000000 and
    // 0x1D000000-0x1DFFFFFF and 0x1C000000 (overlapping 0x1C800000) are taken, so the highest free
    // 16 MB page is physical 0x1B000000, C0000000 + 0x1B000000.
    auto physical = [&](uint32_t flags, uint32_t size, uint32_t protect, uint32_t lowest, uint32_t highest, uint32_t alignment) {
        PPCContext c{};
        c.r3.u64 = flags; c.r4.u64 = size; c.r5.u64 = protect; c.r6.u64 = lowest; c.r7.u64 = highest; c.r8.u64 = alignment;
        __imp__MmAllocatePhysicalMemoryEx(c, base);
        return c.r3.u32;
    };
    const uint32_t large = physical(0, 0x3800000, mem::kLargePages | page::kReadWrite, 0, 0xFFFFFFFF, 0);
    Expect(large == 0xBC800000u && base[large] == 0 && base[large + 0x37FFFFF] == 0,
           "MmAllocatePhysicalMemoryEx: 56 MB of 64 KB pages, zero, from the top of the A0000000 view");
    base[large] = 1;
    Expect(base[0xDC800000u] == 1 && (base[0xDC800000u + 0x37FFFFF] = 9, base[large + 0x37FFFFF] == 9),
           "the C0000000 view aliases the A0000000 view: one physical memory, readable and writable through both");
    const uint32_t huge = physical(0, 0x1000000, mem::k16MbPages | page::kReadWrite, 0, 0xFFFFFFFF, 0x1000000);
    Expect(huge == 0xC0000000u + 0x1B000000 && LookupHeap(huge) == PhysicalHeap(16 * 1024 * 1024) &&
           !PhysicalHeap(16 * 1024 * 1024)->AllocFixed(0xDC000000u, 0x1000000, mem::kReserve | mem::kCommit, page::kReadWrite),
           "16 MB pages skip physical memory the A0000000 view holds; a fixed allocation over it fails");
    base[huge] = 5;
    Expect(base[0xBB000000u] == 5, "and the A0000000 view sees the C0000000 allocation");
    // The E0000000 view: E0000000 + x is physical x + 0x1000. 0x1800 bytes aligned to 0x4000 from
    // the top: free physical memory below 0x1C800000 (A) and above 0x1C000000 (C) ends at
    // 0x1C800000, so the highest 4-page-aligned run of 2 pages is page 0x1C7FC (phys 0x1C7FD000).
    auto physicalOf = [&](uint32_t address) { PPCContext c{}; c.r3.u64 = address; __imp__MmGetPhysicalAddress(c, base); return c.r3.u32; };
    const uint32_t small = physical(0, 0x1800, page::kReadWrite | page::kWriteCombine, 0, 0x1FFFFFFF, 0x4000);
    uint32_t viaPhysical = 0;
    Expect(small == 0xE0000000u + 0x1C7FC000 && physicalOf(small) == 0x1C7FD000 && PhysicalToGuest(0x1C7FD010, viaPhysical) &&
           viaPhysical == small + 0x10, "4 KB pages: the E0000000 view, physical + 0x1000 both ways, below the other views' frames");
    Expect(physicalOf(large) == 0x1C800000 && physicalOf(huge + 0x10) == 0x1B000010 && PhysicalToGuest(0x1C800000, viaPhysical) &&
           viaPhysical == large && PhysicalToGuest(0x1B000010, viaPhysical) && viaPhysical == huge + 0x10 && !PhysicalToGuest(0x100000, viaPhysical),
           "MmGetPhysicalAddress: the offset in the view; a physical address maps back through its view; a free frame does not");
    base[small] = 3;
    Expect(ExitCodeOf([&] { (void)*(volatile uint8_t*)(base + 0xA0000000u + 0x1C7FD000); }) == int(StopCode::GuestFault) &&
           ExitCodeOf([&] { (void)*(volatile uint8_t*)(base + 0xC0000000u + 0x1C7FD000); }) == int(StopCode::GuestFault),
           "the E0000000 view's memory through another view faults (5): its memory is its own");
    Expect(ExitCodeOf([&] { physicalOf(0x40000000); }) == int(StopCode::Unimplemented) &&
           ExitCodeOf([&] { physicalOf(0xFFD00000u); }) == int(StopCode::Unimplemented),
           "MmGetPhysicalAddress of virtual memory or past the E0000000 view stops (7)");
    uint32_t releasedHuge = 0;
    Expect(PhysicalHeap(16 * 1024 * 1024)->Release(huge, &releasedHuge) &&
           PhysicalHeap(64 * 1024)->AllocFixed(0xBB000000u, 0x10000, mem::kReserve | mem::kCommit, page::kReadWrite) &&
           PhysicalHeap(64 * 1024)->Release(0xBB000000u, nullptr),
           "a released view's physical frames can be taken by the other view");
    {
        const uint32_t block = physical(0, 0x10000, mem::kLargePages | page::kReadWrite, 0, 0xFFFFFFFF, 0);
        PPCContext c{};
        c.r3.u64 = 1; c.r4.u64 = block;
        __imp__MmFreePhysicalMemory(c, base);
        Expect(block != 0 && PhysicalHeap(64 * 1024)->Query(block, info) && info.state == mem::kFree,
               "MmFreePhysicalMemory: the allocation is released");
        Expect(ExitCodeOf([&] { __imp__MmFreePhysicalMemory(c, base); }) == int(StopCode::Unimplemented) &&
               ExitCodeOf([&] { PPCContext v{}; v.r3.u64 = 1; v.r4.u64 = 0x40000000; __imp__MmFreePhysicalMemory(v, base); }) ==
                   int(StopCode::Unimplemented), "freeing it twice, or virtual memory, stops (7)");
    }
    Expect(physical(0, 0x1000, page::kNoAccess, 0, 0xFFFFFFFF, 0) == 0 && physical(0, 0, mem::kLargePages | page::kReadWrite, 0, 0xFFFFFFFF, 0) == 0 &&
           physical(0, 0x40000000, mem::kLargePages | page::kReadWrite, 0, 0xFFFFFFFF, 0) == 0,
           "no read or write access, no size, or more than the view: 0");
    Expect(ExitCodeOf([&] { physical(0, 0x10000, mem::kLargePages | page::kReadWrite, 0x10000, 0xFFFFFFFF, 0); }) == int(StopCode::Unimplemented) &&
           ExitCodeOf([&] { physical(0, 0x10000, mem::kLargePages | page::kReadWrite, 0, 0x1FFFFFFE, 0); }) == int(StopCode::Unimplemented) &&
           ExitCodeOf([&] { physical(0, 0x1000, page::kReadWrite, 0x1000, 0x1FD00000, 0); }) == int(StopCode::Unimplemented) &&
           ExitCodeOf([&] { physical(1, 0x10000, page::kReadWrite, 0, 0xFFFFFFFF, 0); }) == int(StopCode::Unimplemented),
           "a range narrower than the view, or non-zero flags, stop (7)");
    Expect(physical(0, 0x1000, page::kReadWrite, 0x1000, 0x1FD00FFF, 0) != 0, "the E0000000 view's exact physical range is whole");
    // MmSetAddressProtect(address, size, protect).
    auto protectRange = [&](uint32_t address, uint32_t size, uint32_t protect) {
        PPCContext c{};
        c.r3.u64 = address; c.r4.u64 = size; c.r5.u64 = protect;
        __imp__MmSetAddressProtect(c, base);
    };
    protectRange(large, 0x10000, page::kReadOnly);
    Expect(base[large] == 1 && ExitCodeOf([&] { base[large] = 2; }) == int(StopCode::GuestFault) &&
           ExitCodeOf([&] { base[large + 0x10000] = 2; }) == 0, "MmSetAddressProtect read-only: readable, writes stop (5), the next page unchanged");
    protectRange(large, 0x10000, page::kReadWrite);
    Expect(ExitCodeOf([&] { base[large] = 2; }) == 0, "read-write again: writable");
    protectRange(large, 0x10000, page::kNoCache);
    Expect(ExitCodeOf([&] { base[large] = 2; }) == 0, "no access kind: the pages stay as they were (writable)");
    auto queryProtect = [&](uint32_t address) { PPCContext c{}; c.r3.u64 = address; __imp__MmQueryAddressProtect(c, base); return c.r3.u32; };
    protectRange(large + 0x10000, 0x10000, page::kReadOnly | page::kWriteCombine);
    Expect(queryProtect(large) == page::kReadWrite && queryProtect(large + 0x1FFFF) == (page::kReadOnly | page::kWriteCombine) &&
           queryProtect(0xA0000000u) == 0 && queryProtect(0x40300000) == 0,
           "MmQueryAddressProtect: the flags as set, per page; reserved or free pages 0");
    Expect(queryProtect(t.stackBase - 4) == page::kReadWrite && queryProtect(t.stackLimit) == page::kReadWrite &&
           queryProtect(t.stackLimit - 4) == 0 && queryProtect(t.stackBase) == 0 && queryProtect(kStacksEnd - 4) == 0,
           "a guest stack: read/write inside, 0 for its guard pages and the unused range");
    Expect(ExitCodeOf([&] { queryProtect(0x90000000); }) == int(StopCode::Unimplemented), "an address outside the heaps stops (7)");
    Expect(ExitCodeOf([&] { protectRange(large, 0xFFFFFFFF, page::kReadOnly); }) == int(StopCode::Unimplemented),
           "a protection size that runs past the heap (wrapping) stops (7)");

    // ---- M3 unit C: thread layout, KeGetCurrentProcessType, affinity, critical sections, TLS slots,
    // handles and objects, guest threads, waits, semaphores, signal-and-wait, events, multiple waits,
    // time, contended critical sections. Taken from ForzaRecomp tests/runtime/test_runtime.cpp
    // (expected values from Xenia 67f16c4, worked out by hand there), re-run here. FM4 Recomp
    // changes: the TLS template and the stand-in thread functions are at this test layout's addresses.
    {
    PPC_LOOKUP_FUNC(base, uint32_t(PPC_CODE_BASE) + 0x20) = ThreadBody;
    PPC_LOOKUP_FUNC(base, uint32_t(PPC_CODE_BASE) + 0x24) = XapiStartup;
    PPC_LOOKUP_FUNC(base, uint32_t(PPC_CODE_BASE) + 0x28) = Terminating;
    PPC_LOOKUP_FUNC(base, uint32_t(PPC_CODE_BASE) + 0x2C) = TerminatingThroughImport;
    // Thread layout (M3-T01): a TLS template of 0x20 bytes at a test address, 4 slots, 0x30 data.
    const uint32_t tmpl = uint32_t(PPC_IMAGE_BASE) + 0x3000;
    CommitGuestMemory(base, tmpl, 0x1000);
    for (uint32_t i = 0; i < 0x20; i++) base[tmpl + i] = uint8_t(0xA0 + i);
    const TlsInfo tls{4, tmpl, 0x30, 0x20};
    const uint32_t process = CreateProcessBlock(base, tls);
    Expect(be32(process + process::kTlsDataSize) == 0x30 && base[process + process::kProcessType] == kProcessTypeTitle &&
           base[process + process::kTlsSlotSize] == 0 && base[process + process::kTlsSlotSize + 1] == 16,
           "process block: TLS sizes and title process type, big-endian");
    const ThreadState t = CreateThreadState(base, tls, process, 0x10000, 7, 0x12340000, 0, 2);
    Expect(be32(t.pcr + pcr::kTls) == t.tls && be32(t.pcr + pcr::kCurrentThread) == t.kthread &&
           be32(t.pcr + pcr::kSelf) == t.pcr && be32(t.pcr + pcr::kStackBase) == t.stackBase &&
           be32(t.pcr + pcr::kStackEnd) == t.stackLimit && base[t.pcr + pcr::kCurrentCpu] == 2,
           "PCR: TLS, self, stack bounds, current thread, CPU");
    Expect(be32(t.kthread + kthread::kThreadId) == 7 && be32(t.kthread + kthread::kProcess) == process &&
           be32(t.kthread + kthread::kStartAddress) == 0x12340000 && be32(t.kthread + kthread::kTls) == t.tls &&
           base[t.kthread] == 6 && be32(t.kthread + 0x10) == t.kthread + 0x10 && be32(t.kthread + kthread::kLastError) == 0,
           "KTHREAD: type, list head, id, process, start address, TLS, last error");
    Expect(base[t.tls] == 0xA0 && base[t.tls + 0x1F] == 0xBF && base[t.tls + 0x20] == 0 && base[t.tls + 0x30 + 15] == 0,
           "TLS: template copied, the rest of the data and the slots zero");
    Expect(t.stackBase - t.stackLimit == 0x10000 && t.kthread - t.tls >= 0x40 && t.pcr - t.kthread >= kKthreadSize,
           "stack size; TLS, KTHREAD and PCR do not overlap");
    PPCContext self{};
    self.r13.u64 = t.pcr;
    __imp__KeGetCurrentProcessType(self, base);
    Expect(self.r3.u64 == kProcessTypeTitle, "KeGetCurrentProcessType: the title process (1)");
    base[process + process::kProcessType] = 2;
    __imp__KeGetCurrentProcessType(self, base);
    Expect(self.r3.u64 == 2, "KeGetCurrentProcessType reads the process block through PCR and KTHREAD");
    base[process + process::kProcessType] = kProcessTypeTitle;
    const uint32_t previous = ArenaAlloc(base, 4);
    self.r3.u64 = t.kthread; self.r4.u64 = 0x04 | 0x01; self.r5.u64 = previous;
    __imp__KeSetAffinityThread(self, base);
    Expect(self.r3.u64 == 0 && be32(previous) == 1u << 2 && base[t.kthread + kthread::kCurrentCpu] == 2 &&
           base[t.pcr + pcr::kCurrentCpu] == 2, "KeSetAffinityThread: previous mask out, highest CPU in KTHREAD and PCR");
    self.r3.u64 = t.kthread; self.r4.u64 = 0x08; self.r5.u64 = previous;
    __imp__KeSetAffinityThread(self, base);
    Expect(base[t.kthread + kthread::kCurrentCpu] == 3 && base[t.pcr + pcr::kCurrentCpu] == 3 && be32(previous) == 4,
           "a second call reports the first CPU; KTHREAD and PCR move to CPU 3");
    self.r3.u64 = t.kthread; self.r4.u64 = 0;
    __imp__KeSetAffinityThread(self, base);
    Expect(self.r3.u64 == 0xC000000D, "affinity 0 is an invalid parameter");
    base[t.kthread + kthread::kCurrentCpu] = 2; base[t.pcr + pcr::kCurrentCpu] = 2;

    // Critical sections (M3-T03): every field big-endian, owner = KTHREAD.
    const ThreadState other = CreateThreadState(base, tls, process, 0x4000, 9, 0, 0, 1);
    const uint32_t cs = ArenaAlloc(base, 28);
    std::memset(base + cs, 0xEE, 28);
    auto invoke = [&](void (*f)(PPCContext&, uint8_t*), uint32_t pcrAddress, uint32_t r3, uint32_t r4 = 0) {
        PPCContext c{};
        c.r13.u64 = pcrAddress; c.r3.u64 = r3; c.r4.u64 = r4;
        f(c, base);
        return c.r3.u32;
    };
    invoke(__imp__RtlInitializeCriticalSection, t.pcr, cs);
    const uint8_t initial[28] = {1, 0, 0xEE, 0xEE, 0, 0, 0, 0, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
                                 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
    Expect(std::memcmp(base + cs, initial, 28) == 0,
           "initialize: type 1, spin 0, signal 0, lock -1, recursion 0, owner 0; size and wait list untouched");
    for (auto [spin, packed] : {std::pair{0u, 0}, {1u, 1}, {256u, 1}, {257u, 2}, {65280u, 255}, {0xFFFFFFFFu, 255}})
    {
        invoke(__imp__RtlInitializeCriticalSectionAndSpinCount, t.pcr, cs, spin);
        Expect(base[cs + 1] == packed, "spin count packed as count / 256, rounded up, at most 255");
    }
    invoke(__imp__RtlEnterCriticalSection, t.pcr, cs);
    invoke(__imp__RtlEnterCriticalSection, t.pcr, cs);
    invoke(__imp__RtlEnterCriticalSection, t.pcr, cs);
    const uint8_t owned[12] = {0, 0, 0, 2, 0, 0, 0, 3, uint8_t(t.kthread >> 24), uint8_t(t.kthread >> 16), uint8_t(t.kthread >> 8), uint8_t(t.kthread)};
    Expect(std::memcmp(base + cs + 0x10, owned, 12) == 0, "entered three times: lock 2, recursion 3, owner KTHREAD, all big-endian");
    Expect(invoke(__imp__RtlTryEnterCriticalSection, other.pcr, cs) == 0, "try-enter by another thread fails");
    invoke(__imp__RtlLeaveCriticalSection, t.pcr, cs);
    invoke(__imp__RtlLeaveCriticalSection, t.pcr, cs);
    invoke(__imp__RtlLeaveCriticalSection, t.pcr, cs);
    Expect(be32(cs + 0x10) == 0xFFFFFFFF && be32(cs + 0x14) == 0 && be32(cs + 0x18) == 0, "left three times: free again");
    Expect(invoke(__imp__RtlTryEnterCriticalSection, other.pcr, cs) == 1 && be32(cs + 0x18) == other.kthread,
           "try-enter of a free section by another thread succeeds");
    Expect(ExitCodeOf([&] { invoke(__imp__RtlLeaveCriticalSection, t.pcr, cs); }) == int(StopCode::GuestFault),
           "leave by a thread that does not own it stops (5)");
    invoke(__imp__RtlLeaveCriticalSection, other.pcr, cs);
    Expect(ExitCodeOf([&] { invoke(__imp__RtlEnterCriticalSection, t.pcr, 0); }) == int(StopCode::GuestFault),
           "a null critical section stops (5)");

    // Dynamic TLS: 4 slots (the test TLS info), values per thread, after the 0x30-byte static data.
    const uint32_t slotA = invoke(__imp__KeTlsAlloc, t.pcr, 0), slotB = invoke(__imp__KeTlsAlloc, t.pcr, 0);
    invoke(__imp__KeTlsSetValue, t.pcr, slotB, 0xCAFEF00D);
    invoke(__imp__KeTlsSetValue, other.pcr, slotB, 0x12345678);
    Expect(slotA == 0 && slotB == 1 && invoke(__imp__KeTlsGetValue, t.pcr, slotB) == 0xCAFEF00D &&
           invoke(__imp__KeTlsGetValue, other.pcr, slotB) == 0x12345678 && be32(t.tls + 0x30 + 4) == 0xCAFEF00D,
           "TLS slots: allocated in order, per-thread values after the static data, big-endian");
    invoke(__imp__KeTlsAlloc, t.pcr, 0);
    invoke(__imp__KeTlsAlloc, t.pcr, 0);
    Expect(invoke(__imp__KeTlsAlloc, t.pcr, 0) == 0xFFFFFFFF, "a fifth slot is TLS_OUT_OF_INDEXES (4 slots)");
    Expect(invoke(__imp__KeTlsFree, t.pcr, slotB) == 1 && invoke(__imp__KeTlsGetValue, other.pcr, slotB) == 0 &&
           invoke(__imp__KeTlsAlloc, t.pcr, 0) == slotB, "free clears the slot in every thread and makes it available");
    Expect(ExitCodeOf([&] { invoke(__imp__KeTlsGetValue, t.pcr, 4); }) == int(StopCode::GuestFault),
           "a slot past the executable's count stops (5)");

    // Events behind handles.
    const uint32_t ev = ArenaAlloc(base, 64);
    auto createEvent = [&](uint32_t attributes, uint32_t type, uint32_t initial) {
        PPCContext c{};
        c.r13.u64 = t.pcr; c.r3.u64 = ev; c.r4.u64 = attributes; c.r5.u64 = type; c.r6.u64 = initial;
        __imp__NtCreateEvent(c, base);
        return c.r3.u32;
    };
    Expect(createEvent(0, 1, 0) == 0, "NtCreateEvent: success");
    const auto object = LookupHandle(be32(ev));
    Expect(object && object->type == ObjectType::Event && base[object->header] == 1 && be32(object->header + 4) == 0,
           "the handle names a synchronization event, not signalled");
    // Named: X_OBJECT_ATTRIBUTES at ev+8 -> X_ANSI_STRING at ev+20 -> "Ev" at ev+28.
    StoreBigEndian32(base, ev + 12, ev + 20);
    base[ev + 20] = 0; base[ev + 21] = 2; StoreBigEndian32(base, ev + 24, ev + 28);
    base[ev + 28] = 'E'; base[ev + 29] = 'v';
    Expect(createEvent(ev + 8, 0, 1) == 0, "a named notification event, signalled");
    const uint32_t first = be32(ev);
    Expect(createEvent(ev + 8, 0, 0) == 0x40000000 && be32(ev) != first && LookupHandle(be32(ev))->header == LookupHandle(first)->header,
           "the same name again: OBJECT_NAME_EXISTS and a second handle to the same event");
    Expect(ExitCodeOf([&] { createEvent(0, 2, 0); }) == int(StopCode::Unimplemented), "an unknown event type stops (7)");
    RecordDataImport("ExEventObjectType", 0x7F040000);
    RecordDataImport("ExThreadObjectType", 0x7F044000);
    auto reference = [&](uint32_t handle, uint32_t type) {
        PPCContext c{};
        c.r13.u64 = t.pcr; c.r3.u64 = handle; c.r4.u64 = type; c.r5.u64 = ev + 40;
        __imp__ObReferenceObjectByHandle(c, base);
        return c.r3.u32;
    };
    Expect(reference(first, 0x7F040000) == 0 && be32(ev + 40) == LookupHandle(first)->header,
           "ObReferenceObjectByHandle: an event handle with the event type gives the event");
    Expect(reference(first, 0x7F044000) == 0xC0000024 && be32(ev + 40) == 0, "the thread type on an event: type mismatch, out cleared");
    Expect(reference(0xFFFFFFFE, 0x7F044000) == 0 && be32(ev + 40) == t.kthread, "the current-thread pseudo-handle gives its KTHREAD");
    Expect(reference(0x1234, 0) == 0xC0000008, "an unknown handle is invalid");
    RecordDataImport("ExSemaphoreObjectType", 0x7F048000);
    auto dereference = [&](uint32_t object) { PPCContext c{}; c.r3.u64 = object; __imp__ObDereferenceObject(c, base); };
    dereference(LookupHandle(first)->header);
    dereference(t.kthread);
    Expect(ExitCodeOf([&] { dereference(ev); }) == int(StopCode::GuestFault) &&
           ExitCodeOf([&] { dereference(0); }) == int(StopCode::GuestFault),
           "ObDereferenceObject: an event and a thread pass; an unknown or null pointer stops (5)");

    // Guest threads: host functions stand in for translated ones at code addresses.
    SetTitleProcess(process, tls, 0x8000);
    uint32_t id = 0;
    uint32_t k = CreateGuestThread(base, 0, 0, uint32_t(PPC_CODE_BASE) + 0x20, 0x1234, 0, id);
    WaitForEvent(base, k);
    Expect(threadSaw == 0x1234 && be32(k + kthread_state::kExitStatus) == 0x77 && base[k + kthread_state::kTerminated] == 1 &&
           be32(k + kthread::kThreadId) == id && be32(k + 4) == 1, "a raw thread runs start(context); its result is the exit status");
    k = CreateGuestThread(base, 0x100, uint32_t(PPC_CODE_BASE) + 0x24, 0xF0F0, 0x0F0F, 0, id);
    WaitForEvent(base, k);
    Expect(threadSaw == 0xFFFF && be32(k + kthread_state::kExitStatus) == 0 &&
           be32(k + kthread::kStackBase) - be32(k + kthread::kStackLimit) == 0x4000,
           "an XAPI thread runs startup(start, context), exit status 0; a tiny stack becomes 16 KB");
    threadSaw = 0;
    k = CreateGuestThread(base, 0, 0, uint32_t(PPC_CODE_BASE) + 0x20, 0x4321, 1, id);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool waited = threadSaw == 0 && base[k + kthread_state::kSuspendCount] == 1;
    Expect(ResumeGuestThread(base, k) == 1, "resume returns the previous suspend count");
    WaitForEvent(base, k);
    Expect(waited && threadSaw == 0x4321, "a thread created suspended runs only after resume");
    k = CreateGuestThread(base, 0, 0, uint32_t(PPC_CODE_BASE) + 0x28, 0, 0, id);
    WaitForEvent(base, k);
    Expect(be32(k + kthread_state::kExitStatus) == 0x55, "ExTerminateThread ends the thread with its exit code");
    k = CreateGuestThread(base, 0, 0, uint32_t(PPC_CODE_BASE) + 0x2C, 0, 0, id);
    WaitForEvent(base, k);
    Expect(be32(k + kthread_state::kExitStatus) == 0x66 && base[k + kthread_state::kTerminated] == 1,
           "the ExTerminateThread import ends the calling thread with its exit code (0x66)");

    threadSaw = 0;
    k = CreateGuestThread(base, 0, 0, uint32_t(PPC_CODE_BASE) + 0x20, 0x5150, 1, id);
    const uint32_t threadHandle = CreateObject(ObjectType::Thread, k, "");
    PPCContext suspend{};
    suspend.r13.u64 = t.pcr; suspend.r3.u64 = threadHandle; suspend.r4.u64 = ev + 48;
    __imp__NtSuspendThread(suspend, base);
    Expect(suspend.r3.u64 == 0 && be32(ev + 48) == 1 && base[k + kthread_state::kSuspendCount] == 2,
           "NtSuspendThread of a thread not yet started: count 1 -> 2, previous 1");
    PPCContext resume{};
    resume.r13.u64 = t.pcr; resume.r3.u64 = threadHandle; resume.r4.u64 = ev + 48;
    __imp__NtResumeThread(resume, base);
    Expect(threadSaw == 0 && be32(ev + 48) == 2, "one resume is not enough: it stays parked (previous 2)");
    resume.r3.u64 = threadHandle;  // the call overwrote r3 with its result
    __imp__NtResumeThread(resume, base);
    WaitForEvent(base, k);
    Expect(resume.r3.u64 == 0 && be32(ev + 48) == 1 && threadSaw == 0x5150, "NtResumeThread: starts the thread, previous count 1");
    Expect(ExitCodeOf([&] { PPCContext c = suspend; c.r3.u64 = threadHandle; __imp__NtSuspendThread(c, base); }) == int(StopCode::Unimplemented),
           "suspending a thread that has run, from another thread, stops (7)");
    resume.r3.u64 = first; resume.r4.u64 = 0;
    __imp__NtResumeThread(resume, base);
    Expect(resume.r3.u64 == 0xC0000024, "NtResumeThread on an event handle: type mismatch");
    auto duplicate = [&](uint32_t handle, uint32_t options) {
        PPCContext c{};
        c.r13.u64 = t.pcr; c.r3.u64 = handle; c.r4.u64 = ev + 52; c.r5.u64 = options;
        __imp__NtDuplicateObject(c, base);
        return c.r3.u32;
    };
    Expect(duplicate(0xFFFFFFFE, 0) == 0 && LookupHandle(be32(ev + 52))->header == t.kthread &&
           LookupHandle(be32(ev + 52))->type == ObjectType::Thread, "NtDuplicateObject(current thread): a real handle to it");
    Expect(duplicate(first, 1) == 0 && LookupHandle(be32(ev + 52))->header == LookupHandle(be32(ev))->header &&
           !LookupHandle(first), "duplicating with DUPLICATE_CLOSE_SOURCE: same object, source closed");
    Expect(duplicate(first, 0) == 0xC0000008, "a closed handle is invalid");
    PPCContext closing{};
    closing.r3.u64 = be32(ev + 52);
    __imp__NtClose(closing, base);
    const uint32_t closed = closing.r3.u32;
    closing.r3.u64 = be32(ev + 52);
    __imp__NtClose(closing, base);
    Expect(closed == 0 && closing.r3.u32 == 0xC0000008 && LookupHandle(be32(ev)), "NtClose: once, then invalid; other handles stay");
    auto waitOn = [&](uint32_t handle, int64_t* timeout) {
        PPCContext c{};
        c.r13.u64 = t.pcr; c.r3.u64 = handle;
        if (timeout) { StoreBigEndian32(base, ev + 56, uint32_t(uint64_t(*timeout) >> 32)); StoreBigEndian32(base, ev + 60, uint32_t(*timeout)); }
        c.r6.u64 = timeout ? ev + 56 : 0;
        __imp__NtWaitForSingleObjectEx(c, base);
        return c.r3.u32;
    };
    createEvent(0, 1, 1);  // a signalled synchronization event
    const uint32_t sync = be32(ev);
    int64_t zero = 0, tenMs = -100000;
    const auto before = std::chrono::steady_clock::now();
    Expect(waitOn(sync, &zero) == 0 && waitOn(sync, &tenMs) == 0x102 &&
           std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(10),
           "NtWaitForSingleObjectEx: a signalled synchronization event satisfies one wait, the next times out after 10 ms");
    SetEvent(base, LookupHandle(sync)->header);
    Expect(waitOn(sync, nullptr) == 0, "an infinite wait on a signalled event returns");
    Expect(waitOn(threadHandle, &zero) == 0, "a finished thread's handle is signalled");
    PPCContext sem{};
    sem.r3.u64 = ev; sem.r5.u64 = 1; sem.r6.u64 = 2;
    __imp__NtCreateSemaphore(sem, base);
    const uint32_t semaphore = be32(ev);
    Expect(sem.r3.u64 == 0 && waitOn(semaphore, &zero) == 0 && waitOn(semaphore, &zero) == 0x102,
           "NtCreateSemaphore(count 1): one wait succeeds, the next times out");
    auto release = [&](uint32_t n) { PPCContext c{}; c.r3.u64 = semaphore; c.r4.u64 = n; c.r5.u64 = ev + 56; __imp__NtReleaseSemaphore(c, base); return c.r3.u32; };
    Expect(release(2) == 0 && be32(ev + 56) == 0 && release(1) == 0xC0000047 && be32(ev + 56) == 2,
           "NtReleaseSemaphore: up to the limit (previous 0), then LIMIT_EXCEEDED (previous 2, unchanged)");
    Expect(waitOn(semaphore, &zero) == 0 && waitOn(semaphore, &zero) == 0 && waitOn(semaphore, &zero) == 0x102,
           "two released units satisfy two waits");
    // NtSignalAndWaitForSingleObjectEx(signal, wait, mode, alertable, timeout*).
    auto signalAndWait = [&](uint32_t signal, uint32_t handle, int64_t* timeout) {
        PPCContext c{};
        c.r13.u64 = t.pcr; c.r3.u64 = signal; c.r4.u64 = handle; c.r5.u64 = 1; c.r6.u64 = 1;
        if (timeout) { StoreBigEndian32(base, ev + 56, uint32_t(uint64_t(*timeout) >> 32)); StoreBigEndian32(base, ev + 60, uint32_t(*timeout)); }
        c.r7.u64 = timeout ? ev + 56 : 0;
        __imp__NtSignalAndWaitForSingleObjectEx(c, base);
        return c.r3.u32;
    };
    createEvent(0, 0, 1);
    const uint32_t notification = be32(ev);
    createEvent(0, 0, 0);
    const uint32_t quiet = be32(ev);
    const uint32_t semaphoreHeader = LookupHandle(semaphore)->header, syncHeader = LookupHandle(sync)->header;
    Expect(signalAndWait(semaphore, notification, &zero) == 0 && be32(semaphoreHeader + 4) == 1 &&
           be32(LookupHandle(notification)->header + 4) == 1,
           "NtSignalAndWaitForSingleObjectEx: a semaphore unit released, a notification event's wait satisfied (it stays set)");
    Expect(signalAndWait(sync, sync, &zero) == 0 && be32(syncHeader + 4) == 0,
           "signalling and waiting for one synchronization event: the wait takes the signal");
    Expect(signalAndWait(quiet, sync, &zero) == 0x102 && be32(LookupHandle(quiet)->header + 4) == 1,
           "the signal happens even when the wait times out");
    Expect(signalAndWait(semaphore, notification, &zero) == 0 && signalAndWait(semaphore, notification, &zero) == 0xC0000047 &&
           be32(semaphoreHeader + 4) == 2, "a semaphore at its limit: LIMIT_EXCEEDED, count unchanged");
    Expect(signalAndWait(0x1234, notification, &zero) == 0xC0000008 && signalAndWait(notification, 0x1234, &zero) == 0xC0000008,
           "an unknown handle is invalid");
    Expect(ExitCodeOf([&] { signalAndWait(threadHandle, notification, &zero); }) == int(StopCode::Unimplemented),
           "signalling a thread stops (7)");
    auto setEvent = [&](uint32_t handle, uint32_t previousOut) {
        PPCContext c{};
        c.r3.u64 = handle; c.r4.u64 = previousOut;
        StoreBigEndian32(base, ev + 56, 0xEEEEEEEE);
        __imp__NtSetEvent(c, base);
        return c.r3.u32;
    };
    ResetEvent(base, LookupHandle(quiet)->header);
    Expect(setEvent(quiet, ev + 56) == 0 && be32(ev + 56) == 0 && be32(LookupHandle(quiet)->header + 4) == 1 &&
           setEvent(quiet, ev + 56) == 0 && be32(ev + 56) == 1 && setEvent(quiet, 0) == 0,
           "NtSetEvent: signalled, the previous state written (0, then 1), none asked for");
    Expect(setEvent(semaphore, 0) == 0xC0000024 && setEvent(0x1234, 0) == 0xC0000008 && be32(semaphoreHeader + 4) == 2,
           "NtSetEvent on a semaphore: OBJECT_TYPE_MISMATCH; an unknown handle: INVALID_HANDLE");
    auto keWait = [&](uint32_t object, int64_t* timeout) {
        PPCContext c{};
        c.r3.u64 = object; c.r4.u64 = 3; c.r5.u64 = 1;
        if (timeout) { StoreBigEndian32(base, ev + 56, uint32_t(uint64_t(*timeout) >> 32)); StoreBigEndian32(base, ev + 60, uint32_t(*timeout)); }
        c.r7.u64 = timeout ? ev + 56 : 0;
        __imp__KeWaitForSingleObject(c, base);
        return c.r3.u32;
    };
    SetEvent(base, LookupHandle(quiet)->header);
    Expect(keWait(LookupHandle(quiet)->header, &zero) == 0 && keWait(LookupHandle(sync)->header, &zero) == 0x102,
           "KeWaitForSingleObject by object pointer: a signalled event satisfies it; an unsignalled one times out");
    Expect(ExitCodeOf([&] { keWait(0, &zero); }) == int(StopCode::GuestFault), "a null object stops (5)");
    {
        const uint32_t kevent = ArenaAlloc(base, 16);  // a guest KEVENT: synchronization, not signalled
        base[kevent] = 1;
        PPCContext k{};
        k.r3.u64 = kevent; k.r4.u64 = 1;
        __imp__KeSetEvent(k, base);
        const uint32_t before = k.r3.u32, state = be32(kevent + 4);
        k.r3.u64 = kevent;
        __imp__KeResetEvent(k, base);
        Expect(before == 0 && state == 1 && k.r3.u32 == 1 && be32(kevent + 4) == 0,
               "KeSetEvent / KeResetEvent on a guest KEVENT: previous state returned, the signal state set then cleared");
        base[kevent] = 5;
        Expect(ExitCodeOf([&] { PPCContext c{}; c.r3.u64 = kevent; __imp__KeSetEvent(c, base); }) == int(StopCode::Unimplemented),
               "KeSetEvent on another dispatcher object stops (7)");
    }
    auto clearEvent = [&](uint32_t handle) { PPCContext c{}; c.r3.u64 = handle; __imp__NtClearEvent(c, base); return c.r3.u32; };
    Expect(clearEvent(quiet) == 0 && be32(LookupHandle(quiet)->header + 4) == 0 && clearEvent(quiet) == 0 &&
           clearEvent(semaphore) == 0xC0000024 && be32(semaphoreHeader + 4) == 2 && clearEvent(0x1234) == 0xC0000008,
           "NtClearEvent: reset (twice is fine); a semaphore: OBJECT_TYPE_MISMATCH, count kept; unknown: INVALID_HANDLE");
    // NtWaitForMultipleObjectsEx(count, handles*, type 0 all / 1 any, mode, alertable, timeout*).
    const uint32_t list = ArenaAlloc(base, 16);
    auto waitMany = [&](std::vector<uint32_t> handles, uint32_t type, int64_t* timeout) {
        for (size_t i = 0; i < handles.size(); i++) StoreBigEndian32(base, list + 4 * uint32_t(i), handles[i]);
        PPCContext c{};
        c.r13.u64 = t.pcr; c.r3.u64 = handles.size(); c.r4.u64 = list; c.r5.u64 = type; c.r6.u64 = 1;
        if (timeout) { StoreBigEndian32(base, ev + 56, uint32_t(uint64_t(*timeout) >> 32)); StoreBigEndian32(base, ev + 60, uint32_t(*timeout)); }
        c.r8.u64 = timeout ? ev + 56 : 0;
        __imp__NtWaitForMultipleObjectsEx(c, base);
        return c.r3.u32;
    };
    const uint32_t quietHeader = LookupHandle(quiet)->header;
    ResetEvent(base, quietHeader);
    SetEvent(base, syncHeader);
    Expect(waitMany({quiet, sync}, 1, &zero) == 1 && be32(syncHeader + 4) == 0,
           "NtWaitForMultipleObjectsEx any: the index of the signalled object (1), whose synchronization event is reset");
    SetEvent(base, syncHeader);
    Expect(waitMany({quiet, sync}, 0, &zero) == 0x102 && be32(syncHeader + 4) == 1, "all: one unsignalled object times the wait out, nothing consumed");
    SetEvent(base, quietHeader);
    Expect(waitMany({quiet, sync, semaphore}, 0, &zero) == 0 && be32(syncHeader + 4) == 0 && be32(quietHeader + 4) == 1 &&
           be32(semaphoreHeader + 4) == 1, "all signalled: success; each consumed (synchronization reset, notification kept, semaphore - 1)");
    Expect(waitMany({sync, quiet, semaphore}, 1, &zero) == 1 && be32(semaphoreHeader + 4) == 1, "any: the first signalled in list order");
    Expect(waitMany({}, 1, &zero) == 0xC000000D && waitMany({quiet}, 2, &zero) == 0xC000000D && waitMany({quiet, 0x1234}, 1, &zero) == 0xC000000D,
           "no objects, an unknown wait type or an unknown handle: invalid parameter");
    Expect(ExitCodeOf([&] { waitMany({quiet, quiet}, 0, &zero); }) == int(StopCode::Unimplemented), "an object twice in a wait for all stops (7)");
    {
        PPCContext c{};
        c.r3.u64 = 65; c.r4.u64 = list; c.r5.u64 = 1;
        __imp__NtWaitForMultipleObjectsEx(c, base);
        Expect(c.r3.u32 == 0xC000000D, "more than 64 objects: invalid parameter");
    }
    {
        // KeWaitForMultipleObjects(count, objects*, type, reason, mode, alertable, timeout*, blocks*):
        // object pointers (dispatcher headers), the timeout in r9.
        const uint32_t objects = ArenaAlloc(base, 16);
        auto keWaitMany = [&](std::vector<uint32_t> headers, uint32_t type) {
            for (size_t i = 0; i < headers.size(); i++) StoreBigEndian32(base, objects + 4 * uint32_t(i), headers[i]);
            StoreBigEndian32(base, ev + 56, 0); StoreBigEndian32(base, ev + 60, 0);
            PPCContext c{};
            c.r3.u64 = headers.size(); c.r4.u64 = objects; c.r5.u64 = type; c.r6.u64 = 3; c.r9.u64 = ev + 56;
            __imp__KeWaitForMultipleObjects(c, base);
            return c.r3.u32;
        };
        ResetEvent(base, quietHeader);
        SetEvent(base, syncHeader);
        Expect(keWaitMany({quietHeader, syncHeader}, 1) == 1 && be32(syncHeader + 4) == 0,
               "KeWaitForMultipleObjects any: the index of the signalled object (1), its synchronization event reset");
        Expect(keWaitMany({quietHeader, syncHeader}, 1) == 0x102, "none signalled: a zero timeout times out");
        SetEvent(base, quietHeader); SetEvent(base, syncHeader);
        Expect(keWaitMany({quietHeader, syncHeader}, 0) == 0 && be32(syncHeader + 4) == 0 && be32(quietHeader + 4) == 1,
               "all signalled: success, each consumed");
        Expect(ExitCodeOf([&] { keWaitMany({}, 1); }) == int(StopCode::Unimplemented) &&
                   ExitCodeOf([&] { keWaitMany({quietHeader}, 2); }) == int(StopCode::Unimplemented),
               "no objects or another wait type: not modelled, stops (7)");
        Expect(ExitCodeOf([&] { keWaitMany({quietHeader, 0}, 1); }) == int(StopCode::GuestFault), "a null object stops (5)");
    }
    // Waits across threads: a wait for all with a relative timeout of INT64_MIN (2^63 ticks:
    // effectively infinite; the host clock cannot add it, and ticks * 100 would wrap to 0) blocks
    // until another thread signals the last object;
    // an absolute timeout 20 ms ahead times out after about 20 ms.
    ResetEvent(base, quietHeader);
    SetEvent(base, syncHeader);
    const uint32_t own = ArenaAlloc(base, 16);
    StoreBigEndian32(base, own, quiet); StoreBigEndian32(base, own + 4, sync);
    StoreBigEndian32(base, own + 8, 1u << 31);  // INT64_MIN: high word 1 << 31 StoreBigEndian32(base, own + 12, 0);
    std::atomic<uint32_t> allWaited{0xFFFFFFFF};
    std::thread waiter([&] {
        PPCContext c{};
        c.r13.u64 = t.pcr; c.r3.u64 = 2; c.r4.u64 = own; c.r5.u64 = 0; c.r8.u64 = own + 8;
        __imp__NtWaitForMultipleObjectsEx(c, base);
        allWaited = c.r3.u32;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool stillWaiting = allWaited == 0xFFFFFFFF;
    SetEvent(base, quietHeader);
    waiter.join();
    Expect(stillWaiting && allWaited == 0 && be32(syncHeader + 4) == 0,
           "a wait for all with timeout INT64_MIN (relative) blocks until another thread signals the last object, then consumes them");
    // Signal-and-wait ping-pong between two threads over two synchronization events, 1,000 rounds:
    // each round's signal wakes the other side and its wait takes the reply; a lost wake-up hangs.
    createEvent(0, 1, 0);
    const uint32_t ping = be32(ev);
    createEvent(0, 1, 0);
    const uint32_t pong = be32(ev);
    std::atomic<int> rounds{0};
    std::thread partner([&] {
        for (int i = 0; i < 1000; i++)
        {
            WaitForObject(base, LookupHandle(ping)->header, nullptr);
            rounds++;
            SetEvent(base, LookupHandle(pong)->header);
        }
    });
    uint32_t pingFailures = 0;
    for (int i = 0; i < 1000; i++)
    {
        PPCContext c{};
        c.r13.u64 = t.pcr; c.r3.u64 = ping; c.r4.u64 = pong;
        __imp__NtSignalAndWaitForSingleObjectEx(c, base);
        pingFailures += c.r3.u32 != 0;
    }
    partner.join();
    Expect(pingFailures == 0 && rounds == 1000 && be32(LookupHandle(ping)->header + 4) == 0 && be32(LookupHandle(pong)->header + 4) == 0,
           "NtSignalAndWaitForSingleObjectEx ping-pong: 1,000 rounds across two threads, no wake-up lost");
    int64_t soon = int64_t(GuestSystemTime() + 200000);
    const auto started = std::chrono::steady_clock::now();
    Expect(waitOn(sync, &soon) == 0x102 && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(15),
           "an absolute timeout 20 ms ahead times out after about that long");
    sem.r3.u64 = ev; sem.r4.u64 = 0; sem.r5.u64 = 3; sem.r6.u64 = 2;
    __imp__NtCreateSemaphore(sem, base);
    Expect(sem.r3.u64 == 0xC000000D, "a count above the limit is an invalid parameter");
    Expect(reference(semaphore, 0x7F048000) == 0 && reference(semaphore, 0x7F040000) == 0xC0000024,
           "ObReferenceObjectByHandle: a semaphore matches the semaphore type, not the event type");
    PPCContext clock{};
    clock.r3.u64 = ev + 56;
    __imp__KeQuerySystemTime(clock, base);
    const uint64_t filetime = uint64_t(be32(ev + 56)) << 32 | be32(ev + 60);
    // 2026-01-01 is 134117856000000000 in 100 ns since 1601 (11644473600 s to 1970 + 1767225600 s).
    Expect(filetime > 134117856000000000ull && filetime < 134117856000000000ull + 3650ull * 86400 * 10000000,
           "KeQuerySystemTime: a FILETIME after 2026-01-01, big-endian");
    // KeQueryPerformanceFrequency: the time base (mftb) rate, 49,875,000 Hz = the Xenon clock 3.192 GHz / 64
    // (ReCarbon runtime/cpu/guest_time.h kTimebaseHz; Xenia uses a round 50,000,000).
    PPCContext frequency{};
    __imp__KeQueryPerformanceFrequency(frequency, base);
    Expect(frequency.r3.u64 == 49875000, "KeQueryPerformanceFrequency: 49,875,000 Hz (3.192 GHz / 64)");

    // Mutual exclusion: 4 threads x 2,000 entries around an unprotected counter, contended (spin 0,
    // so waiters block on the section's event).
    invoke(__imp__RtlInitializeCriticalSection, t.pcr, cs);
    std::vector<ThreadState> workers;
    for (uint32_t i = 0; i < 4; i++)
        workers.push_back(CreateThreadState(base, tls, process, 0x4000, 20 + i, 0, 0, uint8_t(i)));
    long counter = 0;
    std::vector<std::thread> threads;
    for (const auto& w : workers)
        threads.emplace_back([&, pcrAddress = w.pcr] {
            for (int i = 0; i < 2000; i++)
            {
                invoke(__imp__RtlEnterCriticalSection, pcrAddress, cs);
                const long seen = counter;
                std::this_thread::yield();
                counter = seen + 1;
                invoke(__imp__RtlLeaveCriticalSection, pcrAddress, cs);
            }
        });
    for (auto& th : threads)
        th.join();
    Expect(counter == 8000 && be32(cs + 0x10) == 0xFFFFFFFF && be32(cs + 0x18) == 0,
           "4 threads x 2,000 contended entries: no lost update, free at the end");

    const ThreadState u = CreateThreadState(base, tls, process, 0x8000, 8, 0, 0, 0);
    Expect(u.stackLimit >= t.stackBase + 16384, "consecutive stacks are separated by a guard page");
    Expect(PlaceStack(0x70000000, 0x10001, 0x4000).base == 0x70000000 + 0x4000 + 0x14000,
           "a stack size is rounded up to whole pages");
    }

    // The headless command processor (kernel/command_processor.h), on hand-built PM4 (xenos.h MakePacketType0/1/3:
    // type 0 = ((count - 1) << 16) | index | one-register << 15; type 1 = 1 << 30 | index2 << 11 | index1;
    // type 3 = 3 << 30 | (count - 1) << 16 | opcode << 8 | predicate). Memory: one 64 KB page of the A0000000 view.
    {
        using namespace gpu;
        Expect(GpuSwap(0x11223344, 0) == 0x11223344 && GpuSwap(0x11223344, 1) == 0x22114433 && GpuSwap(0x11223344, 2) == 0x44332211 &&
                   GpuSwap(0x11223344, 3) == 0x33441122,
               "GpuSwap: none, 8in16, 8in32, 16in32 (xenos.h:1070-1087)");
        const uint32_t page = PhysicalHeap(64 * 1024)->Alloc(0x10000, 0x10000, mem::kReserve | mem::kCommit, page::kReadWrite, false);
        uint32_t phys = 0;
        const bool mapped = page != 0 && PhysicalAddress(page, phys) && gpu::PhysicalToGuest(phys) == page;
        auto put = [&](uint32_t offset, std::initializer_list<uint32_t> words) { for (uint32_t w : words) { Store32(base, page + offset, w); offset += 4; } };
        CpState st;
        // Sub-buffer at +0x100: type 0 writing register 2003 = 7.
        put(0x100, {0x00002003, 7});
        put(0x200, {  // 44 words at +0x200; the writes they make land at +0, +8 and +0x10
            0x00012000, 0x11, 0x22,                     // type 0: 2000 = 11, 2001 = 22
            0x0001A002, 0x33, 0x44,                     // type 0, one register: 2002 = 33 then 44
            (1u << 30) | (0x11u << 11) | 0x10, 0x55, 0x66,  // type 1: 10 = 55, 11 = 66
            2u << 30,                                   // type 2: no-op
            0xC0022100, 0x0010, 0xFFFF, 0x05,           // REG_RMW (13-bit index): 10 = (55 & FFFF) | 5 = 55
            0xC0023D00, phys | 2, 0xAABBCCDD, 0x01020304, // MEM_WRITE 8in32 at +0: two words big-endian
            0xC0025800, 0x14, (phys + 8) | 2, 0xCAFE,     // EVENT_WRITE_SHD: initiator 14, value CAFE at +8
            0x000001DC, 1, 0x000001DD, phys + 0x10,       // SCRATCH_UMSK = 1, SCRATCH_ADDR = +0x10
            0x00000578, 0x12345678,                       // SCRATCH_REG0 -> big-endian at +0x10
            0xC0013F00, phys + 0x100, 2,                  // INDIRECT_BUFFER: +0x100, 2 words
            0xC0043C00, 3, 0x2003, 7, 0xFFFFFFFF, 0,       // WAIT_REG_MEM register 2003 == 7 (satisfied)
            0xC0015100, 0, 0,                             // SET_BIN_SELECT 0
            0xC0022101, 0x0011, 0, 0x99,                  // a predicated REG_RMW on 11: skipped
        });
        if (mapped)
            Execute(base, page + 0x200, 44, 0, 44, st);
        Expect(mapped && ReadRegister(base, 0x2000) == 0x11 && ReadRegister(base, 0x2001) == 0x22 && ReadRegister(base, 0x2002) == 0x44 &&
                   ReadRegister(base, 0x10) == 0x55 && ReadRegister(base, 0x11) == 0x66 && ReadRegister(base, 0x2003) == 7,
               "PM4: type 0 (range and one register), type 1, type 2, REG_RMW, an indirect buffer; a predicated packet skipped after SET_BIN_SELECT 0");
        Expect(be32(page) == 0xAABBCCDD && be32(page + 4) == 0x01020304 && be32(page + 8) == 0xCAFE && ReadRegister(base, reg::kVgtEventInitiator) == 0x14 &&
                   be32(page + 0x10) == 0x12345678,
               "PM4: MEM_WRITE and EVENT_WRITE_SHD with 8in32 store big-endian; a scratch register enabled in SCRATCH_UMSK is written to SCRATCH_ADDR");
        // A ring of 4 words read from word 3 with wraparound: a type-0 header at word 3, its values at words 0 and 1.
        put(0x300, {0x22, 0x33, 0, 0x00012004});
        CpState ring;
        Execute(base, page + 0x300, 4, 3, 3, ring);
        Expect(ReadRegister(base, 0x2004) == 0x22 && ReadRegister(base, 0x2005) == 0x33, "PM4: a ring read wraps around its end");
        put(0x400, {0xC0036400, 0x53574150, 0x1B441000, 1280, 720});
        put(0x420, {0xC0007F00, 0});
        put(0x430, {0xC0042100, 0x2000});
        Expect(StderrOf([&] { CpState c; Execute(base, page + 0x400, 5, 0, 5, c); }) == "[stop 11] first swap: front buffer 1B441000, 1280x720\n" &&
                   ExitCodeOf([&] { CpState c; Execute(base, page + 0x400, 5, 0, 5, c); }) == int(StopCode::FirstSwap) &&
                   ExitCodeOf([&] { CpState c; Execute(base, page + 0x420, 2, 0, 2, c); }) == int(StopCode::Unimplemented) &&
                   ExitCodeOf([&] { CpState c; Execute(base, page + 0x430, 2, 0, 2, c); }) == int(StopCode::Unimplemented),
               "PM4: XE_SWAP stops (11) with the front buffer and size; an unmodelled opcode and a packet longer than its data stop (7)");

        // VdSwap (Xenia xboxkrnl_video.cc:394-476): front buffer = this page (fetch dword 1 = page | 0x86 low bits, dword 2 =
        // height - 1 << 13 | width - 1 = 719 << 13 | 1279), format 6, colour space 0, width/height on the stack (arguments 8, 9).
        const uint32_t args = ArenaAlloc(base, 0x100), slot = page + 0x800;
        put(0x500, {0x11111111, page | 0x86, (719u << 13) | 1279, 0x44444444, 0x55555555, 0x66666666});
        Store32(base, args, page), Store32(base, args + 4, 6), Store32(base, args + 8, 0), Store32(base, args + 12, 1280), Store32(base, args + 16, 720);
        Store32(base, args + 0x80 + 0x54, args + 12), Store32(base, args + 0x80 + 0x5C, args + 16);
        PPCContext swap{};
        swap.r1.u32 = args + 0x80, swap.r3.u32 = slot, swap.r4.u32 = page + 0x500, swap.r8.u32 = args, swap.r9.u32 = args + 4, swap.r10.u32 = args + 8;
        (void)StderrOf([] {});
        imp("VdSwap")(swap, base);
        Expect(be32(slot) == 0x00054800 && be32(slot + 4) == 0x11111111 && be32(slot + 8) == ((phys & 0xFFFFF000u) | 0x86) &&
                   be32(slot + 28) == 0xC0036400 && be32(slot + 32) == 0x53574150 && be32(slot + 36) == phys && be32(slot + 40) == 1280 &&
                   be32(slot + 44) == 720 && be32(slot + 48) == (2u << 30) && be32(slot + 252) == (2u << 30),
               "VdSwap: the slot holds type 0 (6 fetch words to 4800, physical base), XE_SWAP {SWAP, physical, 1280, 720}, type-2 fill");
        Store32(base, args + 4, 7);
        Expect(ExitCodeOf([&] { PPCContext c = swap; imp("VdSwap")(c, base); }) == int(StopCode::Unimplemented), "VdSwap: format 7 stops (7)");
    }

    std::printf("runtime tests: %d failures\n", failures);
    return failures != 0;
}
