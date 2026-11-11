// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// CPU-only tests of the display gamma ramp model (runtime/kernel/gamma_ramp.h): write sequences -> table entries, and the
// PWL application. Expected values are computed by hand from Xenia 67f16c4e315f ("X file:line", src/xenia/gpu/) in the
// comment next to each check, never from the model. Write sequences marked OBSERVED-XENIA are the ones Xenia's
// register_table.inc:302-426 records for other titles (Direct3D 9's setup); FM4's own sequence is not traced yet
// (the local capture trace14 (not distributed) holds only the registers' last values at four draws).
//
//   tests/run_gamma_ramp_tests.sh
#include "gamma_ramp.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace runtime::gpu::gamma;

namespace {
int failures = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}
bool Is(PwlEntry e, uint16_t base, uint16_t delta) { return e.base == base && e.delta == delta; }

// One register write as the command processor makes it (the register file holds mode, enable and the index).
struct Device
{
    Ramp ramp;
    uint32_t mode = 0, enable = 7, rwIndex = 0;
    Write Put(uint32_t index, uint32_t value)
    {
        if (index == reg::kRwIndex)
            rwIndex = value;  // the register file takes the value first (X command_processor.cc:589)
        return OnRegisterWritten(ramp, index, value, mode, enable, rwIndex);
    }
};

void TestDefaults()
{
    const Ramp r = DefaultRamp();
    // X :86-92: i * 0x3FF / 0xFF. 1 -> 1023 / 255 = 4; 128 -> 130944 / 255 = 513 (0x201); 255 -> 1023.
    Expect(r.table[0] == 0, "default table 0");
    Expect(r.table[1] == 0x00401004, "default table 1 = 4 | 4 << 10 | 4 << 20");
    Expect(r.table[128] == 0x20180601, "default table 128 = 0x201 in each channel");
    Expect(r.table[255] == 0x3FFFFFFF, "default table 255");
    // X :93-100: base (i * 0xFFFF / 0x7F) & ~0x3F. 1: 516 -> 0x200; 64: 4194240 / 127 = 33025 (0x8101) -> 0x8100;
    // 0x7E: 8257410 / 127 = 65018 (0xFDFA) -> 0xFDC0; 0x7F: 0xFFFF -> 0xFFC0, delta 0.
    Expect(Is(r.pwl[0][0], 0, 0x200) && Is(r.pwl[1][1], 0x200, 0x200), "default pwl 0 and 1");
    Expect(Is(r.pwl[64][2], 0x8100, 0x200), "default pwl 0x40 = 8100/0200 (not linear)");
    Expect(Is(r.pwl[0x7E][0], 0xFDC0, 0x200) && Is(r.pwl[0x7F][2], 0xFFC0, 0), "default pwl 7E, 7F");
    Expect(r.component == 0, "default component 0");
    Expect(!IsD3dLinearPwl(r.pwl), "CONTROL: Xenia's default PWL is not Direct3D 9's linear ramp (bases differ from entry 0x40 on)");
}

void TestStateRegisters()
{
    // X :604-606: RW_INDEX..30_COLOR only.
    Expect(!IsStateRegister(reg::kRwMode) && IsStateRegister(reg::kRwIndex) && IsStateRegister(reg::k30Color) &&
           !IsStateRegister(0x1926) && !IsStateRegister(reg::kWriteEnMask) && !IsStateRegister(reg::kLutaControl) && !IsStateRegister(0),
           "state registers are 1922..1925");
}

