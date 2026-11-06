// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The signed-in user (a compatibility policy: a local offline profile): user 0 is signed in locally (sign-in state 1, never Live) with Xenia's default profile
// (xenia-canary 67f16c4e315f kernel/xam/user_profile.cc:22-30: XUID B13EBABEBABEBABE + index, gamertag "User");
// users 1-3 are not signed in. Xenia keys signed-in users by index 0-3 (kernel_state.h:122-124).
#pragma once
#include <cstdint>

namespace runtime::user {
constexpr uint32_t kSignedInIndex = 0;
constexpr uint64_t kXuid = 0xB13EBABEBABEBABEull;  // + index (Xenia)
constexpr char kGamertag[] = "User";                // user_profile.cc:27 (index 0)
constexpr uint32_t kSigninStateLocal = 1;           // eXUserSigninState_SignedInLocally
// Profile type bits as Xenia's XamUserGetXUID reads them (xam_user.cc:41-50: 1 offline, 2 and 4 online). Xenia's profile
// reports 1 | 2 (user_profile.h:230, "local | online profile?"); this one is offline only (the policy above).
constexpr uint32_t kProfileType = 1;
inline bool SignedIn(uint32_t index) { return index == kSignedInIndex; }

// XamUserReadProfileSettings, after its checks passed: writes the X_USER_READ_PROFILE_SETTINGS header, `count`
// X_USER_PROFILE_SETTING records and the string data for the setting ids at `ids` into `buffer` (Xenia
// xam_user.cc:283-320; profile_settings.cpp). `byXuid`: the call named user 0 by its XUID (the record holds the XUID)
// rather than by index (the record holds the index and FFFFFFFF). A setting id the profile does not have stops (7).
void WriteProfileSettings(uint8_t* base, uint32_t buffer, uint32_t count, uint32_t ids, bool byXuid, uint32_t userIndex);
}  // namespace runtime::user
