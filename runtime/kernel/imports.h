// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: GUEST_IMPORT marks its parameters [[maybe_unused]].
//
// Kernel/XAM imports (M3-T04). The generated strict stubs (fm4_imports.cpp) are weak: an import
// is implemented by defining it with GUEST_IMPORT, which overrides the stub and records its
// provenance (VERIFIED or REFERENCE_DERIVED, with the source). Anything not implemented still
// stops. Arguments arrive in r3..r10, results go to r3, as in the guest ABI.
#pragma once
#include "stops.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace runtime {

struct ImportInfo { const char* name; const char* provenance; void (*function)(PPCContext&, uint8_t*); };
bool RegisterImport(const char* name, const char* provenance, void (*function)(PPCContext&, uint8_t*));
// The implementation of an import by name, or null.
void (*FindImport(const char* name))(PPCContext&, uint8_t*);
size_t ImplementedImportCount();
const ImportInfo& ImplementedImport(size_t index);

// The calling guest thread's KTHREAD (its PCR's current thread; imports_object.cpp).
uint32_t CurrentThread(const PPCContext& ctx, const uint8_t* base);

// Guest memory helpers (big-endian).
inline uint32_t Load32(const uint8_t* base, uint32_t address)
{
    uint32_t v;
    std::memcpy(&v, base + address, 4);
    return __builtin_bswap32(v);
}
inline void Store32(uint8_t* base, uint32_t address, uint32_t value)
{
    const uint32_t v = __builtin_bswap32(value);
    std::memcpy(base + address, &v, 4);
}
inline void Store16(uint8_t* base, uint32_t address, uint16_t value)
{
    const uint16_t v = __builtin_bswap16(value);
    std::memcpy(base + address, &v, 2);
}

// Argument n (0-based) of an import: r3..r10, then the stack (n >= 8: the low word of the
// 8-byte slot at r1 + 0x50 + (n - 8) * 8; Xenia shim_utils.h get_arg_stack_ptr).
inline uint32_t Argument(const PPCContext& ctx, const uint8_t* base, unsigned n)
{
    const PPCRegister* registers[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    return n < 8 ? registers[n]->u32 : Load32(base, ctx.r1.u32 + 0x54 + (n - 8) * 8);
}

}  // namespace runtime

// FM4 Recomp: the definition marks its two parameters [[maybe_unused]]: the signature is the guest ABI's,
// so an import that needs no guest memory (or no registers) is not an error under -Wextra -Werror.
#define GUEST_IMPORT(name, provenance)                                                                     \
    PPC_FUNC(__imp__##name);                                                                               \
    [[maybe_unused]] static const bool registered_##name = runtime::RegisterImport(#name, provenance, __imp__##name); \
    void __imp__##name([[maybe_unused]] PPCContext& __restrict ctx, [[maybe_unused]] uint8_t* base)