void TestPwlWrites()
{
    // OBSERVED-XENIA (register_table.inc:343-357, 4D5307E6's linear ramp): mode 1, index 0, enable 7, then three equal
    // words per entry. X :511-544: entry = rw_index & 0x7F, component 0 red, 1 green, 2 blue; after blue the index's low 7
    // bits increase.
    Device d;
    d.mode = 1;
    d.Put(reg::kRwIndex, 0);
    bool all = true;
    for (int i = 0; i < 3; i++)
        all &= d.Put(reg::kPwlData, 0x02000000) == Write::Pwl;
    Expect(all && Is(d.ramp.pwl[0][0], 0, 0x200) && Is(d.ramp.pwl[0][1], 0, 0x200) && Is(d.ramp.pwl[0][2], 0, 0x200), "pwl entry 0 = 0000/0200");
    Expect(d.rwIndex == 1 && d.ramp.component == 0, "pwl index advances to 1 after the third component");
    for (int i = 0; i < 3; i++)
        d.Put(reg::kPwlData, 0x02000200);
    Expect(Is(d.ramp.pwl[1][2], 0x200, 0x200) && d.rwIndex == 2, "pwl entry 1 = 0200/0200, index 2");

    // Bits 0-5 of base and delta are hardwired to zero (X :527-529): 0x0247FFFF -> base FFC0, delta 0240.
    d.Put(reg::kRwIndex, 9);
    d.Put(reg::kPwlData, 0x0247FFFF);
    Expect(Is(d.ramp.pwl[9][0], 0xFFC0, 0x0240), "pwl low 6 bits dropped");
    Expect(d.ramp.component == 1 && d.rwIndex == 9, "pwl mid-entry: component 1, index unchanged");
    // A RW_INDEX write resets the component (X :469-473).
    d.Put(reg::kRwIndex, 9);
    Expect(d.ramp.component == 0, "RW_INDEX resets the component");

    // Write enable: bit 2 red, 1 green, 0 blue (X :519-521). 0b101: red and blue written, green skipped, index still moves.
    Device m;
    m.mode = 1, m.enable = 5;
    m.Put(reg::kRwIndex, 5);
    const Write r = m.Put(reg::kPwlData, 0x00400040), g = m.Put(reg::kPwlData, 0x00800080), b = m.Put(reg::kPwlData, 0x00C000C0);
    Expect(r == Write::Pwl && g == Write::None && b == Write::Pwl, "pwl masked green returns None");
    Expect(Is(m.ramp.pwl[5][0], 0x40, 0x40) && Is(m.ramp.pwl[5][1], 0, 0) && Is(m.ramp.pwl[5][2], 0xC0, 0xC0) && m.rwIndex == 6,
           "pwl mask 101: red and blue written, green kept, index 6");

    // Bit 7 of the index is ignored and kept (X :515-516, :537-539): 0x80 -> entry 0, then 0x81; 0xFF -> entry 7F, then 0x80;
    // 0x17F (bits above the field) -> entry 7F, then 0x100.
    Device w;
    w.mode = 1;
    w.Put(reg::kRwIndex, 0x80);
    for (int i = 0; i < 3; i++)
        w.Put(reg::kPwlData, 0x01000100);
    Expect(Is(w.ramp.pwl[0][1], 0x100, 0x100) && w.rwIndex == 0x81, "pwl index 0x80 writes entry 0, then 0x81");
    w.Put(reg::kRwIndex, 0xFF);
    for (int i = 0; i < 3; i++)
        w.Put(reg::kPwlData, 0x0000FFC0);
    Expect(Is(w.ramp.pwl[0x7F][2], 0xFFC0, 0) && w.rwIndex == 0x80, "pwl index 0xFF writes entry 7F, then 0x80");
    w.Put(reg::kRwIndex, 0x17F);
    for (int i = 0; i < 3; i++)
        w.Put(reg::kPwlData, 0);
    Expect(w.rwIndex == 0x100, "pwl index 0x17F -> 0x100");

    // CONTROL: a PWL write in table mode is refused and changes nothing (Xenia asserts, :513).
    Device x;
    x.mode = 0;
    x.Put(reg::kRwIndex, 3);
    const Ramp before = x.ramp;
    Expect(x.Put(reg::kPwlData, 0x02000200) == Write::ModeMismatch && x.rwIndex == 3 && x.ramp.component == 0 &&
           Is(x.ramp.pwl[3][0], before.pwl[3][0].base, before.pwl[3][0].delta), "CONTROL: PWL_DATA in mode 0 refused");
}

