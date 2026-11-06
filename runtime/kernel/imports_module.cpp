// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_module.cpp (GPL-3.0-or-later, same owner),
// reviewed and re-tested here. FM4 Recomp changes: only the imports a run has reached so far
// (RtlImageXexHeaderField, XexCheckExecutablePrivilege, XexGetModuleHandle, XexGetModuleSection, XexLoadImage,
// XexGetProcedureAddress); the
// others stay strict stubs until reached. Until title modules are loaded (XexLoadImage), the executable
// is the only title module: any other module name stops, and only its handle has sections.
//
// Module imports. Semantics: Xenia xenia-canary 67f16c4e315f:
// src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc:501 RtlImageXexHeaderField_entry, which returns
// UserModule::GetOptHeader(memory, header, key, out) (src/xenia/kernel/user_module.cc:314).
#include "imports.h"
#include "exports.h"
#include "module.h"
#include <cstring>

GUEST_IMPORT(RtlImageXexHeaderField,
             "REFERENCE_DERIVED: Xenia xboxkrnl_rtl.cc:501 RtlImageXexHeaderField, user_module.cc GetOptHeader")
{
    ctx.r3.u64 = runtime::XexHeaderField(base, ctx.r3.u32, ctx.r4.u32);
}

GUEST_IMPORT(XexCheckExecutablePrivilege,
             "REFERENCE_DERIVED: Xenia xboxkrnl_modules.cc:24 XexCheckExecutablePrivilege (bit of XEX_HEADER_SYSTEM_FLAGS)")
{
    // The privilege is a bit number in the executable's system flags (optional header 0x00030000).
    const uint32_t privilege = ctx.r3.u32;
    if (privilege >= 32)
        Stop(StopCode::Unimplemented, "XexCheckExecutablePrivilege(%u): privileges past 31 are not implemented", privilege);
    const uint32_t flags = runtime::XexHeaderField(base, runtime::ExecutableModule().header, 0x00030000);
    ctx.r3.u64 = (flags >> privilege) & 1;
}

GUEST_IMPORT(XexGetModuleHandle, "REFERENCE_DERIVED: Xenia xboxkrnl_modules.cc:44 XexGetModuleHandle, xmodule.h:98 hmodule_ptr")
{
    // (name*, handle*): a null or empty name is the executable; the handle is its loader entry
    // (Xenia: hmodule_ptr points to the LDR_DATA_TABLE_ENTRY). A system module gives its entry.
    // Any other name stops: no title module but the executable is loaded yet.
    const uint32_t name = ctx.r3.u32, handle = ctx.r4.u32;
    uint32_t ldr = runtime::ExecutableModule().ldr;
    if (name != 0 && base[name] != 0)
    {
        const char* text = reinterpret_cast<const char*>(base + name);
        ldr = runtime::SystemModule(base, text);
        if (ldr == 0)
            Stop(StopCode::Unimplemented, "XexGetModuleHandle(\"%.64s\"): not a system module, and no title module is loaded", text);
    }
    runtime::Store32(base, handle, ldr);
    ctx.r3.u64 = 0;  // X_ERROR_SUCCESS
}

