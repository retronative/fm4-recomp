// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_module.cpp (GPL-3.0-or-later, same owner),
// reviewed and re-tested here. FM4 Recomp changes: only the imports a run has reached so far
// (RtlImageXexHeaderField, XexCheckExecutablePrivilege); the others stay strict stubs until reached.
//
// Module imports. Semantics: Xenia xenia-canary 67f16c4e315f:
// src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc:501 RtlImageXexHeaderField_entry, which returns
// UserModule::GetOptHeader(memory, header, key, out) (src/xenia/kernel/user_module.cc:314).
#include "imports.h"
#include "module.h"

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
