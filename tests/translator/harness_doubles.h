// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
//
// Test doubles for the translator harness (tests/run_translator_tests.sh), force-included before
// ppc_context.h. The harness has no function table, so an indirect call records its target in r9
// instead of calling it.
#pragma once
#define PPC_CALL_INDIRECT_FUNC(x) (ctx.r9.u64 = uint32_t(x))
// The guest time base is the runtime's; the harness returns a fixed value.
inline uint64_t PpcQueryTimebase() { return 0x0123456789ABCDEFull; }
