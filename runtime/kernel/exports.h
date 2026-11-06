// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// System-module exports a title asks for by ordinal (XexGetProcedureAddress). Each known export gets
// one guest address (a trampoline: a kernel arena word with no guest code behind it); an indirect call
// to that address runs the runtime's implementation of the export (GUEST_IMPORT, by name) or stops
// like an unimplemented import (3), naming it. Xenia does the same with generated trampolines
// (xenia-canary 67f16c4e315f kernel/kernel_module.cc:87 KernelModule::GetProcAddressByOrdinal).
#pragma once
#include <cstdint>

struct PPCContext;

namespace runtime {

// An export the runtime knows by ordinal: only those a run has asked for, each cited.
struct SystemExport { const char* module; uint32_t ordinal; const char* name; const char* provenance; };

// The export `ordinal` of system module `module` ("xam.xex", "xboxkrnl.exe"), or null if unknown.
const SystemExport* FindSystemExport(const char* module, uint32_t ordinal);
// Its trampoline address (allocated on first use, the same afterwards).
uint32_t ExportTrampoline(uint8_t* base, const SystemExport& e);
// The export whose trampoline is `address`, or null.
const SystemExport* TrampolineExport(uint32_t address);
// Calls an export through its trampoline: the runtime's implementation, else a strict stop (3).
void CallExport(PPCContext& ctx, uint8_t* base, const SystemExport& e);

}  // namespace runtime
