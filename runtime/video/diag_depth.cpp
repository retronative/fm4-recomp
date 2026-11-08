// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See diag_depth.h. Diagnostic only, behind FM4_DIAG_PS_NO_DEPTH.
#include "diag_depth.h"
#include "stops.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace runtime::video {

namespace {
bool parsed = false, announced = false;
uint64_t hashes[8] = {};
uint32_t count = 0;
}  // namespace

bool DiagNoDepth(uint64_t psHash, PipelineKey& key)
{
    if (!parsed)
    {
        parsed = true;
        if (const char* env = std::getenv("FM4_DIAG_PS_NO_DEPTH"); env != nullptr && *env != '\0')
        {
            const std::string list = env;
            for (size_t at = 0; at <= list.size();)
            {
                const size_t comma = std::min(list.find(',', at), list.size());
                const std::string item = list.substr(at, comma - at);
                char* end = nullptr;
                const uint64_t h = std::strtoull(item.c_str(), &end, 16);
                if (item.size() != 16 || end == nullptr || *end != '\0' || count == 8)
                    Stop(StopCode::Setup, "FM4_DIAG_PS_NO_DEPTH=%s is not a list of at most 8 16-hex-digit pixel shader hashes", env);
                hashes[count++] = h;
                at = comma + 1;
            }
        }
    }
    if (count == 0 || std::find(hashes, hashes + count, psHash) == hashes + count || !key.depthUsed)
        return false;
    key.depthWrite = 0;
    key.depthFunc = 7;  // always: pipeline_cache.cpp enables the depth test only for a write or a function other than always
    if (!announced)
    {
        announced = true;
        std::fprintf(stderr, "[diag] FM4_DIAG_PS_NO_DEPTH: depth test and write OFF for ps_%016llx (and %u other hash(es)); a diagnostic run, "
                             "not a rendering mode\n", (unsigned long long)psHash, count - 1);
    }
    return true;
}

}  // namespace runtime::video
