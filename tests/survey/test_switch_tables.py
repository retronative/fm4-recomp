#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ForzaRecomp contributors
# Copyright (C) 2026 FM4 Recomp contributors
#
# Taken from ForzaRecomp tests/survey/test_switch_tables.py (GPL-3.0-or-later).
# FM4 Recomp changes: the script takes an explicit output path; the EXPERIMENTAL-extent cases
# are removed (patches 0029-0031 not imported); an unproven dispatch must have table_size 0;
# every unproven dispatch in a function blocks a function-level proof (new case
# 'unproven dispatch whose table avoids the region'); the output must be byte-identical across
# two runs. Instruction words are hand-encoded from the PowerPC ISA (Book I) field layouts below;
# every expected class and case address is worked out by hand from the words each case lays out.
"""Tests for survey/switch_tables.py on synthetic images (hand-encoded PowerPC, no game bytes).

    /opt/homebrew/bin/python3 -B -I tests/survey/test_switch_tables.py

Positive cases must be recognized with the right index register and case addresses; negative
controls (the bound not dominating the dispatch) must be UNPROVEN. Each check prints 'pass <name>'
or 'FAIL <name>' (tests/mutate.py matches the FAIL line); the last line is
'switch table tests: N failures'.
"""
import json, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = str(ROOT / 'survey/switch_tables.py')  # FM4 Recomp: mutants are planted by tests/mutate.py
BASE = 0x82000000


def lis(rd, imm): return 15 << 26 | rd << 21 | (imm & 0xFFFF)
def addi(rd, ra, imm): return 14 << 26 | rd << 21 | ra << 16 | (imm & 0xFFFF)
def slwi(ra, rs, n): return 21 << 26 | rs << 21 | ra << 16 | n << 11 | (31 - n) << 1
def xform(rt, ra, rb, xo): return 31 << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1
def lwzx(rt, ra, rb): return xform(rt, ra, rb, 23)
def lbzx(rt, ra, rb): return xform(rt, ra, rb, 87)
def add(rt, ra, rb): return xform(rt, ra, rb, 266)
def clrlwi(ra, rs, n): return 21 << 26 | rs << 21 | ra << 16 | n << 6 | 31 << 1  # rlwinm ra,rs,0,n,31
def lwz(rt, ra, d): return 32 << 26 | rt << 21 | ra << 16 | (d & 0xFFFF)
def stw(rs, ra, d): return 36 << 26 | rs << 21 | ra << 16 | (d & 0xFFFF)
def mr(ra, rs): return 31 << 26 | rs << 21 | ra << 16 | rs << 11 | 444 << 1
def mtctr(rs): return 0x7C0903A6 | rs << 21
def cmplwi(crf, ra, uimm): return 10 << 26 | crf << 23 | ra << 16 | uimm
def bc(bo, bi, disp): return 16 << 26 | bo << 21 | bi << 16 | (disp & 0xFFFC)
def bgt(crf, disp): return bc(12, 4 * crf + 1, disp)
def bne(crf, disp): return bc(4, 4 * crf + 2, disp)
def cmpwi(crf, ra, simm): return 11 << 26 | crf << 23 | ra << 16 | (simm & 0xFFFF)
def b(disp): return 18 << 26 | (disp & 0x3FFFFFC)
def bl(disp): return b(disp) | 1
BCTR, BLR, NOP = 0x4E800420, 0x4E800020, 0x60000000


def absolute(index=4, compared=4, extra=(), guard=True, prefix=(), bound=2):
    """<prefix>; cmplwi cr6,r<compared>,<bound>; bgt cr6,default; <extra>; table dispatch on
    r<index>; 3 cases, the table inline after the bctr."""
    code = list(prefix) + [cmplwi(6, compared, bound)] + ([bgt(6, 0)] if guard else [NOP]) + list(extra)
    at = len(prefix) + 1  # the bgt
    code += [lis(12, 0x8200), slwi(0, index, 2), None, lwzx(0, 12, 0), mtctr(0), BCTR]
    table = len(code)
    code[code.index(None)] = addi(12, 12, 4 * table)
    cases = [BASE + 4 * (table + 3 + i) for i in range(3)]
    code += cases + [BLR, BLR, BLR, BLR]
    if guard:
        code[at] = bgt(6, 4 * (len(code) - 1) - 4 * at)
    return code, cases


