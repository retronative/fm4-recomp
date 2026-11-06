// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
#include "imports.h"
#include <cstring>
#include <vector>

namespace runtime {
namespace {

std::vector<ImportInfo>& Registry()
{
    static std::vector<ImportInfo> registry;  // filled during static initialisation
    return registry;
}

}  // namespace

bool RegisterImport(const char* name, const char* provenance, void (*function)(PPCContext&, uint8_t*))
{
    Registry().push_back({name, provenance, function});
    return true;
}

void (*FindImport(const char* name))(PPCContext&, uint8_t*)
{
    for (const auto& info : Registry())
        if (std::strcmp(info.name, name) == 0)
            return info.function;
    return nullptr;
}

size_t ImplementedImportCount() { return Registry().size(); }
const ImportInfo& ImplementedImport(size_t index) { return Registry()[index]; }

}  // namespace runtime
