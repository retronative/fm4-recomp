// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Title modules (loader.h).
#include "loader.h"
#include "guest_memory.h"
#include "stops.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unistd.h>

namespace runtime {
namespace {

struct CodeRange { uint32_t codeBase, codeSize, table, spanStart, spanEnd; };
std::array<CodeRange, kMaxCodeRanges> ranges;
std::atomic<size_t> rangeCount{0};
std::mutex loaderLock;  // mapping and registration (lookups read published ranges without it)

std::array<LoadedModule, kMaxCodeRanges> loaded;
std::atomic<size_t> loadedCount{0};

const ModuleImage* images = nullptr;
size_t imageCount = 0;
std::string modulesDirectory;

constexpr uint32_t kModulesBase = 1u << 31, kModulesEnd = 5u << 29;  // 2 GB .. the physical views (A0000000)

char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
bool EqualCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (Lower(a[i]) != Lower(b[i]))
            return false;
    return true;
}

}  // namespace

void RegisterCode(uint32_t codeBase, uint32_t codeSize, uint32_t imageBase, uint32_t imageSize)
{
    std::lock_guard guard(loaderLock);
    const size_t n = rangeCount.load();
    if (n == kMaxCodeRanges)
        Stop(StopCode::Setup, "more than %zu code ranges", kMaxCodeRanges);
    const uint32_t table = imageBase + imageSize;
    ranges[n] = CodeRange{codeBase, codeSize, table, imageBase, uint32_t(table + uint64_t(codeSize) * 2)};
    rangeCount.store(n + 1);  // published after the entry is written
}

PPCFunc* FindFunction(uint8_t* base, uint32_t address)
{
    const size_t n = rangeCount.load();
    for (size_t i = 0; i < n; i++)
        if (IsCodeAddress(address, ranges[i].codeBase, ranges[i].codeSize))
            return *reinterpret_cast<PPCFunc**>(base + ranges[i].table + uint64_t(address - ranges[i].codeBase) * 2);
    return nullptr;
}

uint32_t ModuleSpanEnd(const ModuleImage& image) { return uint32_t(uint64_t(image.base) + image.size + uint64_t(image.codeSize) * 2); }

bool ModuleFits(const ModuleImage& image)
{
    const uint64_t end = uint64_t(image.base) + image.size + uint64_t(image.codeSize) * 2;
    return image.base >= kModulesBase && image.base % 0x10000 == 0 && end <= kModulesEnd && image.codeBase >= image.base &&
           uint64_t(image.codeBase) + image.codeSize <= uint64_t(image.base) + image.size;
}

bool ModulesOverlap(uint32_t base, uint32_t end)
{
    const size_t n = rangeCount.load();
    for (size_t i = 0; i < n; i++)
        if (base < ranges[i].spanEnd && ranges[i].spanStart < end)
            return true;
    return false;
}

bool NamesModule(const char* name, const char* fileName)
{
    // Xenia XModule::Matches (xmodule.cc:44): the file name, the name without its extension, or the path.
    // The directory of a path is not compared (INFERRED: every title module is on the one disc device).
    std::string_view requested(name), file(fileName);
    const size_t cut = requested.find_last_of("\\/:");
    if (cut != std::string_view::npos)
        requested = requested.substr(cut + 1);
    const size_t dot = file.rfind('.');
    return EqualCase(requested, file) || (dot != std::string_view::npos && EqualCase(requested, file.substr(0, dot)));
}

void SetModuleImages(const ModuleImage* list, size_t count, const char* directory)
{
    images = list, imageCount = count, modulesDirectory = directory;
}

const ModuleImage* FindModuleImage(const char* name)
{
    for (size_t i = 0; i < imageCount; i++)
        if (NamesModule(name, images[i].fileName))
            return &images[i];
    return nullptr;
}

std::string ModuleSurveyImage(const ModuleImage& image) { return modulesDirectory + "/" + image.name + "/survey/image.bin"; }