def byte_shifted():
    """cmplwi cr6,r11,1; bgt; byte table in data; case = base + byte << 2."""
    code = [cmplwi(6, 11, 1), bgt(6, 0), lis(12, 0x8200), addi(12, 12, 0x1000), lbzx(0, 12, 11),
            slwi(0, 0, 2), lis(12, 0x8200), None, add(12, 12, 0), mtctr(12), BCTR]
    label_base = len(code)
    code[code.index(None)] = addi(12, 12, 4 * label_base)
    code += [BLR, BLR, BLR]
    code[1] = bgt(6, 4 * (len(code) - 1) - 4)
    return code, [BASE + 4 * label_base + 4 * 0, BASE + 4 * label_base + 4 * 2], bytes([0, 2])


def nested(index=28, prefix=(), between=(), loop=(), after=(), outside=(), bound=2, cases1=('A', 'B', 'B')):
    """A function (one .pdata entry): <prefix>; cmplwi cr6,r<index>,<bound>; bgt cr6,default; switch 1
    on r<index> (cases <cases1>); A: li r3,1; b J; B: <between>; L: <loop>; J: switch 2 on r<index> (3 cases: X); X: blr; default: blr; <after>. Then
    <outside>, which is not in the function. Switch 2 is reached only through switch 1's cases.
    Each entry of the sequences may be a function of the addresses (a dict) to its word."""
    code, at = [], {}

    def emit(items):
        for item in items:
            code.append(item)
    emit(prefix)
    emit([cmplwi(6, index, bound), 'bgt'])
    at['d1'] = len(code)
    emit([lis(12, 0x8200), slwi(0, index, 2), 'addi1', lwzx(0, 12, 0), mtctr(0), BCTR])
    at['t1'] = len(code)
    emit(cases1)
    at['A'] = len(code)
    emit([addi(3, 0, 1), 'bJ'])
    at['B'] = len(code)
    emit(between)
    at['L'] = len(code)
    emit(loop)
    at['J'] = len(code)
    emit([lis(12, 0x8200), slwi(0, index, 2), 'addi2', lwzx(0, 12, 0), mtctr(0), BCTR])
    at['t2'] = len(code)
    emit(['X', 'X', 'X'])
    at['X'] = len(code)
    emit([BLR])
    at['default'] = len(code)
    emit([BLR])
    emit(after)
    at['end'] = len(code)
    emit(outside)
    addr = {k: BASE + 4 * v for k, v in at.items()}
    fixed = {'bgt': lambda i: bgt(6, 4 * (at['default'] - i)), 'addi1': lambda i: addi(12, 12, 4 * at['t1']),
             'addi2': lambda i: addi(12, 12, 4 * at['t2']), 'A': lambda i: addr['A'], 'B': lambda i: addr['B'],
             'X': lambda i: addr['X'], 'bJ': lambda i: b(4 * (at['J'] - i))}
    for i, item in enumerate(code):
        if isinstance(item, str):
            code[i] = fixed[item](i)
        elif callable(item):
            code[i] = item(addr, i)
    return code, addr, BASE + 4 * at['J'] + 20


def chained(compared=4):
    """FM4 Recomp (FM4's 0x82B90D5C shape): cmplwi cr6,r<compared>,2; bgt default; switch 1 on r4
    (table D, D, D); D: switch 2 on r4 (table X, X, X); X: blr; default: blr. D follows switch 1's
    table directly; its last word (0x820000.., primary opcode 32) decodes as lwz. Returns the code,
    switch 2's bctr and X."""
    code = [cmplwi(6, compared, 2), None]
    s1 = len(code)
    code += [lis(12, 0x8200), slwi(0, 4, 2), None, lwzx(0, 12, 0), mtctr(0), BCTR]
    t1 = len(code)
    code += [None] * 3
    d = len(code)
    code += [lis(12, 0x8200), slwi(0, 4, 2), None, lwzx(0, 12, 0), mtctr(0), BCTR]
    t2 = len(code)
    code += [None] * 3
    x = len(code)
    code += [BLR, BLR]
    code[1] = bgt(6, 4 * (x + 1 - 1))
    code[s1 + 2], code[d + 2] = addi(12, 12, 4 * t1), addi(12, 12, 4 * t2)
    code[t1:t1 + 3], code[t2:t2 + 3] = [BASE + 4 * d] * 3, [BASE + 4 * x] * 3
    return code, BASE + 4 * (d + 5), BASE + 4 * x