GUEST_IMPORT(XexGetModuleSection, "REFERENCE_DERIVED: Xenia xboxkrnl_modules.cc:72 XexGetModuleSection, user_module.cc:260 GetSection")
{
    // (module, name*, data*, size*): a named resource of the module (XEX_HEADER_RESOURCE_INFO,
    // 0x000002FF: a size word, then 16-byte entries: an 8-character name, the data's guest address
    // and size). Not found: STATUS_NOT_FOUND; a handle that is no module: STATUS_INVALID_HANDLE.
    const uint32_t module = ctx.r3.u32, name = ctx.r4.u32, dataOut = ctx.r5.u32, sizeOut = ctx.r6.u32;
    if (runtime::SystemModuleName(module) != nullptr)
        Stop(StopCode::Unimplemented, "XexGetModuleSection on system module %s", runtime::SystemModuleName(module));
    if (module == 0 || module != runtime::ExecutableModule().ldr)
    {
        ctx.r3.u64 = 0xC0000008;  // STATUS_INVALID_HANDLE
        return;
    }
    const char* wanted = reinterpret_cast<const char*>(base + name);
    const uint32_t info = runtime::XexHeaderField(base, runtime::ExecutableModule().header, 0x000002FF);
    const uint32_t count = info ? (runtime::Load32(base, info) - 4) / 16 : 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t entry = info + 4 + 16 * i;
        if (strnlen(wanted, 9) > 8 || strncmp(wanted, reinterpret_cast<const char*>(base + entry), 8) != 0)
            continue;
        runtime::Store32(base, dataOut, runtime::Load32(base, entry + 8));
        runtime::Store32(base, sizeOut, runtime::Load32(base, entry + 12));
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = 0xC0000225;  // STATUS_NOT_FOUND
}

GUEST_IMPORT(XexLoadImage, "REFERENCE_DERIVED: Xenia xboxkrnl_modules.cc:94 XexLoadImage, xmodule.h:41 load_count")
{
    // (name*, flags, minimum version, handle*): a module already loaded gives its loader entry and
    // its load count (big-endian 16 bits at +0x40) goes up. The system modules (xam.xex,
    // xboxkrnl.exe) are always loaded (Xenia: KernelState::GetModule finds them). Loading a title
    // module is not implemented yet: any other name stops.
    const uint32_t name = ctx.r3.u32, handle = ctx.r6.u32;
    const char* text = name ? reinterpret_cast<const char*>(base + name) : "";
    const uint32_t ldr = *text ? runtime::SystemModule(base, text) : 0;
    if (ldr == 0)
        Stop(StopCode::Unimplemented, "XexLoadImage(\"%.64s\", flags %X): loading a title module is not implemented", text, ctx.r4.u32);
    const uint32_t count = uint32_t(base[ldr + 0x40] << 8 | base[ldr + 0x41]) + 1;
    base[ldr + 0x40] = uint8_t(count >> 8), base[ldr + 0x41] = uint8_t(count);
    runtime::Store32(base, handle, ldr);
    ctx.r3.u64 = 0;  // X_STATUS_SUCCESS
}

GUEST_IMPORT(XexGetProcedureAddress, "REFERENCE_DERIVED: Xenia xboxkrnl_modules.cc:160 XexGetProcedureAddress, kernel_module.cc:87")
{
    // (module, ordinal or name*, address*): a system module's export by ordinal gives its trampoline
    // (exports.h). An ordinal the runtime does not know, a lookup by name (a value above 0xFFFF), or
    // the executable's own exports stop (7): a real console may have them, so no failure is invented.
    // A handle that is no module: STATUS_INVALID_HANDLE (as Xenia).
    const uint32_t module = ctx.r3.u32, ordinal = ctx.r4.u32, out = ctx.r5.u32;
    if (ordinal > 0xFFFF)
        Stop(StopCode::Unimplemented, "XexGetProcedureAddress by name (\"%.64s\") is not implemented",
             reinterpret_cast<const char*>(base + ordinal));
    const char* system = runtime::SystemModuleName(module);
    if (system == nullptr)
    {
        if (module == 0 || module == runtime::ExecutableModule().ldr)
            Stop(StopCode::Unimplemented, "XexGetProcedureAddress(executable, ordinal %u): its exports are not implemented", ordinal);
        ctx.r3.u64 = 0xC0000008;  // STATUS_INVALID_HANDLE
        return;
    }
    const runtime::SystemExport* e = runtime::FindSystemExport(system, ordinal);
    if (e == nullptr)
        Stop(StopCode::Unimplemented, "XexGetProcedureAddress(%s, ordinal %u = 0x%X): an export the runtime does not know", system, ordinal, ordinal);
    runtime::Store32(base, out, runtime::ExportTrampoline(base, *e));
    ctx.r3.u64 = 0;
}