const LoadedModule& MapModule(uint8_t* base, const ModuleImage& image, const std::vector<uint8_t>& bytes,
                              const std::vector<uint8_t>& header)
{
    std::lock_guard guard(loaderLock);
    const uint32_t spanEnd = ModuleSpanEnd(image);
    if (!ModuleFits(image) || bytes.size() != image.size)
        Stop(StopCode::Setup, "module %s (%08X, %X bytes) does not fit 80000000-A0000000 or its image is %zu bytes",
             image.name, image.base, image.size, bytes.size());
    if (ModulesOverlap(image.base, spanEnd))
        Stop(StopCode::Setup, "module %s (%08X-%08X) overlaps a mapped module", image.name, image.base, spanEnd);
    const size_t page = size_t(getpagesize());
    CommitGuestMemory(base, image.base, image.size);
    std::memcpy(base + image.base, bytes.data(), bytes.size());
    const uint32_t table = image.base + image.size;
    CommitGuestMemory(base, table, uint64_t(image.codeSize) * 2);
    for (const PPCFuncMapping* m = image.mappings; m->host != nullptr; m++)
    {
        if (!IsCodeAddress(uint32_t(m->guest), image.codeBase, image.codeSize))
            Stop(StopCode::Setup, "module %s maps %08zX outside its code", image.name, m->guest);
        *reinterpret_cast<PPCFunc**>(base + table + uint64_t(m->guest - image.codeBase) * 2) = m->host;
    }
    for (size_t i = 0; i < image.importCount; i++)
    {
        const ModuleImport& import = image.imports[i];
        uint32_t value = import.thunk;
        if (import.variable)
        {
            const char* name = std::strncmp(import.name, "__imp__", 7) == 0 ? import.name + 7 : import.name;
            value = DataImportValue(name);  // the executable's block (or guard) for that name
            if (value == 0)
                Stop(StopCode::Setup, "module %s imports data %s, which the executable does not", image.name, name);
        }
        StoreBigEndian32(base, import.address, value);
    }
    for (uint64_t at = image.base; at < table; at += page)
        if (!PageWritable(uint32_t(at), page, image.sections, image.sectionCount))
            ProtectGuestMemoryReadOnly(base, at, page);
    ProtectGuestMemoryReadOnly(base, table, ((uint64_t(table) + uint64_t(image.codeSize) * 2 + page - 1) & ~uint64_t(page - 1)) - table);
    const Module module = CreateModule(base, header, image.base, image.entry);

    const size_t n = rangeCount.load();
    if (n == kMaxCodeRanges)
        Stop(StopCode::Setup, "more than %zu code ranges", kMaxCodeRanges);
    ranges[n] = CodeRange{image.codeBase, image.codeSize, table, image.base, spanEnd};
    rangeCount.store(n + 1);
    const size_t m = loadedCount.load();
    loaded[m] = LoadedModule{&image, module};
    loadedCount.store(m + 1);
    return loaded[m];
}

void RefreshModule(uint8_t* base, const LoadedModule& module, const std::vector<uint8_t>& bytes)
{
    std::lock_guard guard(loaderLock);
    const ModuleImage& image = *module.image;
    if (bytes.size() != image.size)
        Stop(StopCode::Setup, "module %s: the image to reload is %zu bytes, not %X", image.name, bytes.size(), image.size);
    const size_t page = size_t(getpagesize());
    for (uint64_t at = image.base; at < uint64_t(image.base) + image.size; at += page)
        if (PageWritable(uint32_t(at), page, image.sections, image.sectionCount))
        {
            const uint64_t n = std::min<uint64_t>(page, uint64_t(image.base) + image.size - at);
            std::memcpy(base + at, bytes.data() + (at - image.base), n);
        }
    for (size_t i = 0; i < image.importCount; i++)
    {
        const ModuleImport& import = image.imports[i];
        uint32_t value = import.thunk;
        if (import.variable)
        {
            const char* name = std::strncmp(import.name, "__imp__", 7) == 0 ? import.name + 7 : import.name;
            value = DataImportValue(name);
        }
        StoreBigEndian32(base, import.address, value);
    }
}

const LoadedModule* LoadedModuleByName(const char* name)
{
    const size_t n = loadedCount.load();
    for (size_t i = 0; i < n; i++)
        if (NamesModule(name, loaded[i].image->fileName))
            return &loaded[i];
    return nullptr;
}

const LoadedModule* LoadedModuleByLdr(uint32_t ldr)
{
    const size_t n = loadedCount.load();
    for (size_t i = 0; i < n; i++)
        if (loaded[i].module.ldr == ldr)
            return &loaded[i];
    return nullptr;
}

}  // namespace runtime
