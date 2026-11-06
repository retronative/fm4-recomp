// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// System-module exports by ordinal (exports.h).
#include "exports.h"
#include "arena.h"
#include "imports.h"
#include <map>
#include <mutex>
#include <string_view>

namespace runtime {
namespace {

// Ordinals from Xenia xenia-canary 67f16c4e315f src/xenia/kernel/{xam/xam_table.inc,xboxkrnl/xboxkrnl_table.inc}: the ordinals FM4
// names as constants at its GetProcAddress call sites (the call sites and their evidence:
// reference/kernel/xam_ordinals.md; no title address in runtime source). Looking one up only gives its
// trampoline; calling it stops unless the runtime implements it.
constexpr SystemExport kExports[] = {
    {"xam.xex", 0xAFF, "XamPartyGetUserList", "Xenia xam_table.inc:1686"},
    {"xam.xex", 0xB00, "XamPartySendGameInvites", "Xenia xam_table.inc:1687"},
    {"xam.xex", 0xB0B, "XamPartySetCustomData", "Xenia xam_table.inc:1698"},
    {"xam.xex", 0xB10, "XamPartyGetBandwidth", "Xenia xam_table.inc:1703"},
    {"xam.xex", 0x305, "XamShowPartyUI", "Xenia xam_table.inc:569"},
    {"xam.xex", 0x30B, "XamShowCommunitySessionsUI", "Xenia xam_table.inc:575"},
    {"xam.xex", 0x9BB, "XamMediaVerificationCreate", "Xenia xam_table.inc:1586"},
    {"xam.xex", 0x9BC, "XamMediaVerificationClose", "Xenia xam_table.inc:1587"},
    {"xam.xex", 0x9BD, "XamMediaVerificationVerify", "Xenia xam_table.inc:1588"},
    {"xam.xex", 0x9BE, "XamMediaVerificationFailedBlocks", "Xenia xam_table.inc:1589"},
    {"xam.xex", 0x2FD, "XamReadTileToTextureEx", "Xenia xam_table.inc:561"},
    {"xam.xex", 0x369, "XuiAlloc", "Xenia xam_table.inc:669"},
    {"xam.xex", 0x8DF, "XamNuiGetTrueColorInfo", "Xenia xam_table.inc:1568"},
    // Resolved after the first frames.
    {"xam.xex", 0x50, "NetDll_XNetStartupEx", "Xenia xam_table.inc:80"},
    {"xam.xex", 0x24, "NetDll_WSAStartupEx", "Xenia xam_table.inc:50"},
    {"xam.xex", 0x48C, "XamVoiceSetMicArrayIdleUsers", "Xenia xam_table.inc:960"},
    // Resolved by FM4's static initializers (reference/kernel/xam_ordinals.md, second section).
    {"xam.xex", 0x1BA, "XamInputGetUserVibrationLevel", "Xenia xam_table.inc:239"},
    {"xam.xex", 0x208, "XamUserGetDeviceContext", "Xenia xam_table.inc:317"},
    {"xboxkrnl.exe", 0x282, "XInputdFFGetDeviceInfo", "Xenia xboxkrnl_table.inc:656"},
    {"xboxkrnl.exe", 0x283, "XInputdFFSetEffect", "Xenia xboxkrnl_table.inc:657"},
    {"xboxkrnl.exe", 0x284, "XInputdFFUpdateEffect", "Xenia xboxkrnl_table.inc:658"},
    {"xboxkrnl.exe", 0x285, "XInputdFFEffectOperation", "Xenia xboxkrnl_table.inc:659"},
    {"xboxkrnl.exe", 0x286, "XInputdFFDeviceControl", "Xenia xboxkrnl_table.inc:660"},
    {"xboxkrnl.exe", 0x287, "XInputdFFSetDeviceGain", "Xenia xboxkrnl_table.inc:661"},
    {"xboxkrnl.exe", 0x288, "XInputdFFCancelIo", "Xenia xboxkrnl_table.inc:662"},
    {"xboxkrnl.exe", 0x289, "XInputdFFSetRumble", "Xenia xboxkrnl_table.inc:663"},
};

std::mutex lock;
std::map<uint32_t, const SystemExport*> trampolines;  // address -> export
std::map<const SystemExport*, uint32_t> addresses;    // export -> address

}  // namespace

const SystemExport* FindSystemExport(const char* module, uint32_t ordinal)
{
    for (const SystemExport& e : kExports)
        if (std::string_view(e.module) == module && e.ordinal == ordinal)
            return &e;
    return nullptr;
}

uint32_t ExportTrampoline(uint8_t* base, const SystemExport& e)
{
    std::lock_guard guard(lock);
    if (const auto it = addresses.find(&e); it != addresses.end())
        return it->second;
    const uint32_t address = ArenaAlloc(base, 4);  // a distinct, aligned guest address
    trampolines[address] = &e;
    addresses[&e] = address;
    return address;
}

const SystemExport* TrampolineExport(uint32_t address)
{
    std::lock_guard guard(lock);
    const auto it = trampolines.find(address);
    return it == trampolines.end() ? nullptr : it->second;
}

void CallExport(PPCContext& ctx, uint8_t* base, const SystemExport& e)
{
    if (auto implementation = FindImport(e.name))
        implementation(ctx, base);
    else
        PpcImportStop(e.name, ctx);
}

}  // namespace runtime
