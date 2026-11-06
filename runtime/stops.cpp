// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/stops.cpp (GPL-3.0-or-later, same owner), reviewed and re-tested here.
// FM4 Recomp changes: an import stop also reports r7 and r8 (six argument registers); PpcStrictStop is
// PpcResidualStop (patch 0035: the dispatch's name, address, index and caller); unused parameters are
// marked instead of a disabled warning; messages name our milestones.
#include "stops.h"
#include "indirect.h"
#include "layout.h"
#include <cstdarg>
#include <cstdio>
#include <unistd.h>

void Stop(StopCode code, const char* format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    char line[600];
    const int length = snprintf(line, sizeof(line), "[stop %d] %s\n", int(code), message);
    // write(2) and _exit(2): also reached from the fault handler and the watchdog thread.
    (void)!write(STDERR_FILENO, line, size_t(length) < sizeof(line) ? size_t(length) : sizeof(line) - 1);
    _exit(int(code));
}

void PpcImportStop(const char* name, PPCContext& ctx)
{
    Stop(StopCode::Import, "import %s called from %08X (r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X)", name,
         uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
}

void PpcResidualStop(uint32_t address, const char* name, uint32_t index, uint32_t lr)
{
    Stop(StopCode::Residual, "residual stop %s: switch at %08X has no proven bound (index %u, lr=%08X)", name, address,
         index, lr);
}

void PpcCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    PPCFunc* function = runtime::IsCodeAddress(target, PPC_CODE_BASE, PPC_CODE_SIZE) ? PPC_LOOKUP_FUNC(base, target) : nullptr;
    if (function == nullptr)
        Stop(StopCode::IndirectCall, "indirect call to %08X, which is no translated function (lr=%08X)", target, uint32_t(ctx.lr));
    function(ctx, base);
}

// Runtime interfaces the translation declares; each is M3 work and stops until then.
uint64_t PpcQueryTimebase()
{
    Stop(StopCode::Unimplemented, "mftb: the guest time base is not implemented (M3)");
}

jmp_buf& PpcJumpBuffer(uint32_t guestAddress)
{
    Stop(StopCode::Unimplemented, "setjmp/longjmp on the guest jmp_buf at %08X is not implemented (M3)", guestAddress);
}

PPC_FUNC(PpcRuntime_switch_fiber)
{
    (void)base;
    Stop(StopCode::Unimplemented, "fiber switch is not implemented (M3; lr=%08X)", uint32_t(ctx.lr));
}

PPC_FUNC(PpcRuntime_restore_context)
{
    (void)base;
    Stop(StopCode::Unimplemented, "context restore is not implemented (M3; lr=%08X)", uint32_t(ctx.lr));
}
