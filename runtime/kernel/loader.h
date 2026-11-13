// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Title modules: the executable and the modules it loads (XexLoadImage). Tested in
// tests/runtime/test_runtime.cpp.
//
// Each module's function table follows its own image, as XenonRecomp's PPC_LOOKUP_FUNC places the
// executable's: base + image end + (address - code base) x 2 holds the host function of a code word.
// Generated code calls other functions of its module by symbol; an indirect call or a thread start
// finds its target here, by the code range that holds it.
#pragma once
#include "layout.h"
#include "module.h"
#include "stops.h"  // ppc_config.h, then ppc_context.h
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace runtime {

// An import record of a module: a function slot (holds its thunk's address) or a data import (holds
// the kernel block the executable's import of the same name got).
struct ModuleImport { uint32_t address; bool variable; uint32_t thunk; const char* name; };

// A module default.xex loads, as translated (generated: fm4_modules.h).
struct ModuleImage
{
    const char* name;      // the library's name (game/modules/<name>)
    const char* fileName;  // its file on the disc
    uint32_t base, size, entry, codeBase, codeSize;
    uint32_t imageCrc32, headerSize, headerCrc32, fileSize, fileCrc32;
    uint32_t exportTable;  // the XEX export table's guest address (security info), 0 if none
    const Section* sections; size_t sectionCount;
    const ModuleImport* imports; size_t importCount;
    const PPCFuncMapping* mappings;  // {0, nullptr}-terminated
};

constexpr size_t kMaxCodeRanges = 8;

// Registers a module's code range; its function table follows its image (imageBase + imageSize). The
// executable's first (main.cpp), then each mapped module's. Stops (9) past kMaxCodeRanges.
void RegisterCode(uint32_t codeBase, uint32_t codeSize, uint32_t imageBase, uint32_t imageSize);
// The host function of a translated function's guest address, in any registered range; null if none.
PPCFunc* FindFunction(uint8_t* base, uint32_t address);

// A module must lie in the 2 GB-2.5 GB range (below the physical views at A0000000), 64 KB aligned, its code
// and table inside that span, and overlap no registered module.
bool ModuleFits(const ModuleImage& image);
bool ModulesOverlap(uint32_t base, uint32_t end);  // [base, end) against every registered image and table
uint32_t ModuleSpanEnd(const ModuleImage& image);   // the end of its image and function table

struct LoadedModule { const ModuleImage* image; Module module; };

// Maps a module: commits and copies its image, fills its function table and import slots, makes
// code and the table read-only, creates its loader entry from `header`, and records it. Stops (9) if
// the module does not fit, overlaps, or imports data the executable did not.
const LoadedModule& MapModule(uint8_t* base, const ModuleImage& image, const std::vector<uint8_t>& bytes,
                              const std::vector<uint8_t>& header);
// The modules the runtime can load (generated fm4::kModules) and the directory of their surveys
// (game/modules: <name>/survey/image.bin).
void SetModuleImages(const ModuleImage* images, size_t count, const char* modulesDirectory);
const ModuleImage* FindModuleImage(const char* name);  // by NamesModule; null if none
std::string ModuleSurveyImage(const ModuleImage& image);  // <modules dir>/<name>/survey/image.bin
// A mapped module by its file name ("x.xex", "x", "game:\\x.xex"; any case), or by its loader entry.
const LoadedModule* LoadedModuleByName(const char* name);
const LoadedModule* LoadedModuleByLdr(uint32_t ldr);
// FM4 Recomp: a module XexUnloadImage unloaded and XexLoadImage loads again (Xenia maps it afresh,
// kernel_state.cc LoadUserModule): its writable pages are copied from `bytes` again (the read-only code is unchanged) and its
// import slots filled again, so its data is as at the first load. Stops (9) if `bytes` is not its image.
void RefreshModule(uint8_t* base, const LoadedModule& module, const std::vector<uint8_t>& bytes);
// Whether `name` names the module file `fileName` (the directory ignored; with or without the
// extension; any case): Xenia XModule::Matches.
bool NamesModule(const char* name, const char* fileName);

// The title-terminate notifications registered so far ({routine, priority}; ExRegisterTitleTerminateNotification).
std::vector<std::pair<uint32_t, uint32_t>> TitleTerminateNotifications();

}  // namespace runtime
