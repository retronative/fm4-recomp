// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_module.cpp (GPL-3.0-or-later, same owner),
// reviewed and re-tested here. FM4 Recomp changes: only the imports a run has reached so far
// (RtlImageXexHeaderField, XexCheckExecutablePrivilege, XexGetModuleHandle, XexGetModuleSection, XexLoadImage,
// XexGetProcedureAddress); the
// others stay strict stubs until reached. Title modules (XMediaFacade) load through kernel/loader.h.
//
// Module imports. Semantics: Xenia xenia-canary 67f16c4e315f:
// src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc:501 RtlImageXexHeaderField_entry, which returns
// UserModule::GetOptHeader(memory, header, key, out) (src/xenia/kernel/user_module.cc:314).
#include "imports.h"
#include "exports.h"
#include "file_system.h"
#include "loader.h"
#include "module.h"
#include <cstring>
#include <fstream>
#include <mutex>
#include <utility>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadHostFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

thread_local bool inEntry = false;  // a load from inside a DllMain stops

// Title-terminate notifications ({routine, priority}; Xenia kernel_state.cc:165-180). The runtime never ends the
// title normally (a run ends at a stop), so they are recorded and never called.
std::mutex terminateLock;
std::vector<std::pair<uint32_t, uint32_t>> terminateNotifications;

