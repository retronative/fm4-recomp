// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See diag_alpha.h. Diagnostic only, behind FM4_DIAG_ALPHA_QUANT.
#include "diag_alpha.h"
#include "stops.h"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace runtime::video {

namespace {
bool parsed = false, announced = false, anyHash = true;
uint32_t levels = 0;
uint64_t onlyHash = 0;

void Parse()
{
    parsed = true;
    const char* env = std::getenv("FM4_DIAG_ALPHA_QUANT");
    if (env == nullptr || *env == '\0')
        return;
    const std::string value = env;
    const size_t colon = value.find(':');
    const std::string count = value.substr(0, colon);
    char* end = nullptr;
    const unsigned long n = std::strtoul(count.c_str(), &end, 10);
    bool ok = !count.empty() && end != nullptr && *end == '\0' && n >= 1 && n <= 65535;
    if (ok && colon != std::string::npos)
    {
        const std::string hash = value.substr(colon + 1);
        char* hashEnd = nullptr;
        onlyHash = std::strtoull(hash.c_str(), &hashEnd, 16);
        ok = hash.size() == 16 && hashEnd != nullptr && *hashEnd == '\0';
        anyHash = false;
    }
    if (!ok)
        Stop(StopCode::Setup, "FM4_DIAG_ALPHA_QUANT=%s is not <levels 1-65535>[:<16-hex-digit pixel shader hash>]", env);
    levels = uint32_t(n);
}
}  // namespace

bool DiagAlphaQuantize(uint64_t psHash, ShaderVariantDefines& psDefines)
{
    if (!parsed)
        Parse();
    if (levels == 0 || (!anyHash && psHash != onlyHash))
        return false;
    bool alphaTested = false;
    for (const std::string& line : psDefines.lines)
        alphaTested = alphaTested || line.rfind("XE_ALPHA_TEST ", 0) == 0;
    if (!alphaTested)
        return false;
    psDefines.Add("XE_ALPHA_TEST_QUANT " + std::to_string(levels) + ".0");
    if (!announced)
    {
        announced = true;
        std::fprintf(stderr, "[diag] FM4_DIAG_ALPHA_QUANT: alpha quantized to %u levels before the alpha test (first at ps_%016llx%s); a diagnostic "
                             "run, not a rendering mode\n",
                     levels, (unsigned long long)psHash, anyHash ? ", every alpha-tested shader" : " only");
    }
    return true;
}

}  // namespace runtime::video
