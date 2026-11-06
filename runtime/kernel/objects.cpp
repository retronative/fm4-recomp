// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/objects.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
#include "objects.h"
#include "imports.h"
#include <map>
#include <memory>
#include <mutex>

namespace runtime {
namespace {

struct Entry { KernelObject object; std::string name; int handles; };

std::mutex lock;
std::map<uint32_t, std::shared_ptr<Entry>> handles;
std::map<std::string, std::weak_ptr<Entry>> names;
uint32_t nextHandle = 0xF8000004;

uint32_t NewHandle(const std::shared_ptr<Entry>& entry)
{
    const uint32_t handle = nextHandle;
    nextHandle += 4;
    handles[handle] = entry;
    entry->handles++;
    return handle;
}

}  // namespace

uint32_t CreateObject(ObjectType type, uint32_t header, const std::string& name)
{
    std::lock_guard guard(lock);
    auto entry = std::make_shared<Entry>(Entry{{type, header}, name, 0});
    if (!name.empty())
        names[name] = entry;
    return NewHandle(entry);
}

std::optional<KernelObject> LookupHandle(uint32_t handle)
{
    std::lock_guard guard(lock);
    const auto it = handles.find(handle);
    if (it == handles.end())
        return std::nullopt;
    return it->second->object;
}

std::optional<uint32_t> LookupName(const std::string& name, ObjectType& type)
{
    std::lock_guard guard(lock);
    const auto it = names.find(name);
    if (name.empty() || it == names.end())
        return std::nullopt;
    const auto entry = it->second.lock();
    if (!entry)
        return std::nullopt;
    type = entry->object.type;
    return NewHandle(entry);
}

std::optional<uint32_t> DuplicateHandle(uint32_t handle)
{
    std::lock_guard guard(lock);
    const auto it = handles.find(handle);
    if (it == handles.end())
        return std::nullopt;
    return NewHandle(it->second);
}

uint32_t HandleForObject(ObjectType type, uint32_t header)
{
    std::lock_guard guard(lock);
    for (const auto& [handle, entry] : handles)
        if (entry->object.header == header)
            return NewHandle(entry);
    return NewHandle(std::make_shared<Entry>(Entry{{type, header}, "", 0}));
}

bool IsObjectBody(uint32_t header)
{
    std::lock_guard guard(lock);
    for (const auto& [handle, entry] : handles)
        if (entry->object.header == header)
            return true;
    return false;
}

bool CloseHandle(uint32_t handle)
{
    std::lock_guard guard(lock);
    const auto it = handles.find(handle);
    if (it == handles.end())
        return false;
    const auto entry = it->second;
    handles.erase(it);
    if (--entry->handles == 0 && !entry->name.empty())
        names.erase(entry->name);  // the object's memory stays in the arena (no reuse yet)
    return true;
}

std::string ObjectName(const uint8_t* base, uint32_t attributes)
{
    if (attributes == 0)
        return {};
    const uint32_t string = Load32(base, attributes + 4);
    if (string == 0)
        return {};
    const uint16_t length = uint16_t(base[string] << 8 | base[string + 1]);
    const uint32_t buffer = Load32(base, string + 4);
    return std::string(reinterpret_cast<const char*>(base + buffer), length);
}

}  // namespace runtime
