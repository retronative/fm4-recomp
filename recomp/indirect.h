// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp recomp/indirect.h (GPL-3.0-or-later, same owner), reviewed and re-tested here.
// FM4 Recomp changes: none.
//
// Force-included into the translated code: an indirect call (bctrl, a bctr through a function
// pointer) goes through the runtime's checked lookup, which stops with the guest address instead
// of calling a null or wild host pointer. Kept apart from runtime/ so that runtime changes never
// recompile the translated code.
#pragma once
#include <cstdint>

struct PPCContext;
void PpcCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target);

#undef PPC_CALL_INDIRECT_FUNC  // replaces ppc_context.h's unchecked default
#define PPC_CALL_INDIRECT_FUNC(x) PpcCallIndirect(ctx, base, (x))