// Loads a translated title module named `text` (Xenia KernelState::LoadUserModule, FinishLoadingUserModule
// kernel_state.cc:452): the file comes from the disc and must be the one translated (size and CRC-32),
// the image is the survey's image.bin (CRC-32) and the header the file's first bytes (CRC-32); then it is
// mapped and its DllMain runs on this thread with (module handle, DLL_PROCESS_ATTACH 1, 0). Returns 0, or
// NO_SUCH_FILE if the file is not on the disc. HYPOTHESIS: the module handle passed to DllMain is the
// loader entry (as XexGetModuleHandle returns); Xenia passes its object handle (kernel_state.cc:470).
uint32_t LoadTitleModule(PPCContext& ctx, uint8_t* base, const char* text, uint32_t& ldr)
{
    std::string path = text;
    if (path.find(':') == std::string::npos && (path.empty() || path[0] != '\\'))
        path = "game:\\" + path;  // a bare name is on the disc (INFERRED)
    runtime::DiscEntry entry;
    std::string device;
    switch (runtime::ResolveGuestPath(path, entry, device))
    {
    case runtime::Resolved::OtherDevice:
        Stop(StopCode::Unimplemented, "XexLoadImage(\"%s\" = %s): not on the disc, and no other device is mounted", text, device.c_str());
    case runtime::Resolved::NotFound:
        return 0xC000000F;  // STATUS_NO_SUCH_FILE (Xenia's result when the load fails)
    case runtime::Resolved::Found:
        break;
    }
    const runtime::ModuleImage* image = runtime::FindModuleImage(text);
    if (image == nullptr || entry.directory)
        Stop(StopCode::Unimplemented, "XexLoadImage(\"%s\"): on the disc, but not a module the runtime translated", text);
    if (inEntry)
        Stop(StopCode::Unimplemented, "XexLoadImage(\"%s\") from inside a DllMain is not implemented", text);
    const std::vector<uint8_t> file = ReadHostFile(entry.host);
    if (file.size() != image->fileSize || runtime::Crc32(file.data(), file.size()) != image->fileCrc32)
        Stop(StopCode::Setup, "%s is not the file %s was translated from (re-run scripts/survey.sh --module %s)", entry.host.c_str(),
             image->name, image->name);
    const std::vector<uint8_t> bytes = ReadHostFile(runtime::ModuleSurveyImage(*image));
    if (bytes.size() != image->size || runtime::Crc32(bytes.data(), bytes.size()) != image->imageCrc32)
        Stop(StopCode::Setup, "%s is not the image %s was translated from", runtime::ModuleSurveyImage(*image).c_str(), image->name);
    const std::vector<uint8_t> header(file.begin(), file.begin() + image->headerSize);
    if (runtime::Crc32(header.data(), header.size()) != image->headerCrc32)
        Stop(StopCode::Setup, "the XEX header of %s does not match its translation", image->name);
    const runtime::LoadedModule& module = runtime::MapModule(base, *image, bytes, header);
    ldr = module.module.ldr;
    if (image->entry != 0)
    {
        PPCFunc* dllMain = runtime::FindFunction(base, image->entry);
        if (dllMain == nullptr)
            Stop(StopCode::Setup, "the entry %08X of %s has no translated function", image->entry, image->name);
        PPCContext call = ctx;
        call.r3.u64 = ldr, call.r4.u64 = 1, call.r5.u64 = 0;
        inEntry = true;
        dllMain(call, base);
        inEntry = false;
        std::fprintf(stderr, "[runtime] loaded %s at %08X; DllMain(PROCESS_ATTACH) returned %u\n", image->fileName, image->base, call.r3.u32);
        if (call.r3.u32 == 0)
            Stop(StopCode::Unimplemented, "DllMain of %s returned FALSE", image->fileName);
    }
    return 0;
}

}  // namespace

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
    // (Xenia: hmodule_ptr points to the LDR_DATA_TABLE_ENTRY). A system module or a loaded title module
    // gives its entry; a translated module not loaded yet: ERROR_NOT_FOUND (0x490) and handle 0 (Xenia).
    // Any other name stops.
    const uint32_t name = ctx.r3.u32, handle = ctx.r4.u32;
    uint32_t ldr = runtime::ExecutableModule().ldr;
    if (name != 0 && base[name] != 0)
    {
        const char* text = reinterpret_cast<const char*>(base + name);
        ldr = runtime::SystemModule(base, text);
        if (ldr == 0)
        {
            if (const runtime::LoadedModule* loaded = runtime::LoadedModuleByName(text))
                ldr = loaded->module.ldr;
            else if (runtime::FindModuleImage(text) != nullptr)
            {
                runtime::Store32(base, handle, 0);
                return (void)(ctx.r3.u64 = 0x490);  // X_ERROR_NOT_FOUND
            }
            else
                Stop(StopCode::Unimplemented, "XexGetModuleHandle(\"%.64s\"): not a system or translated module", text);
        }
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
    const runtime::LoadedModule* loaded = runtime::LoadedModuleByLdr(module);
    if (module == 0 || (module != runtime::ExecutableModule().ldr && loaded == nullptr))
    {
        ctx.r3.u64 = 0xC0000008;  // STATUS_INVALID_HANDLE
        return;
    }
    const uint32_t header = loaded ? loaded->module.header : runtime::ExecutableModule().header;
    const char* wanted = reinterpret_cast<const char*>(base + name);
    const uint32_t info = runtime::XexHeaderField(base, header, 0x000002FF);
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
    // xboxkrnl.exe) are always loaded (Xenia: KernelState::GetModule finds them); a translated title
    // module is loaded on first use (LoadTitleModule). A file not on the disc: NO_SUCH_FILE, handle 0.
    const uint32_t name = ctx.r3.u32, handle = ctx.r6.u32;
    const char* text = name ? reinterpret_cast<const char*>(base + name) : "";
    uint32_t ldr = *text ? runtime::SystemModule(base, text) : 0;
    if (ldr == 0 && *text)
        if (const runtime::LoadedModule* loaded = runtime::LoadedModuleByName(text))
            ldr = loaded->module.ldr;
    if (ldr == 0)
    {
        if (!*text)
            Stop(StopCode::Unimplemented, "XexLoadImage with no name");
        if (const uint32_t status = LoadTitleModule(ctx, base, text, ldr))
        {
            runtime::Store32(base, handle, 0);
            return (void)(ctx.r3.u64 = status);
        }
    }
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
        const runtime::LoadedModule* loaded = runtime::LoadedModuleByLdr(module);
        if (loaded == nullptr)
            return (void)(ctx.r3.u64 = 0xC0000008);  // STATUS_INVALID_HANDLE
        // A title module's export by ordinal (Xenia xex_module.cc:144, xex2_info.h:614): the XEX export table
        // {.., image base >> 16 at +0x20, count +0x24, ordinal base +0x28, offsets from +0x2C}; the ordinal
        // minus the base, in 16 bits, must be below the count, else DRIVER_ENTRYPOINT_NOT_FOUND and 0.
        const uint32_t table = loaded->image->exportTable;
        if (table == 0)
            Stop(StopCode::Unimplemented, "XexGetProcedureAddress(%s, ordinal %u): PE exports are not implemented", loaded->image->fileName, ordinal);
        const uint16_t index = uint16_t(ordinal - runtime::Load32(base, table + 0x28));
        if (index >= runtime::Load32(base, table + 0x24))
        {
            runtime::Store32(base, out, 0);
            return (void)(ctx.r3.u64 = 0xC0000263);  // STATUS_DRIVER_ENTRYPOINT_NOT_FOUND
        }
        runtime::Store32(base, out, runtime::Load32(base, table + 0x2C + 4 * index) + (runtime::Load32(base, table + 0x20) << 16));
        return (void)(ctx.r3.u64 = 0);
    }
    const runtime::SystemExport* e = runtime::FindSystemExport(system, ordinal);
    if (e == nullptr)
        Stop(StopCode::Unimplemented, "XexGetProcedureAddress(%s, ordinal %u = 0x%X): an export the runtime does not know", system, ordinal, ordinal);
    runtime::Store32(base, out, runtime::ExportTrampoline(base, *e));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(ExRegisterTitleTerminateNotification, "REFERENCE_DERIVED: Xenia xboxkrnl_modules.cc:206, xbox.h:307 X_EX_TITLE_TERMINATE_REGISTRATION")
{
    // (registration*, create): {routine +0, priority +4}; create adds it, otherwise the routine is removed. The
    // routine must be a translated function (else stop 7): it would run when the title ends.
    const uint32_t reg = ctx.r3.u32, routine = runtime::Load32(base, reg), priority = runtime::Load32(base, reg + 4);
    std::lock_guard guard(terminateLock);
    if (ctx.r4.u32 != 0)
    {
        if (runtime::FindFunction(base, routine) == nullptr)
            Stop(StopCode::Unimplemented, "ExRegisterTitleTerminateNotification: routine %08X is no translated function", routine);
        terminateNotifications.emplace_back(routine, priority);
    }
    else
        std::erase_if(terminateNotifications, [&](const auto& n) { return n.first == routine; });
}

namespace runtime {
std::vector<std::pair<uint32_t, uint32_t>> TitleTerminateNotifications()
{
    std::lock_guard guard(terminateLock);
    return terminateNotifications;
}
}  // namespace runtime
