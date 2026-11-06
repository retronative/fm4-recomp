// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// ExGetXConfigSetting: the console's configuration settings. REFERENCE_DERIVED from Xenia
// (xenia-canary 67f16c4e315f): src/xenia/kernel/xboxkrnl/xboxkrnl_xconfig.cc:44
// xeExGetXConfigSetting, its values and checks; status codes from src/xenia/xbox.h:57-73.
//
// Compatibility policy (not a claim about any console): the settings Xenia models, with Xenia's
// default values (a USA console, English, time-zone values 0). A category or setting Xenia does not model stops by name (7) instead of
// returning Xenia's INVALID_PARAMETER_1/2: a real console has such settings, so a failure there would
// be fabricated.
#include "imports.h"
#include <cstring>

namespace {

struct Setting { uint16_t category, setting, size; uint32_t value; const char* name; };

// Xenia xboxkrnl_xconfig.cc:53-105. A 4-byte value is stored big-endian; the 1-byte country is its
// low byte (value[0] = user_country).
constexpr Setting kSettings[] = {
    {2, 0x02, 4, 0x00001000, "XCONFIG_SECURED_AV_REGION (USA/Canada)"},
    {3, 0x01, 4, 0, "XCONFIG_USER_TIME_ZONE_BIAS"},
    {3, 0x02, 4, 0, "XCONFIG_USER_TIME_ZONE_STD_NAME"},
    {3, 0x03, 4, 0, "XCONFIG_USER_TIME_ZONE_DLT_NAME"},
    {3, 0x04, 4, 0, "XCONFIG_USER_TIME_ZONE_STD_DATE"},
    {3, 0x05, 4, 0, "XCONFIG_USER_TIME_ZONE_DLT_DATE"},
    {3, 0x06, 4, 0, "XCONFIG_USER_TIME_ZONE_STD_BIAS"},
    {3, 0x07, 4, 0, "XCONFIG_USER_TIME_ZONE_DLT_BIAS"},
    {3, 0x09, 4, 1, "XCONFIG_USER_LANGUAGE (1 = English, Xenia's user_language default)"},
    {3, 0x0A, 4, 0x00040000, "XCONFIG_USER_VIDEO_FLAGS"},
    {3, 0x0C, 4, 0, "XCONFIG_USER_RETAIL_FLAGS"},
    {3, 0x0E, 1, 103, "XCONFIG_USER_COUNTRY (103 = US, Xenia's user_country default)"},
};

constexpr uint32_t kBufferTooSmall = 0xC0000023, kInvalidParameter3 = 0xC00000F1;

}  // namespace

GUEST_IMPORT(ExGetXConfigSetting, "REFERENCE_DERIVED: Xenia xboxkrnl_xconfig.cc:44 xeExGetXConfigSetting (values, checks)")
{
    // (category, setting, buffer*, buffer size, required size*), the 16-bit arguments in the low halves.
    const uint16_t category = uint16_t(ctx.r3.u32), id = uint16_t(ctx.r4.u32), bufferSize = uint16_t(ctx.r6.u32);
    const uint32_t buffer = ctx.r5.u32, requiredOut = ctx.r7.u32;
    const Setting* found = nullptr;
    for (const Setting& s : kSettings)
        if (s.category == category && s.setting == id)
            found = &s;
    if (found == nullptr)
        Stop(StopCode::Unimplemented, "ExGetXConfigSetting(category %u, setting %u): not a setting the runtime models", category, id);

    uint32_t status = 0;
    if (buffer != 0)
    {
        if (bufferSize < found->size)
            status = kBufferTooSmall;
        else if (found->size == 4)
            runtime::Store32(base, buffer, found->value);
        else
            base[buffer] = uint8_t(found->value);
    }
    else if (bufferSize != 0)
        status = kInvalidParameter3;
    // The entry always writes the required size when the pointer is given: the setting's size on
    // success, 0 when a buffer check fails (the size is set only after them; xboxkrnl_xconfig.cc:107-135).
    if (requiredOut != 0)
    {
        const uint16_t required = status == kBufferTooSmall || status == kInvalidParameter3 ? 0 : found->size;
        base[requiredOut] = uint8_t(required >> 8), base[requiredOut + 1] = uint8_t(required);
    }
    ctx.r3.u64 = status;
}
