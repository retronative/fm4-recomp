// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XOVERLAPPED completion (overlapped.h) and XamGetOverlappedResult. REFERENCE_DERIVED from Xenia xenia-canary
// 67f16c4e315f (kernel/kernel_state.cc:763-809, kernel/xam/xam_msg.cc:113-136, xbox.h:33 XSUCCEEDED,
// xbox.h:183-231), re-derived.
#include "overlapped.h"
#include "dispatcher.h"
#include "imports.h"
#include "objects.h"

namespace runtime {

namespace {

// The event's dispatcher header, or 0 when the XOVERLAPPED has no event. Stops on a completion routine or on an
// event field that names no event object.
uint32_t CheckedEvent(const PPCContext& ctx, const uint8_t* base, uint32_t address, const char* import)
{
    const uint32_t routine = Load32(base, address + overlapped::kCompletionRoutine);
    if (routine != 0)
        Stop(StopCode::Unimplemented, "%s: XOVERLAPPED %08X has a completion routine %08X (context %08X; caller lr %08X): APCs are not "
             "modelled", import, address, routine, Load32(base, address + overlapped::kCompletionContext), uint32_t(ctx.lr));
    const uint32_t event = Load32(base, address + overlapped::kEvent);
    if (event == 0)
        return 0;
    const auto object = LookupHandle(event);
    if (!object || object->type != ObjectType::Event)
        Stop(StopCode::Unimplemented, "%s: XOVERLAPPED %08X event %08X is not an event handle (caller lr %08X)", import, address, event,
             uint32_t(ctx.lr));
    return object->header;
}

}  // namespace

void CheckOverlapped(const PPCContext& ctx, const uint8_t* base, uint32_t address, const char* import)
{
    (void)CheckedEvent(ctx, base, address, import);
}

void CompleteOverlappedEx(const PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t result, uint32_t extendedError,
                          uint32_t length, const char* import)
{
    const uint32_t event = CheckedEvent(ctx, base, address, import);
    Store32(base, address + overlapped::kResult, result);
    Store32(base, address + overlapped::kExtendedError, extendedError);
    Store32(base, address + overlapped::kLength, length);
    if (event != 0)
        SetEvent(base, event);  // after the fields: a waiter woken here reads the completed XOVERLAPPED
}

void CompleteOverlappedImmediate(const PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t result, const char* import)
{
    CompleteOverlappedEx(ctx, base, address, result, result, result == 0 ? 0 : 0xFFFFFFFF, import);
}

uint32_t FinishOverlapped(const PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t result, uint32_t extendedError,
                          uint32_t length, const char* import)
{
    if (address == 0)
        return result;
    CompleteOverlappedEx(ctx, base, address, result, extendedError, length, import);
    return overlapped::kErrorIoPending;
}

}  // namespace runtime

GUEST_IMPORT(XamGetOverlappedResult, "REFERENCE_DERIVED: Xenia xam_msg.cc:113-136 XamGetOverlappedResult (pending: stops, see below)")
{
    // (XOVERLAPPED*, length*, wait). A result other than ERROR_IO_PENDING (0x3E5) is returned as it is; when it
    // succeeded (XSUCCEEDED, xbox.h:33: bits 31-30 clear, so every Win32 code does) the length is written to length*
    // if given. Pending without an event: ERROR_IO_INCOMPLETE (0x3E4). Pending with an event: Xenia waits on it with
    // no timeout. Here every completion is immediate (overlapped.h), so nothing would ever set that event: the wait
    // could only end at the run's deadline. It stops instead, naming the XOVERLAPPED. The third argument (the XDK's
    // bWait, INFERRED from the name of XGetOverlappedResult's parameter) is ignored, as Xenia does.
    using namespace runtime;
    const uint32_t address = ctx.r3.u32, lengthOut = ctx.r4.u32;
    if (address == 0)
        Stop(StopCode::Unimplemented, "XamGetOverlappedResult: a null XOVERLAPPED (caller lr %08X)", uint32_t(ctx.lr));
    uint32_t result = Load32(base, address + overlapped::kResult);
    if (result == overlapped::kErrorIoPending)
    {
        if (Load32(base, address + overlapped::kEvent) != 0)
            Stop(StopCode::Unimplemented, "XamGetOverlappedResult: XOVERLAPPED %08X is pending with event %08X; no asynchronous "
                 "completion is modelled, so the wait could never end (caller lr %08X)", address,
                 Load32(base, address + overlapped::kEvent), uint32_t(ctx.lr));
        result = overlapped::kErrorIoIncomplete;
    }
    if ((result & 0xC0000000u) == 0 && lengthOut != 0)
        Store32(base, lengthOut, Load32(base, address + overlapped::kLength));
    ctx.r3.u64 = result;
}