void TestTableWrites()
{
    // SEQ_COLOR (X :475-510): component 0 red, 1 green, 2 blue; value bits 0-15, >> 6. Entry 3 from zero:
    // 0xFFC0 -> red 3FF; 0x0040 -> green 1; 0x0001_8000 -> bits 0-15 = 8000 -> blue 200. 3FF << 20 | 1 << 10 | 200 = 3FF00600.
    Device s;
    s.Put(reg::kRwIndex, 3);
    s.Put(reg::kSeqColor, 0xFFC0);
    s.Put(reg::kSeqColor, 0x0040);
    Expect(s.Put(reg::kSeqColor, 0x00018000) == Write::Table, "seq write returns Table");
    Expect(s.ramp.table[3] == 0x3FF00600 && s.rwIndex == 4 && s.ramp.component == 0, "seq entry 3 = 3FF00600, index 4");
    // Masked red (enable 0b011): entry 4 keeps red, index still moves (X :481-483, :502-505).
    s.enable = 3;
    s.ramp.table[4] = 0x15500000;  // red 0x155
    s.Put(reg::kSeqColor, 0xFFC0);
    s.Put(reg::kSeqColor, 0xFFC0);
    s.Put(reg::kSeqColor, 0xFFC0);
    Expect(s.ramp.table[4] == 0x155FFFFF && s.rwIndex == 5, "seq mask 011: red kept, green and blue 3FF");
    // The 8-bit index wraps (registers.h:858): 0x1FF -> entry FF, then 0x100.
    s.enable = 7;
    s.Put(reg::kRwIndex, 0x1FF);
    for (int i = 0; i < 3; i++)
        s.Put(reg::kSeqColor, 0x0040);
    Expect(s.ramp.table[0xFF] == 0x00100401 && s.rwIndex == 0x100, "seq index FF wraps to 0 (register 0x100)");

    // 30_COLOR (X :546-573). OBSERVED-XENIA (register_table.inc:310-311, 4B4F07FE): index 1, 0x04812048 -> blue 048,
    // green 048, red 048: the entry is the word's bits 0-29.
    Device c;
    c.Put(reg::kRwIndex, 1);
    Expect(c.Put(reg::k30Color, 0x04812048) == Write::Table && c.ramp.table[1] == 0x04812048 && c.rwIndex == 2, "30_COLOR entry 1, index 2");
    Expect(c.Put(reg::k30Color, 0xFFFFFFFF) == Write::Table && c.ramp.table[2] == 0x3FFFFFFF, "30_COLOR drops bits 30-31");
    // Enable 0b010: green only. Mid-sequence component reset (X :568-571).
    c.enable = 2;
    c.ramp.component = 2;
    c.Put(reg::k30Color, 0x3FFFFFFF);
    Expect(c.ramp.table[3] == 0x000FFC00 && c.ramp.component == 0 && c.rwIndex == 4, "30_COLOR mask 010: green only, component 0");
    c.enable = 0;
    Expect(c.Put(reg::k30Color, 0x3FFFFFFF) == Write::None && c.ramp.table[4] == 0 && c.rwIndex == 5, "30_COLOR mask 0: nothing, index moves");
    c.mode = 1;
    Expect(c.Put(reg::k30Color, 1) == Write::ModeMismatch && c.rwIndex == 5, "CONTROL: 30_COLOR in mode 1 refused");
    Expect(c.Put(reg::kSeqColor, 1) == Write::ModeMismatch && c.ramp.component == 0, "CONTROL: SEQ_COLOR in mode 1 refused");
    // Writes to registers outside 1922..1925 do nothing.
    Expect(c.Put(reg::kLutaControl, 3) == Write::None && c.rwIndex == 5, "DC_LUTA_CONTROL write: no state change");
}

