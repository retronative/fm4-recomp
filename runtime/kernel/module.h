// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/module.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// The executable as the kernel sees it: a guest copy of the XEX header and its loader entry
// (X_LDR_DATA_TABLE_ENTRY), and the kernel's data exports. REFERENCE_DERIVED from Xenia
// (xenia-canary 67f16c4e315f: kernel/xmodule.{h,cc}, kernel/user_module.cc LoadContinue,
// kernel/kernel_state.cc SetExecutableModule), re-derived here.
#pragma once
#include <cstdint>
#include <vector>

namespace runtime {

constexpr uint32_t kLdrEntrySize = 0x64;
namespace ldr { constexpr uint32_t kDllBase = 0x18, kImageBase = 0x1C, kFullImageSize = 0x38, kEntryPoint = 0x3C,
                                   kTimeDateStamp = 0x50, kXexHeaderBase = 0x58; }

struct Module { uint32_t ldr, header; };

// Copies the header into the kernel arena and fills a loader entry from it.
Module CreateModule(uint8_t* base, const std::vector<uint8_t>& header, uint32_t imageBase, uint32_t entryPoint);
// Makes a module the executable the imports see (XexExecutableModuleHandle, a null module name).
void SetExecutableModule(const Module& module);

// An optional header of the header copy at `header`, by its key's low byte: 0x00 its inline
// value, 0x01 the guest address of that value, otherwise the guest address of its data; 0 if
// absent (Xenia user_module.cc UserModule::GetOptHeader, as RtlImageXexHeaderField uses it).
uint32_t XexHeaderField(const uint8_t* base, uint32_t header, uint32_t key);

// The executable module (SetExecutableModule).
const Module& ExecutableModule();

// A system module's loader entry ("xam.xex", "xboxkrnl.exe"; case-insensitive), created on first
// use like Xenia's KernelModule (an entry with no image of its own); 0 for any other name.
uint32_t SystemModule(uint8_t* base, const char* name);
// The system module a loader entry belongs to, or null.
const char* SystemModuleName(uint32_t ldr);

// A kernel data export: its block size, how the kernel fills it, and the source.
struct DataExport { const char* name; uint32_t size; const char* provenance; void (*init)(uint8_t* base, uint32_t block, const Module&); };
const DataExport* FindDataExport(const char* name);

// The value the loader put in a data import's slot (its block or guard address), by name; 0 if the
// executable does not import it. Object-type exports are identified by this address (Xenia).
void RecordDataImport(const char* name, uint32_t value);
uint32_t DataImportValue(const char* name);

}  // namespace runtime