def edged(between=(), signed_le=False, compared=4, join=False):
    """FM4 Recomp (FM4's 0x824FB8FC shape), one .pdata function, the table in .rdata:
    cmpwi cr6,r<compared>,1; <between>; bne cr6,C; b D; C: cmplwi cr6,r4,1 (cmpwi if signed_le);
    bgt cr6,default; D: dispatch on r4 (table at .rdata: X0, X1); X0: blr; X1: blr; default: blr;
    [join: b to the bne]. D is entered by the fall-through of the bgt (r4 <= 1) and by 'b D',
    reached only by the fall-through of the bne (r4 == 1): 2 cases at function level."""
    code = [cmpwi(6, compared, 1)] + list(between) + [None, None]
    i_bne, i_b = len(code) - 2, len(code) - 1
    c = len(code)
    code += [cmpwi(6, 4, 1) if signed_le else cmplwi(6, 4, 1), None]
    d = len(code)
    code += [lis(12, 0x8200), slwi(0, 4, 2), addi(12, 12, 0x1000), lwzx(0, 12, 0), mtctr(0), BCTR]
    x0 = len(code)
    code += [BLR, BLR, BLR]
    if join:
        code += [b(4 * (i_bne - len(code)))]
    code[i_bne], code[i_b], code[c + 1] = bne(6, 4 * (c - i_bne)), b(4 * (d - i_b)), bgt(6, 4 * (x0 + 2 - c - 1))
    return code, struct.pack('>II', BASE + 4 * x0, BASE + 4 * x0 + 4), BASE + 4 * (d + 5)


def assemble(items):
    """items: words, label strings ('name:'), or callables (labels, index) -> word. Returns
    (code, {label: address})."""
    at, n = {}, 0
    for item in items:
        if isinstance(item, str):
            at[item[:-1]] = BASE + 4 * n
        else:
            n += 1
    code = []
    for item in items:
        if not isinstance(item, str):
            code.append(item(at, len(code)) if callable(item) else item)
    return code, at


def dispatch_on(reg, table):
    """Inline ABSOLUTE dispatch on r<reg>, its table at label <table>."""
    return [lis(12, 0x8200), slwi(0, reg, 2), lambda at, i: addi(12, 12, at[table] - BASE), lwzx(0, 12, 0), mtctr(0), BCTR]


def group_fn(s1_compared=4, r_prefix=(), after=(), x_prefix=(), t3_extra=0, edge_root=False):
    """FM4 Recomp (FM4's 0x82B4DA48 shape), one .pdata function: cmplwi cr6,r<s1_compared>,2; bgt
    default; S1 on r4 (table P, P, P); P: li r3,1; G1 on r4 (table R, R, R); R: <r_prefix>; G2 on r4
    (table X, X, X, plus t3_extra more X); X: <x_prefix>; blr; default: blr; <after>. G1 is entered
    by falling through from P (a case of S1); G2 only through G1's cases. Each blocks the other's
    function-level proof (blind). edge_root: no S1; 'b P' after the bgt instead (P is a join, the
    only bound is the bgt's edge, and no proven switch on r4 defines N)."""
    s1 = [lambda at, i: b(at['P'] - BASE - 4 * i)] if edge_root else dispatch_on(4, 'T1') + ['T1:'] + [lambda at, i: at['P']] * 3
    items = [cmplwi(6, s1_compared, 2), lambda at, i: bgt(6, at['default'] - BASE - 4 * i)] + s1 + \
        ['P:', addi(3, 0, 1)] + ['G1:'] + dispatch_on(4, 'T2') + \
        ['T2:'] + [lambda at, i: at['R']] * 3 + ['R:'] + list(r_prefix) + ['G2:'] + dispatch_on(4, 'T3') + \
        ['T3:'] + [lambda at, i: at['X']] * (3 + t3_extra) + ['X:'] + list(x_prefix) + [BLR, 'default:', BLR] + list(after) + ['end:']
    return assemble(items)


