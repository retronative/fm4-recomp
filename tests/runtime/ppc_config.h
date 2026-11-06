// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// A synthetic guest layout for the runtime tests (tests/run_runtime_tests.sh): no title's values.
// The real one is ppc/ppc_config.h, written by XenonRecomp. Same macros and form. 0x90000000 lies
// outside every guest heap (kernel/virtual_memory.cpp: 0-0x70000000 virtual, 0xA0000000+ physical)
// and below them no runtime range (stacks 0x70000000, arena 0x7E000000, guards 0x7F000000).
#pragma once
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED

#define PPC_IMAGE_BASE 0x90000000ull
#define PPC_IMAGE_SIZE 0x10000ull
#define PPC_CODE_BASE 0x90001000ull
#define PPC_CODE_SIZE 0x4000ull

#endif
