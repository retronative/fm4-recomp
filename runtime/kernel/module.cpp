// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/module.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: XboxKrnlVersion written inline
// (2.0.17559 as a compatibility policy); console_config.h not ported.
#include "module.h"
#include "arena.h"
#include "clock.h"
#include "critical_section.h"
#include "imports.h"
#include <cstring>
#include <strings.h>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace runtime {
namespace {

constexpr uint32_t kXexMagic = 0x58455832;  // "XEX2"
constexpr uint32_t kHeaderCount = 0x14, kSecurityOffset = 0x10, kOptHeaders = 0x18;
constexpr uint32_t kChecksumTimestamp = 0x00018002;  // {checksum, time-date stamp}
Module executable{};
uint32_t xamLdr = 0, kernelLdr = 0;
std::map<std::string, uint32_t> dataImports;

const DataExport kDataExports[] = {
    // The kernel variable holds the executable's module handle: its loader entry
    // (Xenia KernelState::SetExecutableModule).
    {"XexExecutableModuleHandle", 4, "REFERENCE_DERIVED: Xenia kernel_state.cc SetExecutableModule",
     [](uint8_t* base, uint32_t block, const Module& module) { Store32(base, block, module.ldr); }},
    // 16 bytes: flags (bit 5, 0x20, set by the kernel once storage is initialised, per Xenia's
    // kernel disassembly note), CPU count 6, the rest zero.
    {"XboxHardwareInfo", 16, "REFERENCE_DERIVED: Xenia xboxkrnl_module.cc XboxkrnlModule (flags 0x20, 6 CPUs)",
     [](uint8_t* base, uint32_t block, const Module&) {
         Store32(base, block, 0x20);
         base[block + 4] = 6;
     }},
    // Interrupt time, system time and tick count, refreshed every millisecond (Xenia
    // kernel_state.cc CreateKeTimestampBundle / UpdateKeTimestampBundle).
    // A pointer that is set while a remote debugger is attached: none is (a retail console), so
    // null (Xenia xboxkrnl_module.cc, kernel_debug_monitor off).
    {"KeDebugMonitorData", 4, "REFERENCE_DERIVED: Xenia xboxkrnl_module.cc (no debugger: 0)",
     [](uint8_t* base, uint32_t block, const Module&) { Store32(base, block, 0); }},
    // FM4 Recomp: the kernel version, 2.0.17559 as an explicit compatibility
    // policy, not a claim about the original console. Layout: Xenia xboxkrnl_module.cc:205-216 (u16 major,
    // u16 minor, u16 build, u8 approval type 0x80, u8 QFE; big-endian); the values as ForzaRecomp's
    // console_config.h kRetail17559.
    {"XboxKrnlVersion", 8, "COMPATIBILITY POLICY 2.0.17559; layout REFERENCE_DERIVED from Xenia xboxkrnl_module.cc:205-216",
     [](uint8_t* base, uint32_t block, const Module&) {
         Store16(base, block, 2);
         Store16(base, block + 2, 0);
         Store16(base, block + 4, 17559);
         base[block + 6] = 0x80;
         base[block + 7] = 0;
     }},
    // Likewise for a certificate monitor: none (Xenia: "always set to zero").
    {"KeCertMonitorData", 4, "REFERENCE_DERIVED: Xenia xboxkrnl_module.cc (no certificate monitor: 0)",
     [](uint8_t* base, uint32_t block, const Module&) { Store32(base, block, 0); }},
    // The graphics globals (Xenia xboxkrnl_video.cc RegisterVideoExports): the D3D device and XAM
    // device pointers start null (the title sets them), the GPU clock is 500 MHz, and the HSIO
    // calibration lock is a critical section with spin count 10000.
    {"VdGlobalDevice", 4, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc (0)", [](uint8_t* base, uint32_t block, const Module&) { Store32(base, block, 0); }},
    {"VdGlobalXamDevice", 4, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc (0)", [](uint8_t* base, uint32_t block, const Module&) { Store32(base, block, 0); }},
    {"VdGpuClockInMHz", 4, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc (500)", [](uint8_t* base, uint32_t block, const Module&) { Store32(base, block, 500); }},
    {"VdHSIOCalibrationLock", 28, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc (critical section, spin count 10000)",
     [](uint8_t* base, uint32_t block, const Module&) { InitializeCriticalSection(base, block, 10000); }},
    {"KeTimeStampBundle", kTimeStampBundleSize, "REFERENCE_DERIVED: Xenia kernel_state.cc X_TIME_STAMP_BUNDLE (1 ms updates)",
     [](uint8_t* base, uint32_t block, const Module&) { StartTimeStampBundle(base, block); }},
};

}  // namespace

uint32_t XexHeaderField(const uint8_t* base, uint32_t header, uint32_t key)
{
    const uint32_t count = Load32(base, header + kHeaderCount);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t entry = header + kOptHeaders + 8 * i;
        if (Load32(base, entry) != key)
            continue;
        switch (key & 0xFF)
        {
        case 0x00: return Load32(base, entry + 4);  // the value itself
        case 0x01: return entry + 4;                // the address of the value in the header
        default: return header + Load32(base, entry + 4);  // the data at an offset in the header
        }
    }
    return 0;
}

Module CreateModule(uint8_t* base, const std::vector<uint8_t>& header, uint32_t imageBase, uint32_t entryPoint)
{
    Module m{};
    m.header = ArenaAlloc(base, uint32_t(header.size()));
    std::memcpy(base + m.header, header.data(), header.size());
    if (header.size() < kOptHeaders || Load32(base, m.header) != kXexMagic ||
        kOptHeaders + 8ull * Load32(base, m.header + kHeaderCount) > header.size())
        Stop(StopCode::Setup, "the XEX header is malformed");

    m.ldr = ArenaAlloc(base, kLdrEntrySize);
    const uint32_t security = m.header + Load32(base, m.header + kSecurityOffset);
    Store32(base, m.ldr + ldr::kDllBase, 0);  // GetProcAddress reads it (Xenia leaves 0)
    Store32(base, m.ldr + ldr::kXexHeaderBase, m.header);
    Store32(base, m.ldr + ldr::kFullImageSize, Load32(base, security + 4));  // security info image size
    Store32(base, m.ldr + ldr::kImageBase, imageBase);
    Store32(base, m.ldr + ldr::kEntryPoint, entryPoint);
    if (const uint32_t stamp = XexHeaderField(base, m.header, kChecksumTimestamp))
        Store32(base, m.ldr + ldr::kTimeDateStamp, Load32(base, stamp + 4));
    return m;
}

void SetExecutableModule(const Module& module) { executable = module; }
const Module& ExecutableModule() { return executable; }

uint32_t SystemModule(uint8_t* base, const char* name)
{
    uint32_t* ldr = !strcasecmp(name, "xam.xex") ? &xamLdr : !strcasecmp(name, "xboxkrnl.exe") ? &kernelLdr : nullptr;
    if (ldr && *ldr == 0)
        *ldr = ArenaAlloc(base, kLdrEntrySize);
    return ldr ? *ldr : 0;
}

const char* SystemModuleName(uint32_t ldr)
{
    return ldr && ldr == xamLdr ? "xam.xex" : ldr && ldr == kernelLdr ? "xboxkrnl.exe" : nullptr;
}

// Written when a module is mapped, also while guest threads read it (a module loaded at run time).
std::mutex dataImportsLock;
void RecordDataImport(const char* name, uint32_t value)
{
    std::lock_guard guard(dataImportsLock);
    dataImports[name] = value;
}
uint32_t DataImportValue(const char* name)
{
    std::lock_guard guard(dataImportsLock);
    const auto it = dataImports.find(name);
    return it == dataImports.end() ? 0 : it->second;
}

const DataExport* FindDataExport(const char* name)
{
    for (const auto& e : kDataExports)
        if (std::string_view(e.name) == name)
            return &e;
    return nullptr;
}

}  // namespace runtime
