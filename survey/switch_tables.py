#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ForzaRecomp contributors
# Copyright (C) 2026 FM4 Recomp contributors
#
# Taken from ForzaRecomp survey/switch_tables.py (GPL-3.0-or-later, same owner).
# FM4 Recomp changes (each marked "FM4 Recomp:" below):
#   - command line: the switch TOML goes to an explicit output path (config/switch_tables.toml,
#     committed: addresses only), bctr.json stays in the git-ignored survey directory; report.json
#     comes from tools/survey/xexsurvey (survey/xexsurvey.cpp), not ForzaRecomp's xex_report;
#   - no EXPERIMENTAL dispatch (ForzaRecomp patches 0029-0031 are not imported): no
#     experimental_labels, no partial bound, no experimental extent in bctr.json; an unproven
#     dispatch is written with labels = [] and table_size = 0, which patch 0025 makes FATAL
#     ("switch table at X has no proven bound");
#   - prove_region treats every unproven dispatch as one with unknown targets (ForzaRecomp let the
#     EXPERIMENTAL cases of a dispatch with a derivable extent be followed; that was sound only
#     relative to patch 0030's translated control flow);
#   - experimental_extent is reduced to inline_table_extent, which only checks that a
#     function-level bound agrees with the inline table's own extent (a proof consistency check).
"""Recognize FM4's switch tables (M1) and classify every bctr.

    /opt/homebrew/bin/python3 -I survey/switch_tables.py <survey dir> <switch toml>

Reads <survey dir>/image.bin and report.json (from tools/survey/xexsurvey); writes <switch toml>
(XenonRecomp's [[switch]] format: base = the bctr, r = the index register, labels = case
addresses, plus table and table_size, the table's address and bytes; an unproven dispatch has no
labels and table_size 0) and <survey dir>/bctr.json (every bctr, classified). The output depends
only on the image: no time, path or host in it (two runs must be byte-identical).

A bctr is a SWITCH when the code before it computes CTR from a table indexed by a register rI,
in one of the shapes MSVC emits for FM4:
  ABSOLUTE   lis rT,hi; slwi rX,rI,2; addi rT,rT,lo; lwzx rY,rT,rX; mtctr rY
             case i = the word at table + 4i
  BYTE       lis rT,hi; addi rT,rT,lo; lbzx r0,rT,rI; [slwi r0,r0,2;] lis rB,hi; addi rB,rB,lo;
             add rB,rB,r0; mtctr rB          case i = base + (byte i << shift)
  HALF       lis rT,hi; slwi r0,rI,1; addi rT,rT,lo; lhzx r0,rT,r0; lis rB,hi; addi rB,rB,lo;
             add rB,rB,r0; mtctr rB          case i = base + halfword i
(nop may appear between them), and its bound is PROVEN: no direct branch in the image enters the
dispatch block after its first instruction, the block does not write rI, and every path into the
block passes the same 'cmplwi crN,rI,max': falling through after a branch-if-greater on crN to
the default (or after masking rI: rlwinm rI,r,0,MB,31 or andi. rI,r,max), or branching in with a
branch-if-not-greater on crN right after the compare. The switch then has max + 1 cases.
A bound is also PROVEN at function level (prove_region): every way into the dispatch, followed
back through its .pdata function, starts at a case of a proven switch on rI (max = its cases - 1),
and nothing on the way writes rI. A call is assumed to preserve r14-r31 (REFERENCE_DERIVED: the
PowerPC ABI's non-volatile registers, which FM4's own __savegprlr_14/__restgprlr_14 helpers save);
were the assumption wrong, rI past max reaches the translated switch's trap, never a wrong case.
FM4 Recomp: a function that contains any unproven dispatch is never proven at function level
(its targets are unknown). It applies only to an inline ABSOLUTE table, whose own extent
(inline_table_extent) must equal max + 1.
A table-shaped bctr whose bound is not proven is UNPROVEN: it is listed with no labels and
table_size 0, which the recompiler rejects (patch 0025: FATAL "no proven bound"); its table is
still named so that patch 0028 never makes the table's address an entry. PROVEN means proven for
the instruction forms modelled here (writes(), cr_fields_written()).
Any other bctr is INDIRECT: a branch to CTR, translated as an indirect call (through a function
pointer), with the source of CTR recorded when it is visible nearby.
"""
import json, struct, sys
from pathlib import Path

BCTR, NOP, MTCTR_MASK = 0x4E800420, 0x60000000, 0xFC1FFFFF
WINDOW = 24  # instructions searched back from the bctr


def fields(w):
    return w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 0xFFFF


