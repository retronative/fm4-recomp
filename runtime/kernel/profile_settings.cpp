// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The signed-in profile's settings (user.h WriteProfileSettings). Xenia's default profile (xenia-canary 67f16c4e315f
// kernel/xam/user_profile.cc:35-95) with a fresh offline gamercard (a compatibility policy): gamerscore (cred) 0, titles played
// 0, no gamer-picture key; nothing invented. The record layout is Xenia's (xam_user.cc:283-320, user_profile.h:25-66).
#include "imports.h"
#include "user.h"
#include <cstring>

namespace runtime::user {
using runtime::Load32, runtime::Store32, runtime::Store16;
namespace {

// Xenia's default profile settings (user_profile.cc:35-95, 67f16c4), in its order. type: UserProfile::Setting::Type
// (user_profile.h): 1 INT32, 4 WSTRING, 5 FLOAT (value = the IEEE 754 bits), 6 BINARY. The three title-specific binary
// settings are not set (BinarySetting(id): is_set false); the runtime has no XamUserWriteProfileSettings and stores
// none, so they stay unset (Xenia would load a file written by an earlier run: NOTES.md).
struct DefaultSetting { uint32_t id; uint8_t type; uint32_t value; const char* text; bool set; };
constexpr DefaultSetting kSettings[] = {
    {0x10040002, 1, 0, nullptr, true},           // XPROFILE_GAMER_YAXIS_INVERSION
    {0x10040003, 1, 3, nullptr, true},           // XPROFILE_OPTION_CONTROLLER_VIBRATION
    {0x10040004, 1, 0, nullptr, true},           // XPROFILE_GAMERCARD_ZONE
    {0x10040005, 1, 0, nullptr, true},           // XPROFILE_GAMERCARD_REGION
    {0x10040006, 1, 0, nullptr, true},           // XPROFILE_GAMERCARD_CRED: 0 (owner: no invented gamerscore; Xenia 0xFA)
    {0x5004000B, 5, 0, nullptr, true},           // XPROFILE_GAMERCARD_REP, 0.0f
    {0x1004000C, 1, 0, nullptr, true},           // XPROFILE_OPTION_VOICE_MUTED
    {0x1004000D, 1, 0, nullptr, true},           // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
    {0x1004000E, 1, 0x64, nullptr, true},        // XPROFILE_OPTION_VOICE_VOLUME
    {0x402C0011, 4, 0, "", true},                // XPROFILE_GAMERCARD_MOTTO
    {0x10040012, 1, 0, nullptr, true},           // XPROFILE_GAMERCARD_TITLES_PLAYED: 0 (compatibility policy; Xenia 1)
    {0x10040013, 1, 0, nullptr, true},           // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
    {0x10040015, 1, 0, nullptr, true},           // XPROFILE_GAMER_DIFFICULTY
    {0x10040018, 1, 0, nullptr, true},           // XPROFILE_GAMER_CONTROL_SENSITIVITY
    {0x1004001D, 1, 0xFFFF0000u, nullptr, true}, // preferred color 1
    {0x1004001E, 1, 0xFF00FF00u, nullptr, true}, // preferred color 2
    {0x10040022, 1, 1, nullptr, true},           // XPROFILE_GAMER_ACTION_AUTO_AIM
    {0x10040023, 1, 0, nullptr, true},           // XPROFILE_GAMER_ACTION_AUTO_CENTER
    {0x10040024, 1, 0, nullptr, true},           // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
    {0x10040026, 1, 0, nullptr, true},           // XPROFILE_GAMER_RACE_TRANSMISSION
    {0x10040027, 1, 0, nullptr, true},           // XPROFILE_GAMER_RACE_CAMERA_LOCATION
    {0x10040028, 1, 0, nullptr, true},           // XPROFILE_GAMER_RACE_BRAKE_CONTROL
    {0x10040029, 1, 0, nullptr, true},           // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
    {0x10040038, 1, 0, nullptr, true},           // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
    {0x10040039, 1, 0, nullptr, true},           // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
    {0x4064000F, 4, 0, "", false},               // XPROFILE_GAMERCARD_PICTURE_KEY: unset (compatibility policy; Xenia "gamercard_picture_key")
    {0x63E83FFF, 6, 0, nullptr, false},          // XPROFILE_TITLE_SPECIFIC1
    {0x63E83FFE, 6, 0, nullptr, false},          // XPROFILE_TITLE_SPECIFIC2
    {0x63E83FFD, 6, 0, nullptr, false},          // XPROFILE_TITLE_SPECIFIC3
};

const DefaultSetting* Find(uint32_t id)
{
    for (const DefaultSetting& s : kSettings)
        if (s.id == id)
            return &s;
    return nullptr;
}

}  // namespace

void WriteProfileSettings(uint8_t* base, uint32_t buffer, uint32_t count, uint32_t ids, bool byXuid, uint32_t userIndex)
{
    // Every id is looked up before anything is written (Xenia xam_user.cc:260-281 checks them all first). Xenia answers
    // an id it lacks with ERROR_INVALID_PARAMETER ("TODO: don't fail?"); the model stops instead, naming the id.
    for (uint32_t i = 0; i < count; i++)
        if (const uint32_t id = Load32(base, ids + 4 * i); Find(id) == nullptr)
            Stop(StopCode::Unimplemented, "XamUserReadProfileSettings: setting %08X is not in the default profile", id);
    // X_USER_READ_PROFILE_SETTINGS (xam_user.cc:152-156): count, pointer to the first record (just after the header).
    // X_USER_PROFILE_SETTING (user_profile.h:52-66), 40 bytes: from +0, +4, xuid or user index +8 (a union: Xenia stores
    // xuid -1 then the 32-bit index over its first, big-endian high, half), setting id +0x10, +0x14, data +0x18.
    // X_USER_PROFILE_SETTING_DATA (user_profile.h:25-50): type byte +0x18, value +0x20 (int32 / float bits; a string:
    // size +0x20, pointer +0x24). String data follows all records (the stream starts at 8 + 40 * count, xam_user.cc:
    // 289-291), UTF-16 big-endian with its NUL; an empty string has size 0 and pointer 0 (user_profile.h:153-175).
    Store32(base, buffer, count);
    Store32(base, buffer + 4, buffer + 8);
    uint32_t stream = buffer + 8 + 40 * count;
    for (uint32_t n = 0; n < count; n++)
    {
        const uint32_t record = buffer + 8 + 40 * n, id = Load32(base, ids + 4 * n);
        const DefaultSetting& s = *Find(id);
        std::memset(base + record, 0, 40);
        const bool titleSpecific = (id & 0x3F00) == 0x3F00;  // is_title_specific(), user_profile.h:121
        Store32(base, record, s.set && titleSpecific ? 2 : 1);  // from: xam_user.cc:309-314
        if (byXuid)
            Store32(base, record + 8, uint32_t(kXuid >> 32)), Store32(base, record + 0xC, uint32_t(kXuid));
        else
            Store32(base, record + 8, userIndex), Store32(base, record + 0xC, 0xFFFFFFFF);
        Store32(base, record + 0x10, id);
        base[record + 0x18] = s.type;
        if (!s.set)
            continue;
        if (s.type == 4)
        {
            const uint32_t chars = uint32_t(std::strlen(s.text));
            if (chars == 0)
                continue;
            const uint32_t size = 2 * (chars + 1);
            Store32(base, record + 0x20, size);
            Store32(base, record + 0x24, stream);
            for (uint32_t i = 0; i <= chars; i++)
                Store16(base, stream + 2 * i, uint8_t(s.text[i]));  // ASCII text: UTF-16 code unit = byte
            stream += size;
        }
        else
            Store32(base, record + 0x20, s.value);
    }
}

}  // namespace runtime::user