void TestApply()
{
    // Direct3D 9's linear ramp, written as Xenia records it (register_table.inc:341-371): entry i base i * 0x200, delta
    // 0x200; entry 7F base FFC0, delta 0.
    Device d;
    d.mode = 1;
    d.Put(reg::kRwIndex, 0);
    for (uint32_t i = 0; i < 128; i++)
        for (int c = 0; c < 3; c++)
            d.Put(reg::kPwlData, i < 0x7F ? 0x02000000 | (i * 0x200) : 0x0000FFC0);
    Expect(IsD3dLinearPwl(d.ramp.pwl) && d.rwIndex == 0, "the linear ramp written entry by entry (index wraps to 0)");
    // apply_gamma_pwl.xesli:21-24: (8 base + f delta) / 512. For i < 7F: (8 * 512 i + 512 f) / 512 = 8 i + f = v.
    bool identity = true;
    for (uint32_t v = 0; v < 1016; v++)
        identity &= ApplyPwl10(v, d.ramp.pwl[v >> 3][v % 3]) == v;
    Expect(identity, "CONTROL: the linear ramp leaves 0..1015 unchanged");
    // Entry 7F: 8 * 0xFFC0 / 512 = 1023 for 1016..1023.
    Expect(ApplyPwl10(1016, d.ramp.pwl[127][0]) == 1023 && ApplyPwl10(1023, d.ramp.pwl[127][2]) == 1023, "linear ramp top entry: 1023");

    // OBSERVED-XENIA (register_table.inc:378-392, 4D5307E6's in-game ramp): entry 0 = 05000000 (base 0, delta 500),
    // 1 = 02000500 (base 500, delta 200), 2 = 01800740 (base 740, delta 180).
    // v 4 (entry 0, f 4): 4 * 1280 / 512 = 10. v 8 (entry 1, f 0): 8 * 1280 / 512 = 20. v 9: (10240 + 512) / 512 = 21.
    // v 20 (entry 2, f 4): (8 * 1856 + 4 * 384) / 512 = 16384 / 512 = 32.
    Expect(ApplyPwl10(4, {0, 0x500}) == 10, "pwl v 4 -> 10");
    Expect(ApplyPwl10(8, {0x500, 0x200}) == 20 && ApplyPwl10(9, {0x500, 0x200}) == 21, "pwl v 8, 9 -> 20, 21");
    Expect(ApplyPwl10(20, {0x740, 0x180}) == 32, "pwl v 20 -> 32");
    // Rounding: v 7 on delta 500: 7 * 1280 / 512 = 17.5 -> 18 (a tie; rounds up, HYPOTHESIS: Xenia's float path and the host's
    // UNORM conversion decide it there). v 1 on delta 500: 2.5 -> 3. v 3: 7.5 -> 8. v 2: 5 exactly.
    Expect(ApplyPwl10(7, {0, 0x500}) == 18 && ApplyPwl10(1, {0, 0x500}) == 3 && ApplyPwl10(2, {0, 0x500}) == 5, "pwl ties round up");
    // Below a tie: delta 0x40, f 3: 3 * 64 / 512 = 0.375 -> 0; f 5: 0.625 -> 1.
    Expect(ApplyPwl10(3, {0, 0x40}) == 0 && ApplyPwl10(5, {0, 0x40}) == 1, "pwl rounds to nearest");
    // Saturation (xesl_saturate): base FFC0, delta FFC0, f 7: 15 * 65472 / 512 = 1918 -> 1023.
    Expect(ApplyPwl10(7, {0xFFC0, 0xFFC0}) == 1023, "pwl saturates at 1023");
    // Xenia's default at v 512 (entry 40, base 8100): 8 * 33024 / 512 = 516.
    const Ramp def = DefaultRamp();
    Expect(ApplyPwl10(512, def.pwl[64][0]) == 516 && ApplyPwl10(8, def.pwl[1][1]) == 8, "Xenia's default: 512 -> 516, 8 -> 8");

    // The frame: red bits 0-9 with component 0, green 10-19 with 1, blue 20-29 with 2 (apply_gamma_pwl.xesli:82-93); alpha 3
    // (:99). v 4 everywhere; entry 0: red delta 500 -> 10, green delta 200 -> 4, blue delta 100 -> 4 * 256 / 512 = 2.
    PwlEntry pwl[128][3] = {};
    pwl[0][0] = {0, 0x500}, pwl[0][1] = {0, 0x200}, pwl[0][2] = {0, 0x100};
    uint32_t pixels[2] = {4 | 4 << 10 | 4 << 20, 0};
    ApplyPwlToFrame(pixels, 2, pwl);
    Expect(pixels[0] == (10u | 4u << 10 | 2u << 20 | 3u << 30), "frame: channels use their own component, alpha 3");
    Expect(pixels[1] == 3u << 30, "frame: black stays black, alpha 3");

    Expect(PwlDigest(def.pwl) == PwlDigest(DefaultRamp().pwl) && PwlDigest(def.pwl) != PwlDigest(d.ramp.pwl), "digest: stable, tells ramps apart");

    // FM4 Recomp (the table form of ApplyPwlToFrame): every 10-bit value of every channel against ApplyPwl10 itself, per pixel,
    // on two ramps (Xenia's default, and one whose three components differ in every entry), with the input alpha bits set to 1
    // (the output alpha is always 3). Red v, green 1023 - v, blue 7v mod 1024: each channel meets all 1024 values.
    PwlEntry mixed[128][3];
    for (uint32_t e = 0; e < 128; e++)
        for (uint32_t c = 0; c < 3; c++)
            mixed[e][c] = {uint16_t((e * 0x1C0u + c * 0x2C0u) & 0xFFC0u), uint16_t((0x40u * (1 + (e * 3 + c * 5) % 40)) & 0xFFC0u)};
    using Pwl = const PwlEntry (*)[128][3];
    for (Pwl ramp : {Pwl(&def.pwl), Pwl(&mixed)})
    {
        std::vector<uint32_t> frame(1024);
        for (uint32_t v = 0; v < 1024; v++)
            frame[v] = 1u << 30 | (v * 7 % 1024) << 20 | (1023 - v) << 10 | v;
        const std::vector<uint32_t> in = frame;
        ApplyPwlToFrame(frame.data(), frame.size(), *ramp);
        int wrong = 0;
        for (uint32_t i = 0; i < 1024; i++)
        {
            uint32_t want = 3u << 30;
            for (uint32_t c = 0; c < 3; c++)
            {
                const uint32_t v = (in[i] >> (10 * c)) & 0x3FF;
                want |= ApplyPwl10(v, (*ramp)[v >> 3][c]) << (10 * c);
            }
            wrong += frame[i] != want;
        }
        Expect(wrong == 0, "frame: every value of every channel equals ApplyPwl10");
    }
}
}  // namespace

int main()
{
    TestDefaults();
    TestStateRegisters();
    TestPwlWrites();
    TestTableWrites();
    TestApply();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