def simm(w):
    return (w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF


def xo(w):
    return (w >> 1) & 0x3FF


def writes(w):
    """GPRs an instruction may write (conservative: unknown forms count as writing rT)."""
    op, rt, ra, _, _ = fields(w)
    if op in (14, 15, 32, 34, 40, 42, 7, 8, 12, 13):  # addi addis lwz lbz lhz lha mulli subfic addic(.)
        return {rt}
    if op in (33, 35, 41, 43):  # lwzu lbzu lhzu lhau
        return {rt, ra}
    if op in (37, 39, 45, 49, 51, 53, 55):  # stwu stbu sthu lfsu lfdu stfsu stfdu: rA = EA
        return {ra}
    if op == 46:  # lmw: rT..r31
        return set(range(rt, 32))
    if op == 58:  # ld, ldu (also rA), lwa
        return {rt, ra} if w & 3 == 1 else {rt}
    if op == 62:  # std, stdu (rA)
        return {ra} if w & 3 == 1 else set()
    if op in (24, 25, 26, 27, 28, 29, 20, 21, 23, 30):  # ori oris xori xoris andi andis rlwimi rlwinm rlwnm rld*
        return {ra}
    if op == 31:
        x = xo(w)
        if x in (24, 26, 28, 60, 124, 284, 316, 412, 444, 476, 536, 792, 824, 922, 954, 986, 27, 539, 794):  # logical/shift/ext forms
            return {ra}
        if x in (533, 597):  # lswx lswi: rT and on, wrapping to r0
            return set(range(32))
        if x in (55, 119, 311, 375, 53, 373):  # lwzux lbzux lhzux lhaux ldux lwaux
            return {rt, ra}
        if x in (183, 247, 439, 181, 695, 759, 567, 631):  # stwux stbux sthux stdux stfsux stfdux lfsux lfdux
            return {ra}
        if x in (339, 371, 19, 83):  # mfspr mftb mfcr mfmsr
            return {rt}
        if x in (467, 144, 146, 210, 242, 595, 659, 151, 215, 407, 149, 663, 727, 150, 214):  # mt* and stores
            return set()
        return {rt}
    if op == 18 or op == 16 or op == 19:
        return set() if not (w & 1) else set(range(0, 13))  # a call clobbers volatile registers
    if op == 17:  # sc: not modelled
        return set(range(32))
    return set()


def falls_through(w):
    """False for an unconditional transfer that does not return: b, blr, bctr, and bc/bclr/bcctr
    with BO 'branch always' (a call returns, so it falls through)."""
    op = w >> 26
    return not ((op == 18 and not w & 1) or w in (0x4E800020, BCTR) or
                ((op == 16 or (op == 19 and xo(w) in (16, 528))) and (w >> 21) & 0x14 == 0x14 and not w & 1))


def cr_fields_written(w):
    """CR fields an instruction may write: compares, record forms (cr0, or cr1 for FP), mtcrf,
    mcrf and CR-logical operations; a call clobbers the volatile fields."""
    op = w >> 26
    if op in (10, 11) or (op == 31 and xo(w) in (0, 32)) or (op == 63 and xo(w) in (0, 32)):
        return {(w >> 23) & 7}
    if op == 31 and xo(w) == 144:  # mtcrf FXM
        return {7 - i for i in range(8) if (w >> 12) & (1 << i)}
    if op == 19 and xo(w) == 0:  # mcrf
        return {(w >> 23) & 7}
    if op == 19 and xo(w) in (33, 129, 193, 225, 257, 289, 417, 449):  # CR logical: bit BT
        return {(w >> 23) & 7}
    if op in (16, 18, 19) and w & 1:
        return {0, 1, 5, 6, 7}
    if (op in (28, 29, 13) or (op in (20, 21, 23, 30, 31) and w & 1)):
        return {0}
    if op in (59, 63) and w & 1:
        return {1}
    return set()


def is_branch_if(w, bo, field_bit):
    """A conditional branch (bc or bclr, not a call) with BO in `bo` on CR bit `field_bit`."""
    op = w >> 26
    return (op == 16 or (op == 19 and xo(w) == 16)) and not w & 1 and (w >> 21) & 31 in bo and (w >> 16) & 31 == field_bit


def main():
    # FM4 Recomp: explicit output path for the switch TOML; no default.
    if len(sys.argv) != 3:
        sys.exit('usage: switch_tables.py <survey dir> <switch toml>')
    survey, output = Path(sys.argv[1]), Path(sys.argv[2])
    report = json.loads((survey / 'report.json').read_text())
    image = (survey / 'image.bin').read_bytes()
    base = report['base']

    def word(a):
        return struct.unpack_from('>I', image, a - base)[0]

    code = [(s['start'], s['end']) for s in report['sections'] if s['code']]

    def in_code(a):
        return any(s <= a < e for s, e in code)

    # Every direct branch target in the code: a bound only dominates a dispatch no branch enters.
    sources = {}  # direct branch target -> branch addresses
    for start, end in code:
        for a in range(start, end, 4):
            w = word(a)
            op = w >> 26
            if op == 18:
                d = w & 0x3FFFFFC
                d = d - 0x4000000 if d & 0x2000000 else d
            elif op == 16:
                d = w & 0xFFFC
                d = d - 0x10000 if d & 0x8000 else d
            else:
                continue
            sources.setdefault(d if w & 2 else (a + d) & 0xFFFFFFFF, []).append(a)

    functions, entries = pdata(report, image)
    entries |= address_taken(report, image, in_code)

    def function_of(a):
        """The one .pdata function that contains a, or None."""
        hits = [f for f in functions if f[0] <= a < f[1]]
        return hits[0] if len(hits) == 1 else None

    candidates = []
    for start, end in code:
        for bctr in range(start, end, 4):
            if word(bctr) != BCTR:
                continue
            seq = []  # (address, word), nearest first, nops skipped
            a = bctr - 4
            while len(seq) < WINDOW and a >= start:
                if word(a) != NOP:
                    seq.append((a, word(a)))
                a -= 4
            candidates.append((bctr, seq))

    # A dispatch can be a case of another switch (nested switches on one index), so proofs are
    # repeated until no new switch is proven; case edges of proven switches count as paths.
    proven, cases = {}, {}
    # What the previous pass left: unproven dispatches (FM4 Recomp: all of them have unknown
    # targets) and the words of proven tables. FM4 Recomp: before the first pass every table
    # dispatch is unproven (ForzaRecomp started with none blind: an optimistic first pass let two
    # dispatches prove each other in turn and the passes never settled).
    blind = {bctr for bctr, seq in candidates if recognize(seq) is not None}
    table_words = set()
    grouped = {}  # FM4 Recomp: dispatches proven by prove_groups
    for _ in range(64):
        switches, classified, unproven = [], [], {}
        for bctr, seq in candidates:
            entry = {'bctr': f'{bctr:08X}'}
            result = recognize(seq)
            if result is None:
                entry.update(classify_other(seq))
            else:
                index, shape, dispatch, case, table, size = result
                if bctr in grouped:
                    # FM4 Recomp: proven jointly with the other unproven dispatches of its function.
                    count, reason = grouped[bctr]['cases'], None
                    entry.update({k: v for k, v in grouped[bctr].items() if k != 'cases'})
                else:
                    count, reason = prove(word, sources, cases, dispatch, bctr, index, table_words, entries,
                                          (function_of(bctr) or (None,))[0])
                    if not count:
                        # FM4 Recomp: for every table shape (FM4's 0x824FB8FC is a byte table in .rdata);
                        # the inline-extent comparison is a consistency check for inline tables only.
                        known = (entries, blind, table_words)
                        region, why, bounded_by, calls = prove_region(word, sources, cases, known, function_of(bctr), dispatch, bctr, index)
                        inline = shape == 'ABSOLUTE' and table == bctr + 4
                        if region and inline and inline_table_extent(word, in_code, bctr + 4, region - 1)[0] != region:
                            region, why = 0, f'the inline table does not have {region} entries'
                        if region:
                            count, reason = region, None
                            entry['rule'] = 'function'
                            entry['bounded_by'] = [f'{b:08X}' for b in bounded_by]
                            entry['calls_crossed'] = [f'{c:08X}' for c in calls]  # r14-r31 assumed preserved
                        else:
                            reason += f'; function level: {why}'
                labels = [case(word, i) for i in range(count)] if count else []
                if count and not all(in_code(x) for x in labels):
                    count, reason = 0, 'a case address is outside the code'
                if not count:
                    # FM4 Recomp: no labels and no data range; patch 0025 stops on it (FATAL).
                    entry.update({'class': 'UNPROVEN', 'shape': shape, 'reason': reason})
                    switches.append((bctr, index, [], table, 0))
                    unproven[bctr] = (index, shape, dispatch, case, table)
                else:
                    entry.update({'class': 'SWITCH', 'shape': shape, 'index': index, 'cases': count})
                    switches.append((bctr, index, labels, table, count * size))
            classified.append(entry)
        if {x[0] for x in switches if x[2]} == set(proven):
            found = prove_groups(word, in_code, sources, cases, entries, table_words, proven, unproven, function_of)
            if not found:
                break
            grouped.update(found)
            continue
        proven = {bctr: (index, labels) for bctr, index, labels, _, _ in switches if labels}
        cases = {}
        for bctr, (index, labels) in proven.items():
            for label in labels:
                cases.setdefault(label, []).append((bctr, index, len(labels)))
        blind = {x[0] for x in switches if not x[2]}
        table_words = {a for x in switches for a in range(x[3], x[3] + x[4], 4)}
    else:
        sys.exit('switch_tables: the proofs do not settle')

    lines = ['# Generated by survey/switch_tables.py from the image; do not edit.\n']
    # An unproven dispatch is listed with no labels: the recompiler rejects it (patch 0025).
    for bctr, index, labels, table, table_size in switches:
        lines.append(f'\n[[switch]]\nbase = 0x{bctr:08X}\nr = {index}\ntable = 0x{table:08X}\ntable_size = {table_size}\nlabels = [\n')
        lines += [f'    0x{label:08X},\n' for label in labels]
        lines.append(']\n')
    output.write_text(''.join(lines))
    (survey / 'bctr.json').write_text(json.dumps(classified, indent=1) + '\n')
    counts = {}
    for entry in classified:
        counts[entry['class']] = counts.get(entry['class'], 0) + 1
    print('switch_tables:', ', '.join(f'{k} {v}' for k, v in sorted(counts.items())))


def recognize(seq):
    """Match a table dispatch ending in mtctr; return (index register, shape, address of its first
    instruction, case(word, i) -> address of case i, table address, entry size)."""
    if not seq or (seq[0][1] & MTCTR_MASK) != 0x7C0903A6:
        return None
    ctr_src = (seq[0][1] >> 21) & 31
    rest = seq[1:]

    def find(pred, after=0):
        for i in range(after, len(rest)):
            if pred(rest[i][1]):
                return i
        return None

    i_add = find(lambda w: w >> 26 == 31 and xo(w) == 266 and (w >> 21) & 31 == ctr_src)
    if i_add is not None:  # BYTE or HALF: ctr = base + offset
        _, _, rb_base, r_off, _ = fields(rest[i_add][1])
        i_base_lo = find(lambda w: w >> 26 == 14 and (w >> 21) & 31 == rb_base and (w >> 16) & 31 == rb_base, i_add + 1)
        i_base_hi = find(lambda w: w >> 26 == 15 and (w >> 21) & 31 == rb_base and (w >> 16) & 31 == 0, i_add + 1)
        if i_base_lo is None or i_base_hi is None:
            return None
        label_base = ((rest[i_base_hi][1] & 0xFFFF) << 16) + simm(rest[i_base_lo][1]) & 0xFFFFFFFF
        shift = 0
        i_load = i_base_hi
        i_shift = find(lambda w: w >> 26 == 21 and (w >> 21) & 31 == r_off and (w >> 16) & 31 == r_off and (w >> 6) & 31 == 0 and (w >> 1) & 31 == 31 - ((w >> 11) & 31), i_add + 1)
        i_ld = find(lambda w: w >> 26 == 31 and xo(w) in (87, 279) and (w >> 21) & 31 == r_off, i_add + 1)
        if i_ld is None:
            return None
        if i_shift is not None and i_shift < i_ld:
            shift = (rest[i_shift][1] >> 11) & 31
        _, _, r_tab, r_idx, _ = fields(rest[i_ld][1])
        size = 1 if xo(rest[i_ld][1]) == 87 else 2
        shape = 'BYTE' if size == 1 else 'HALF'
        i_tab_lo = find(lambda w: w >> 26 == 14 and (w >> 21) & 31 == r_tab and (w >> 16) & 31 == r_tab, i_ld + 1)
        i_tab_hi = find(lambda w: w >> 26 == 15 and (w >> 21) & 31 == r_tab and (w >> 16) & 31 == 0, i_ld + 1)
        if i_tab_lo is None or i_tab_hi is None:
            return None
        table = ((rest[i_tab_hi][1] & 0xFFFF) << 16) + simm(rest[i_tab_lo][1]) & 0xFFFFFFFF
        index, last = r_idx, max(i_tab_lo, i_tab_hi)
        if size == 2:  # the index register is scaled: slwi r_idx, rI, 1
            i_scale = find(lambda w: w >> 26 == 21 and (w >> 16) & 31 == r_idx and (w >> 11) & 31 == 1 and (w >> 1) & 31 == 30 and (w >> 6) & 31 == 0, i_ld + 1)
            if i_scale is None:
                return None
            index, last = (rest[i_scale][1] >> 21) & 31, max(last, i_scale)
        first = min(rest[j][0] for j in range(0, last + 1))
        return (index, f'{shape}<<{shift}' if shift else shape, first,
                lambda word, i: (label_base + (image_read(word, table + i * size, size) << shift)) & 0xFFFFFFFF, table, size)

    i_ld = find(lambda w: w >> 26 == 31 and xo(w) == 23 and (w >> 21) & 31 == ctr_src)
    if i_ld is None:
        return None
    _, _, r_tab, r_scaled, _ = fields(rest[i_ld][1])
    i_tab_lo = find(lambda w: w >> 26 == 14 and (w >> 21) & 31 == r_tab and (w >> 16) & 31 == r_tab, i_ld + 1)
    i_tab_hi = find(lambda w: w >> 26 == 15 and (w >> 21) & 31 == r_tab and (w >> 16) & 31 == 0, i_ld + 1)
    i_scale = find(lambda w: w >> 26 == 21 and (w >> 16) & 31 == r_scaled and (w >> 11) & 31 == 2 and (w >> 1) & 31 == 29 and (w >> 6) & 31 == 0, i_ld + 1)
    if None in (i_tab_lo, i_tab_hi, i_scale):
        return None
    table = ((rest[i_tab_hi][1] & 0xFFFF) << 16) + simm(rest[i_tab_lo][1]) & 0xFFFFFFFF
    index = (rest[i_scale][1] >> 21) & 31
    last = max(i_tab_lo, i_tab_hi, i_scale)
    first = min(rest[j][0] for j in range(0, last + 1))
    return index, 'ABSOLUTE', first, lambda word, i: word(table + 4 * i), table, 4


def image_read(word, address, size):
    w = word(address & ~3)
    shift = (4 - size - (address & 3)) * 8
    return (w >> shift) & ((1 << (8 * size)) - 1)


def keeps_memory(w):
    """FM4 Recomp: True for the few instructions known not to write memory (compares, conditional
    branches, immediate arithmetic and logic, rlwinm, non-update loads, mr/add/subf and indexed
    loads); everything else may write memory."""
    op = w >> 26
    if op in (10, 11, 14, 15, 16, 21, 24, 28, 32, 34, 40):
        return True
    return op == 31 and xo(w) in (0, 32, 444, 266, 40, 23, 87, 279)


def is_cmplwi(w, index):
    """cmplwi crN, index, max (L = 0) -> (N, max), else None."""
    if w >> 26 == 10 and (w >> 16) & 31 == index and not (w >> 21) & 1:
        return (w >> 23) & 7, w & 0xFFFF
    return None


def prove(word, sources, cases, dispatch, bctr, index, table_words=frozenset(), entries=frozenset(), start=None):
    """Prove every path into the dispatch block [dispatch, bctr] passes one bound on index.
    Returns (max + 1, None) or (0, reason). FM4 Recomp: no partial bound (it only fed the
    EXPERIMENTAL extent). FM4 Recomp: a word of a proven table (table_words) is data, so nothing falls
    through from it (as prove_region); the straight-line stretch before the dispatch reaches back to
    the function's start (start, when known; else 16 instructions) and ends at every join: a branch
    target, a case of a proven switch, an entry (entries)."""
    failure = None
    for a in range(dispatch + 4, bctr + 4, 4):
        if a in sources:
            failure = f'branch into the dispatch at {a:08X}'
            break
    if failure is None and any(a in cases for a in range(dispatch + 4, bctr + 4, 4)):
        failure = 'a switch case enters the dispatch'
    if any(index in writes(word(a)) for a in range(dispatch, bctr, 4)):
        return 0, 'the dispatch writes the index'
    bounds = set()
    # Fall-through into the dispatch: walk back to 'cmplwi crN,index,max; ... bgt crN,default'.
    previous = dispatch - 4
    while word(previous) == NOP:
        previous -= 4
    w = word(previous)
    if falls_through(w) and previous not in table_words:
        # The straight-line stretch before the dispatch: back to another branch or call, or to an
        # address a branch enters (paths joining there need not pass what comes before it).
        stretch, a = [], previous
        limit = (previous - start) // 4 + 1 if start is not None and start <= previous else 16
        for _ in range(limit):
            w = word(a)
            op = w >> 26
            if op in (16, 18, 19) and not (op in (16, 19) and (w >> 21) & 31 in (12, 13, 14, 15) and not w & 1):
                break
            stretch.insert(0, a)
            if a in sources or a in cases or a in entries:
                break
            a -= 4
        # Forward: registers in one class hold the same value ('mr' joins classes, other writes
        # separate them). The bound holds if the index is in the class of a 'cmplwi crN,r,max'
        # followed by a branch-if-greater on crN (with no later compare into crN).
        cls, fresh, compare, guarded, masked = {r: r for r in range(32)}, 32, None, None, {}
        slots = {}  # FM4 Recomp: d -> the class of the value last loaded from d(r1)
        for a in stretch:
            w = word(a)
            op, rs, ra, rb, _ = fields(w)
            mask = None
            if op == 21 and rb == 0 and (w >> 1) & 31 == 31:  # rlwinm ra,rs,0,MB,31: ra < 2^(32-MB)
                mask = (1 << (32 - ((w >> 6) & 31))) - 1
            elif op == 28:  # andi. ra,rs,UIMM: ra <= UIMM
                mask = w & 0xFFFF
            if op == 10 and not (w >> 21) & 1:
                compare = ((w >> 23) & 7, w & 0xFFFF, cls[ra], False)
            elif compare and compare[0] in cr_fields_written(w):
                compare = None  # the compared field is overwritten
            elif compare and is_branch_if(w, (12, 13, 14, 15), 4 * compare[0] + 1):
                compare = compare[:3] + (True,)
            if op == 31 and xo(w) == 444 and rs == rb:  # mr ra, rs
                cls[ra] = cls[rs]
            elif op == 32 and ra == 1 and rs != 1 and simm(w) in slots:
                # FM4 Recomp: lwz rT,d(r1) again with nothing between that can write memory or r1
                # (FM4's 0x82AE79A8: lwz r9,112(r1); cmplwi r9,15; bgt; lwz r8,112(r1)). The stack slot
                # belongs to this thread; another thread does not write it.
                cls[rs] = slots[simm(w)]
            else:
                for r in writes(w):
                    cls[r], fresh = fresh, fresh + 1
                if op == 32 and ra == 1 and rs != 1:
                    slots[simm(w)] = cls[rs]
            if not keeps_memory(w) or 1 in writes(w):
                slots = {}
            if mask is not None:
                masked[cls[ra]] = mask
        if compare and compare[3] and cls[index] == compare[2]:
            guarded = compare[1]
        if cls[index] in masked:  # a mask bounds the index too; the tighter bound holds
            guarded = masked[cls[index]] if guarded is None else min(guarded, masked[cls[index]])
        if guarded is None:
            failure = failure or 'the fall-through path has no bound'
        else:
            bounds.add(guarded)
    # Branches into the dispatch: each must be 'cmplwi crN,index,max' then branch if not greater.
    for source in sources.get(dispatch, []):
        w = word(source)
        compare_at = source - 4
        while word(compare_at) == NOP:
            compare_at -= 4
        compare = is_cmplwi(word(compare_at), index)
        if not (w >> 26 == 16 and not w & 1 and compare is not None and (w >> 16) & 31 == 4 * compare[0] + 1
                and (w >> 21) & 31 in (4, 5, 6, 7)) or source in sources:
            failure = failure or f'unguarded branch into the dispatch from {source:08X}'
        else:
            bounds.add(compare[1])
    # Cases of proven switches on the same index: their bound holds (the dispatch is a jump).
    for switch, switch_index, count in cases.get(dispatch, []):
        if switch_index != index:
            failure = failure or f'case of the switch at {switch:08X} on another register'
        else:
            bounds.add(count - 1)
    if failure is None and len(bounds) != 1:
        failure = 'no bound' if not bounds else 'paths disagree on the bound'
    if failure:
        return 0, failure
    return bounds.pop() + 1, None


def pdata(report, image):
    """(.pdata functions as sorted (start, end), every address that may be entered from outside
    the code's direct branches: each function start, and the exception handler words before a
    function with ExceptionFlag)."""
    base = report['base']
    section = next((s for s in report['sections'] if s['name'] == '.pdata'), None)
    functions, entries = [], set()
    if section is None:
        return functions, entries
    data = image[section['start'] - base:section['end'] - base]
    for offset in range(0, len(data) - 7, 8):
        begin, w = struct.unpack_from('>II', data, offset)
        if begin:
            functions.append((begin, begin + ((w >> 8) & 0x3FFFFF) * 4))
            entries.add(begin)
            if w >> 31 and begin - 8 >= base:
                entries.update(struct.unpack_from('>II', image, begin - 8 - base))
    return sorted(functions), entries


def address_taken(report, image, in_code):
    """Code addresses a pointer can hold: aligned code addresses stored in data sections, and those
    built in code by 'lis rD,hi' and the next addi/ori on rD (as scripts/validate.py finds them)."""
    base, found = report['base'], set()
    for s in report['sections']:
        if s['end'] <= s['start']:
            continue
        words = struct.unpack_from(f'>{(s["end"] - s["start"]) // 4}I', image, s['start'] - base)
        if not s['code']:
            found.update(v for v in words if not v & 3 and in_code(v))
            continue
        for i, w in enumerate(words):
            if w >> 26 != 15 or (w >> 16) & 31:
                continue
            rd, high = (w >> 21) & 31, (w & 0xFFFF) << 16
            for x in words[i + 1:i + 12]:
                if x >> 26 in (14, 24) and (x >> 16) & 31 == rd:
                    lo = x & 0xFFFF
                    found.add((high + (lo - 0x10000 if x >> 26 == 14 and lo & 0x8000 else lo)) & 0xFFFFFFFF)
                    break
    return found


def edge_bound(word, branch, taken, index, joins):
    """FM4 Recomp: the bound one edge of a conditional branch puts on index, or None. The branch
    tests a field set by 'cmplwi crN,index,K' or 'cmpwi crN,index,K' in the same straight-line
    block: no join (joins: addresses entered other than by falling through) from after the compare
    up to the branch, no write of the index or of crN, no other branch. The not-greater edge of an
    unsigned compare gives index <= K; the equal edge of either compare gives index == K (K >= 0)."""
    w = word(branch)
    if w >> 26 != 16 or w & 1:
        return None
    bo, bi = (w >> 21) & 31, (w >> 16) & 31
    if bo in (12, 13, 14, 15):
        holds = taken
    elif bo in (4, 5, 6, 7):
        holds = not taken
    else:
        return None
    field, bit = bi // 4, bi % 4
    a = branch - 4
    for _ in range(16):
        if a + 4 in joins:
            return None
        x = word(a)
        op = x >> 26
        if op in (10, 11) and (x >> 23) & 7 == field and not (x >> 21) & 1 and (x >> 16) & 31 == index:
            k = x & 0xFFFF
            if op == 10 and bit == 1 and not holds:
                return k
            if bit == 2 and holds and (op == 10 or k < 0x8000):
                return k
            return None
        if field in cr_fields_written(x) or index in writes(x) or op in (16, 18, 19):
            return None
        a -= 4
    return None


def prove_region(word, sources, cases, known, function, dispatch, bctr, index):
    """Function-level bound: follow every way into the dispatch back through its .pdata function
    until each starts at a case of a proven switch on the index. Returns (max + 1, None, those
    switches, the calls on the way) or (0, reason, [], []): the proof assumes each callee preserves
    the index. Ways in: falling through from the previous instruction (not after an unconditional
    transfer or from a switch table's words), a direct branch, a case of a proven switch on another
    register (followed back from its bctr). It fails on any other way in: the function's entry,
    a call, a branch from outside the function, an address a pointer can hold or a .pdata start
    (entries), a function with an unproven dispatch (blind; FM4 Recomp: every unproven dispatch,
    whose targets are unknown), an address with no known way in; or when an instruction on the
    way writes the index (a call is assumed to preserve r14-r31) or the cases disagree.
    FM4 Recomp: a way in also ends at an edge of a conditional branch that bounds the index
    (edge_bound: FM4's 0x824FB8FC, entered after 'cmpwi r28,15; bne' and 'cmplwi r28,15; bgt').
    table_words holds only proven tables' words: an unproven inline table lies in a blind
    function, which is refused before table_words is consulted."""
    entries, blind, table_words = known
    if function is None:
        return 0, 'not in exactly one .pdata function', [], []
    start, end = function
    # FM4 Recomp: the dispatch under proof is itself in `blind` after the first pass (it is
    # unproven until this proof succeeds); only another unproven dispatch refuses the function.
    if any(start <= b < end for b in blind if b != bctr):
        return 0, 'the function has a dispatch with unknown targets', [], []
    for a in range(dispatch + 4, bctr + 4, 4):
        if a in sources or a in cases or a in entries:
            return 0, f'{a:08X} inside the dispatch is entered', [], []
    if any(index in writes(word(a)) for a in range(dispatch, bctr, 4)):
        return 0, 'the dispatch writes the index', [], []
    joins = set(sources) | set(cases) | set(entries)
    bounds, bounded_by, seen, work = set(), set(), {dispatch}, [dispatch]
    while work:
        x = work.pop()
        if not start < x < end:
            return 0, f'reaches {x:08X}, the function entry or outside it', [], []
        if x in entries:
            return 0, f'{x:08X} may be entered through a pointer or a .pdata start', [], []
        ways = []
        if x - 4 not in table_words and falls_through(word(x - 4)):
            ways.append((x - 4, False))
        for source in sources.get(x, []):
            if word(source) & 1:
                return 0, f'{x:08X} is called from {source:08X}', [], []
            ways.append((source, True))
        bounded = False
        for switch, switch_index, count in cases.get(x, []):
            if switch_index == index:
                bounds.add(count - 1)
                bounded_by.add(switch)
                bounded = True
            else:
                ways.append((switch, True))
        if not ways and not bounded:
            return 0, f'no known way into {x:08X}', [], []
        for way, taken in ways:
            if not start <= way < end:
                return 0, f'{x:08X} is entered from {way:08X}, outside the function', [], []
            if index in writes(word(way)):
                return 0, f'{way:08X} writes the index', [], []
            k = edge_bound(word, way, taken, index, joins)
            if k is not None:
                bounds.add(k)
                bounded_by.add(way)
                continue
            if way not in seen:
                seen.add(way)
                work.append(way)
    if len(bounds) != 1:
        return 0, 'no bound' if not bounds else 'the cases disagree on the bound', [], []
    return bounds.pop() + 1, None, sorted(bounded_by), sorted(a for a in seen if word(a) >> 26 in (16, 18, 19) and word(a) & 1)


def prove_groups(word, in_code, sources, cases, entries, table_words, proven, unproven, function_of):
    """FM4 Recomp: joint proof of all the unproven dispatches of one function (FM4's 0x82B4DA48: four
    dispatches, each reached only through the others' cases or code after them). G = every unproven
    dispatch of the function; all inline ABSOLUTE tables on one index register. N = the case count
    of the proven switches on that register in the function (all equal; none: refused). Each
    member's first N table entries become case edges and its N table words data; prove_region then
    runs for every member with no blind dispatch in the function. All are accepted only if every
    member is proven with N cases and its inline extent agrees; otherwise none.
    Sound by induction on the first execution of any member with an index >= N: up to then every
    way into each member's trace was modelled (G holds every unproven dispatch of the function) and
    each ends at an earlier in-bound case edge or a bounding edge, with no write of the index on
    the way: a contradiction. Assumes, as prove_region does, that calls preserve r14-r31 and that
    no other function jumps into this one except through its entries. The bound never comes from
    a table's extent. Returns {bctr: entry fields} for the accepted members."""
    groups = {}
    for bctr in unproven:
        groups.setdefault(function_of(bctr), []).append(bctr)
    accepted = {}
    for function, members in groups.items():
        if function is None:
            continue
        start, end = function
        indexes = {unproven[b][0] for b in members}
        if len(indexes) != 1 or any(unproven[b][1] != 'ABSOLUTE' or unproven[b][4] != b + 4 for b in members):
            continue
        index = indexes.pop()
        roots = {len(labels) for b, (i, labels) in proven.items() if start <= b < end and i == index}
        if len(roots) != 1:
            continue
        n = roots.pop()
        group_cases = {k: list(v) for k, v in cases.items()}
        group_words = set(table_words)
        labels_of = {}
        for b in members:
            labels_of[b] = [unproven[b][3](word, i) for i in range(n)]
            for label in labels_of[b]:
                group_cases.setdefault(label, []).append((b, index, n))
            group_words.update(range(b + 4, b + 4 + 4 * n, 4))
        if not all(in_code(x) for b in members for x in labels_of[b]):
            continue
        proofs = {}
        for b in sorted(members):
            region, why, bounded_by, calls = prove_region(word, sources, group_cases, (entries, set(), group_words),
                                                          function, unproven[b][2], b, index)
            if region != n or inline_table_extent(word, in_code, b + 4, n - 1)[0] != n:
                proofs = None
                break
            proofs[b] = {'cases': n, 'rule': 'group', 'group': [f'{m:08X}' for m in sorted(members)],
                         'bounded_by': [f'{x:08X}' for x in bounded_by], 'calls_crossed': [f'{c:08X}' for c in calls]}
        if proofs:
            accepted.update(proofs)
    return accepted


def inline_table_extent(word, in_code, table, bound):
    """FM4 Recomp: ForzaRecomp's experimental_extent, reduced to its inline ABSOLUTE branch with a
    required bound. A function-level bound (max = bound) is kept only when the inline table right
    after the bctr agrees: (1) the words up to the first that is not an aligned code address,
    (2) the distance from the table to its lowest case after it (the code after the table starts
    there) and (3) bound + 1 must be equal. Returns (extent, reason) or (0, reason)."""
    scanned = 0
    while in_code(word(table + 4 * scanned)) and not word(table + 4 * scanned) & 3:
        scanned += 1
    after = [word(table + 4 * i) for i in range(scanned) if word(table + 4 * i) > table]
    lowest = (min(after) - table) // 4 if after else 0
    if not scanned or lowest != scanned or bound + 1 != scanned:
        return 0, f'inline table extents disagree: scan {scanned}, first case after it {lowest}, bound {bound}'
    return scanned, 'scan, first case after it, bound'


def classify_other(seq):
    """Not a table dispatch: a branch to CTR, translated as an indirect call. The reason records
    where CTR comes from when it is visible nearby."""
    if not seq or (seq[0][1] & MTCTR_MASK) != 0x7C0903A6:
        return {'class': 'INDIRECT', 'reason': 'CTR set elsewhere'}
    src = (seq[0][1] >> 21) & 31
    for _, w in seq[1:]:
        op, rt = w >> 26, (w >> 21) & 31
        if rt == src and (op in (32, 58) or (op == 31 and xo(w) in (23, 21))):
            return {'class': 'INDIRECT', 'reason': 'pointer loaded from memory'}
        if src in writes(w):
            return {'class': 'INDIRECT', 'reason': 'computed CTR'}
    return {'class': 'INDIRECT', 'reason': 'CTR set elsewhere'}


if __name__ == '__main__':
    main()
