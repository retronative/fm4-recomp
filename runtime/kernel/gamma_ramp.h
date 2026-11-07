// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The display gamma ramp (DC_LUT_*), a pure model: no guest memory, no locks, no logging. Semantics from Xenia
// xenia-canary 67f16c4e315f ("X file:line", src/xenia/gpu/):
//  - registers and layouts: register_table.inc:428-479 (DC_LUT_RW_MODE 1921 bit 0: 0 256-entry table, 1 PWL;
//    DC_LUT_RW_INDEX 1922; DC_LUT_SEQ_COLOR 1923; DC_LUT_PWL_DATA 1924; DC_LUT_30_COLOR 1925; DC_LUT_WRITE_EN_MASK 1927
//    bit 0 blue, 1 green, 2 red; DC_LUTA_CONTROL 1930), registers.h:852-899 (rw_index is 8 bits; PWL base bits 0-15,
//    delta 16-31; 30_COLOR blue 0-9, green 10-19, red 20-29);
//  - the default (linear) ramps: command_processor.cc:83-100 (CommandProcessor::Initialize);
//  - the write state machine: command_processor.cc:469-574 (HandleSpecialRegisterWrite), entered only for
//    RW_INDEX..30_COLOR (:604-606), after the register file holds the written value (:589);
//  - the PWL application at present: shaders/apply_gamma_pwl.xesli:12-25, 74-100 (fetched from the 67f16c4e315f tree);
//    the PWL ramp is used for a 2_10_10_10 front buffer, the table otherwise
//    (vulkan/vulkan_command_processor.cc IssueSwap, use_pwl_gamma_ramp).
#pragma once
#include <cstddef>
#include <cstdint>

namespace runtime::gpu::gamma {

namespace reg { constexpr uint32_t kRwMode = 0x1921, kRwIndex = 0x1922, kSeqColor = 0x1923, kPwlData = 0x1924, k30Color = 0x1925,
                                   kWriteEnMask = 0x1927, kLutaControl = 0x1930; }

struct PwlEntry { uint16_t base = 0, delta = 0; };

struct Ramp
{
    uint32_t table[256] = {};  // DC_LUT_30_COLOR layout: blue 0-9, green 10-19, red 20-29
    PwlEntry pwl[128][3] = {}; // [entry][component], component 0 red, 1 green, 2 blue (X command_processor.cc:517-518)
    uint32_t component = 0;    // gamma_ramp_rw_component_: the next sequential component (0 red, 1 green, 2 blue)
};

// X command_processor.cc:83-100: table[i] = i * 0x3FF / 0xFF in each channel; PWL entry i: base (i * 0xFFFF / 0x7F) & ~0x3F,
// delta 0x200 below entry 0x7F and 0 at it.
Ramp DefaultRamp();

// The registers whose write runs the state machine: DC_LUT_RW_INDEX..DC_LUT_30_COLOR (X command_processor.cc:604-606).
constexpr bool IsStateRegister(uint32_t index) { return index - reg::kRwIndex <= reg::k30Color - reg::kRwIndex; }

enum class Write { None, Table, Pwl, ModeMismatch };

// Runs X HandleSpecialRegisterWrite for register `index` after it was written with `value`. `rwMode` and `writeEnable`
// are DC_LUT_RW_MODE and DC_LUT_WRITE_EN_MASK as the register file holds them; `rwIndex` is DC_LUT_RW_INDEX's register,
// updated in place as Xenia updates the register file (only its low 8 bits, the rw_index field, change). Returns which
// table an enabled component was written to (Xenia's OnGammaRamp*ValueWritten), None for RW_INDEX or a masked write, and
// ModeMismatch, changing nothing, for a data write in the other mode (Xenia asserts these, :477, :513, :548: never
// observed, so refused here rather than guessed).
Write OnRegisterWritten(Ramp& ramp, uint32_t index, uint32_t value, uint32_t rwMode, uint32_t writeEnable, uint32_t& rwIndex);

// X apply_gamma_pwl.xesli:12-25 for one 10-bit channel value with increment 8 (:78-80, entry v >> 3, multiplier v & 7):
// saturate((base + (v & 7) * delta / 8) / (64 * 1023)), stored to a 10-bit UNORM target, i.e. the nearest integer to
// (8 * base + (v & 7) * delta) / 512, clamped to 1023. Exact rational arithmetic; a tie (remainder 256) rounds up.
uint32_t ApplyPwl10(uint32_t v, PwlEntry e);

// The front buffer after the ramp, in place: words with red in bits 0-9, green 10-19, blue 20-29 (the display image's
// A2B10G10R10 after front_buffer.h's red/blue exchange). Red uses component 0, green 1, blue 2 (X apply_gamma_pwl.xesli:
// 82-93); alpha becomes 3 (1.0, :99).
void ApplyPwlToFrame(uint32_t* pixels, size_t count, const PwlEntry (&pwl)[128][3]);

// FNV-1a 64 over the PWL table as (base, delta) little-endian 16-bit pairs, entry-major, red-green-blue: the log's digest.
uint64_t PwlDigest(const PwlEntry (&pwl)[128][3]);
// Direct3D 9's linear PWL ramp (X register_table.inc:341-371: entry i base i * 0x200, delta 0x200; entry 7F base FFC0, delta
// 0), in all three components.
bool IsD3dLinearPwl(const PwlEntry (&pwl)[128][3]);

}  // namespace runtime::gpu::gamma