def run(code, data=b'', pdata=()):
    """pdata: (start, size) of each .pdata function."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        table = b''.join(struct.pack('>II', start, (size // 4) << 8) for start, size in pdata)
        image = b''.join(struct.pack('>I', w) for w in code).ljust(0x1000, b'\0') + data.ljust(0x100, b'\0') + table.ljust(0x100, b'\0')
        (tmp / 'image.bin').write_bytes(image)
        (tmp / 'report.json').write_text(json.dumps({'base': BASE, 'sections': [
            {'name': '.text', 'start': BASE, 'end': BASE + 0x1000, 'code': True},
            {'name': '.rdata', 'start': BASE + 0x1000, 'end': BASE + 0x1100, 'code': False},
            {'name': '.pdata', 'start': BASE + 0x1100, 'end': BASE + 0x1200, 'code': False}]}))
        out = tmp / 'out.toml'  # FM4 Recomp: explicit output path
        subprocess.run([sys.executable, '-B', '-I', SCRIPT, str(tmp), str(out)], check=True, capture_output=True)
        entries = json.loads((tmp / 'bctr.json').read_text())
        text = out.read_text()
        return entries, text


def main():
    failures = 0

    def expect(ok, name):
        nonlocal failures
        print(('pass ' if ok else 'FAIL ') + name)
        failures += not ok

    code, cases = absolute()
    entries, text = run(code)
    expect(entries[0]['class'] == 'SWITCH' and entries[0]['index'] == 4 and entries[0]['cases'] == 3
           and all(f'0x{c:08X}' in text for c in cases), 'absolute table: index r4, 3 cases at the table words')

    code, cases = absolute(index=28, compared=3, prefix=[mr(28, 3)])  # mr r28,r3 before cmplwi r3
    entries, text = run(code)
    expect(entries[0]['class'] == 'SWITCH' and entries[0]['index'] == 28, 'bound through a copy (mr r28,r3; cmplwi r3)')

    code, cases, table = byte_shifted()
    entries, text = run(code, table)
    expect(entries[0]['class'] == 'SWITCH' and entries[0]['shape'] == 'BYTE<<2' and all(f'0x{c:08X}' in text for c in cases),
           'byte table shifted by 2: cases at base + (byte << 2)')

    code, _ = absolute(compared=5, guard=False, extra=[clrlwi(4, 9, 31)])  # r4 = r9 & 1: 2 cases
    entries, text = run(code)
    expect(entries[0]['class'] == 'SWITCH' and entries[0]['cases'] == 2, 'index masked to 1 bit: 2 cases')

    # Negative controls: the bound must not be accepted.
    code, _ = absolute(compared=5, guard=False, extra=[clrlwi(4, 9, 31), addi(4, 4, 1)])
    expect(run(code)[0][0]['class'] == 'UNPROVEN', 'masked index written again: unproven')
    code, _ = absolute(extra=[addi(4, 4, 1)])
    expect(run(code)[0][0]['class'] == 'UNPROVEN', 'index written after the compare: unproven')
    code, _ = absolute(compared=5)
    expect(run(code)[0][0]['class'] == 'UNPROVEN', 'compare on another register: unproven')
    mtcrf_cr6 = 31 << 26 | 9 << 21 | 0x02 << 12 | 144 << 1  # mtcrf 0x02,r9: cr6 from r9
    code, _ = absolute(guard=False, extra=[mtcrf_cr6, bgt(6, 0x40)])  # the bgt tests the new cr6
    expect(run(code)[0][0]['class'] == 'UNPROVEN', 'compared field overwritten (mtcrf) before the branch: unproven')
    code, _ = absolute(guard=False)
    expect(run(code)[0][0]['class'] == 'UNPROVEN', 'no branch to the default: unproven')
    code, _ = absolute()
    dispatch = 2
    code += [b(4 * dispatch - 4 * len(code))]  # an unguarded branch into the dispatch
    expect(run(code)[0][0]['class'] == 'UNPROVEN', 'unguarded branch into the dispatch: unproven')
    entries, text = run(absolute(compared=5)[0])
    expect('labels = [\n]' in text, 'unproven dispatch listed with no labels (the recompiler rejects it)')
    # FM4 Recomp: the unproven inline table is named (patch 0028 keeps its address from becoming
    # an entry) but is not a data range (no extent is claimed without a proof); no other keys.
    # By hand: absolute(compared=5) lays out cmplwi, bgt, lis, slwi, addi, lwzx, mtctr at words
    # 0-6 and the bctr at word 7 (0x8200001C); the table follows at 0x82000020; the compare is on
    # r5, the index r4, so the bound is unproven.
    entries, text = run(absolute(compared=5)[0])
    expect('base = 0x8200001C\nr = 4\ntable = 0x82000020\ntable_size = 0\nlabels = [\n]\n' in text
           and 'experimental' not in text,
           'unproven inline table: table named, table_size 0, no labels, no experimental keys')
    code, _ = absolute()
    first = run(code)[1]
    expect(first == run(code)[1] and 'Generated by survey/switch_tables.py' in first,
           'output is byte-identical across two runs')

    # Function-level bound: switch 2 is reached only through the cases of switch 1 (bounded).
    def nested_result(data=b'', pdata=True, **kw):
        code, addr, bctr2 = nested(**kw)
        entries, text = run(code, data, [(BASE, addr['end'] - BASE)] if pdata else [])
        return next(e for e in entries if e['bctr'] == f'{bctr2:08X}'), text, addr

    def unproven(name, why, **kw):
        entry = nested_result(**kw)[0]
        expect(entry['class'] == 'UNPROVEN' and why in entry['reason'], f'{name}: unproven ({entry.get("reason")})')

    entry, text, addr = nested_result()
    expect(entry['class'] == 'SWITCH' and entry.get('rule') == 'function' and entry['cases'] == 3 and entry['index'] == 28
           and text.count(f'0x{addr["X"]:08X}') == 3, 'reached only through a bounded switch\'s cases: 3 cases, function level')
    call = lambda a, i: bl(4 * ((a['default'] - BASE) // 4 - i))
    entry = nested_result(between=[call])[0]
    expect(entry['class'] == 'SWITCH' and entry['calls_crossed'] == [f'{addr["B"]:08X}'],
           'a call between the switches preserves r28 (ABI, recorded as crossed)')
    # L: addi r4,r4,-1; beq cr0,J; addi r<n>,r<n>,1; b L: the write is only on the back edge.
    def loop(n):
        return [addi(4, 4, -1), lambda a, i: bc(12, 2, a['J'] - BASE - 4 * i), addi(n, n, 1), lambda a, i: b(a['L'] - BASE - 4 * i)]
    entry = nested_result(loop=loop(5))[0]
    expect(entry['class'] == 'SWITCH', 'a loop whose back edge writes another register: proven')
    unproven('a call may clobber a volatile index (r11)', 'writes the index', index=11, between=[call])
    unproven('the index written again on one path (case B)', 'writes the index', between=[addi(28, 28, 1)])
    unproven('the index written on a loop back edge', 'writes the index', loop=loop(28))
    unproven('the bound (4 cases) exceeds the inline table (3 words)', 'does not have 4 entries', bound=3, cases1=('A', 'B', 'B', 'B'))
    unproven('no .pdata function', 'not in exactly one .pdata function', pdata=False)
    unproven('a branch from before the compare into the region', 'function entry',
             prefix=[lambda a, i: bc(12, 2, a['J'] - BASE - 4 * i)])
    unproven('a branch from outside the function into the region', 'outside the function',
             outside=[lambda a, i: b(a['J'] - BASE - 4 * i)])
    unproven('a call into the region', 'is called from', outside=[lambda a, i: bl(a['B'] + 4 - BASE - 4 * i)],
             between=[addi(3, 0, 2), addi(3, 3, 1)])
    unproven('a code address of the region stored in data', 'pointer or a .pdata start',
             data=struct.pack('>I', nested()[1]['B']))
    # An unproven dispatch (on r5, no bound) in the same function: its targets are unknown, so the
    # function-level proof is refused whatever its table holds. FM4 Recomp: ForzaRecomp followed
    # the derivable cases of [Y, B] (B in the region: refused) and would have proven the region
    # for [Y, Y] (no case in it); here all three are refused.
    def other_dispatch(words):
        return [lis(12, 0x8200), slwi(0, 5, 2), lambda a, i: addi(12, 12, 4 * (i + 4)), lwzx(0, 12, 0), mtctr(0), BCTR] + \
            [lambda a, i, w=w: (a['B'] if w == 'B' else BASE + 4 * (i + 2 - words.index(w))) for w in words] + [BLR]
    unproven('an unproven dispatch with a case in the region', 'unknown targets', after=other_dispatch(['Y', 'B']))
    unproven('an unproven dispatch with unknown targets in the function', 'unknown targets', after=other_dispatch(['B', 'B']))
    unproven('an unproven dispatch whose table avoids the region (FM4 Recomp)', 'unknown targets', after=other_dispatch(['Y', 'Y']))

    # FM4 Recomp: a proven table's word is data, never a fall-through into the next dispatch.
    code, bctr2, x = chained()
    entries, text = run(code)
    e = next(e for e in entries if e['bctr'] == f'{bctr2:08X}')
    expect(e['class'] == 'SWITCH' and e['cases'] == 3 and e.get('rule') is None and text.count(f'0x{x:08X}') == 3,
           'a dispatch after a proven table, reached only through its cases: 3 cases, local')
    code, bctr2, x = chained(compared=5)
    e = next(e for e in run(code)[0] if e['bctr'] == f'{bctr2:08X}')
    expect(e['class'] == 'UNPROVEN' and 'fall-through path has no bound' in e['reason'],
           'the same after an unproven table: its last word still counts as a fall-through')

    # FM4 Recomp: the straight-line stretch reaches back to the function start (FM4's 0x82ADF294:
    # the compare 66 instructions before the dispatch); 20 instructions here, past the old 16.
    long = [addi(5, 5, 1)] * 20
    code, _ = absolute(extra=long)
    e = run(code, pdata=[(BASE, 4 * len(code))])[0][0]
    expect(e['class'] == 'SWITCH' and e.get('rule') is None,
           'a compare 22 instructions before the dispatch, function start known: proven locally (no function rule)')
    expect(run(code)[0][0]['class'] == 'UNPROVEN', 'the same with no .pdata function: 16 instructions only, unproven')
    # Joins end the stretch: an address taken in code (lis/addi) inside it, or a proven switch's case.
    middle = BASE + 4 * 12  # word 12: inside the 20 addi words (words 2-21)
    code, _ = absolute(extra=long)
    code += [lis(3, 0x8200), addi(3, 3, middle - BASE)]
    e = run(code, pdata=[(BASE, 4 * len(code))])[0][0]
    expect(e['class'] == 'UNPROVEN' and 'fall-through path has no bound' in e['reason'],
           'an address-taken word inside the stretch is a join: unproven')
    code, _ = absolute(extra=long)
    k = len(code)  # a switch on r6 with one case, to the middle of the stretch
    code += [cmplwi(6, 6, 0), bgt(6, 4 * 9), lis(12, 0x8200), slwi(0, 6, 2), addi(12, 12, 4 * (k + 8)), lwzx(0, 12, 0),
             mtctr(0), BCTR, middle, BLR, BLR]
    entries = run(code, pdata=[(BASE, 4 * len(code))])[0]
    expect(entries[1]['class'] == 'SWITCH' and entries[0]['class'] == 'UNPROVEN'
           and 'fall-through path has no bound' in entries[0]['reason'],
           'a proven switch case inside the stretch is a join: unproven')

    # FM4 Recomp: the compared value reloaded from the same stack slot (FM4's 0x82AE79A8).
    def reload(extra):
        code, _ = absolute(compared=9, prefix=[lwz(9, 1, 112)], extra=extra)
        return run(code)[0][0]
    expect(reload([lwz(4, 1, 112)])['class'] == 'SWITCH', 'lwz r9,112(r1); cmplwi r9; bgt; lwz r4,112(r1): proven')
    expect(reload([lwz(4, 1, 116)])['class'] == 'UNPROVEN', 'a reload of another stack slot: unproven')
    expect(reload([stw(5, 3, 0), lwz(4, 1, 112)])['class'] == 'UNPROVEN', 'a store between the loads: unproven')
    expect(reload([addi(1, 1, 16), lwz(4, 1, 112)])['class'] == 'UNPROVEN', 'r1 written between the loads: unproven')
    expect(reload([lwz(4, 3, 112)])['class'] == 'UNPROVEN', 'the same offset from another base register: unproven')

    # FM4 Recomp: an edge of a conditional branch bounds the index (function level, any table shape).
    def edge(**kw):
        code, data, bctr = edged(**kw)
        return next(e for e in run(code, data, [(BASE, 4 * len(code))])[0] if e['bctr'] == f'{bctr:08X}')
    e = edge()
    expect(e['class'] == 'SWITCH' and e['cases'] == 2 and e.get('rule') == 'function'
           and e['bounded_by'] == ['82000004', '82000010'],
           'bounded by the equal edge of bne (r4 == 1) and the not-greater edge of bgt (r4 <= 1): 2 cases')
    expect(edge(signed_le=True)['class'] == 'UNPROVEN', 'a signed compare does not bound the not-greater edge: unproven')
    expect(edge(between=[addi(4, 4, 1)])['class'] == 'UNPROVEN', 'the index written between compare and bne: unproven')
    expect(edge(compared=5)['class'] == 'UNPROVEN', 'the equality compare on another register: unproven')
    expect(edge(join=True)['class'] == 'UNPROVEN', 'a branch into the bne bypasses its compare: unproven')
    mtcrf_cr6 = 31 << 26 | 9 << 21 | 0x02 << 12 | 144 << 1
    expect(edge(between=[mtcrf_cr6])['class'] == 'UNPROVEN', 'cr6 rewritten between compare and bne: unproven')

    # FM4 Recomp: joint proof of all the unproven dispatches of one function (prove_groups).
    def group(**kw):
        code, at = group_fn(**kw)
        entries = {e['bctr']: e for e in run(code, pdata=[(BASE, at['end'] - BASE)])[0]}
        return [entries[f'{at[g] + 20:08X}'] for g in ('G1', 'G2')], at
    (g1, g2), at = group()
    # bounded_by by hand: each bctr is the word before its table (S1 0x8200001C, G1 0x82000044).
    expect(g1['class'] == g2['class'] == 'SWITCH' and g1['cases'] == g2['cases'] == 3 and g1.get('rule') == g2.get('rule') == 'group'
           and g1['bounded_by'] == [f'{at["T1"] - 4:08X}'] and g2['bounded_by'] == [f'{at["G1"] + 20:08X}'],
           'two dispatches blocking each other, rooted in a proven switch: proven together, 3 cases each')
    (g1, g2), _ = group(s1_compared=5)
    expect(g1['class'] == g2['class'] == 'UNPROVEN', 'the root switch itself unproven: unproven')
    try:
        (g1, g2), _ = group(edge_root=True)
        settled = True
    except subprocess.CalledProcessError:
        settled, g1, g2 = False, {'class': None}, {'class': None}
    expect(settled, 'the passes settle when two dispatches could prove each other in turn (no optimistic first pass)')
    expect(g1['class'] == g2['class'] == 'UNPROVEN', 'bounded only by a compare edge, no proven switch on the index (N undefined): unproven')
    (g1, g2), _ = group(r_prefix=[addi(4, 4, 1)])
    expect(g1['class'] == g2['class'] == 'UNPROVEN', 'one member not provable (index written): none accepted')
    # A dispatch on r5 at X, reached only through G2's cases: proving it with r4's bound would be wrong.
    (g1, g2), _ = group(x_prefix=dispatch_on(5, 'T4') + ['T4:'] + [lambda at, i: at['Y']] * 3 + ['Y:'])
    expect(g1['class'] == g2['class'] == 'UNPROVEN', 'another unproven dispatch on another register in the function: unproven')
    four = [cmplwi(6, 4, 3), lambda at, i: bgt(6, at['default'] - BASE - 4 * i)] + dispatch_on(4, 'T5') + ['T5:'] + [lambda at, i: at['X']] * 4
    (g1, g2), _ = group(after=four)
    expect(g1['class'] == g2['class'] == 'UNPROVEN', 'proven switches on the index disagree on the count (3, 4): unproven')
    (g1, g2), _ = group(t3_extra=1)
    expect(g1['class'] == g2['class'] == 'UNPROVEN', "a member's inline table has 4 words for 3 cases (extent check): unproven")

    print(f'switch table tests: {failures} failures')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
