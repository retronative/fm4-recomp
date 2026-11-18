// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4_DIAG_TRIGGER (diag_trigger.h).
#include "diag_trigger.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace runtime::video::diag {

void TriggerAtRetire(uint64_t swap, const std::function<void(const std::string&)>& run)
{
    static const char* path = std::getenv("FM4_DIAG_TRIGGER");
    if (path == nullptr || *path == 0 || swap % 30 != 0)
        return;
    std::ifstream in(path);
    if (!in)
        return;
    std::string line;
    while (std::getline(in, line))
        if (!line.empty())
        {
            std::fprintf(stderr, "[diag] trigger at swap %llu: %s\n", (unsigned long long)swap, line.c_str());
            run(line);
        }
    in.close();
    std::rename(path, (std::string(path) + ".done").c_str());
}

}  // namespace runtime::video::diag
