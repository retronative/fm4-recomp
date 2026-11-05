# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ForzaRecomp contributors
# Copyright (C) 2026 FM4 Recomp contributors
#
# branch_targets, context_shape and context_routines are taken from ForzaRecomp
# scripts/survey.py (GPL-3.0-or-later, same owner).
# FM4 Recomp changes: pure functions over a word(address) callable (testable on synthetic words,
# no image file); a word outside the image reads as 0 (ForzaRecomp's slice gave b'' -> 0, the same
# value, implicitly); branch_targets also records whether each source is a call; new: evidence()
# (the table a reviewer uses to assign setjmp/longjmp/runtime_routines) and check_configured()
# (ForzaRecomp survey.py:141-149 as a function returning messages instead of exiting).
"""Context routines (setjmp, longjmp, fiber switch, context restore) in a PowerPC image.

A routine has the context shape when, within `window` instructions of its address, one base
register other than r1 is used to store (std/stw) or load (ld/lwz) r1 and at least 17 of r14-r31.
Kinds: 'store' (setjmp-like: r1 and the registers stored), 'load' (longjmp/restore-like: r1 and
the registers loaded), 'switch' (a context switch: stores r1 and the registers, loads the
registers; the new stack pointer may go elsewhere than r1).

Encodings (PowerPC ISA Book I): stw = primary opcode 36, lwz = 32 (D-form); std = 62 and ld = 58
(DS-form, extended opcode in the low 2 bits: 0 = std/ld, 1 = stdu/ldu, 2 = lwa); RS/RT bits 6-10,
RA bits 11-15. b = 18 (LI, AA, LK), bc = 16 (BD, AA, LK).
"""

STORE_OPS = (36,)       # stw
LOAD_OPS = (32,)        # lwz
DS_STORE, DS_LOAD = 62, 58


def image_word(image, base):
    """word(address) over a loaded image; 0 outside it."""
    def word(a):
        o = a - base
        return int.from_bytes(image[o:o + 4], 'big') if 0 <= o <= len(image) - 4 else 0
    return word


def branch_targets(word, code_sections):
    """{target: [(source, is_call), ...]} for every direct b/bl/bc in the code sections
    ((start, end) pairs)."""
    targets = {}
    for start, end in code_sections:
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
            target = d if w & 2 else (a + d) & 0xFFFFFFFF
            targets.setdefault(target, []).append((a, bool(w & 1)))
    return targets


def register_sets(word, address, window):
    """({base: stored registers}, {base: loaded registers}) of r1 and r14-r31 through a base
    register other than r1, within `window` instructions from `address`."""
    stores, loads = {}, {}
    for a in range(address, address + 4 * window, 4):
        w = word(a)
        op, rs, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if ra == 1 or not (rs == 1 or rs >= 14):
            continue
        if op in STORE_OPS or (op == DS_STORE and (w & 3) == 0):
            stores.setdefault(ra, set()).add(rs)
        elif op in LOAD_OPS or (op == DS_LOAD and (w & 3) == 0):
            loads.setdefault(ra, set()).add(rs)
    return stores, loads


def context_shape(word, address, kind, window=320):
    stores, loads = register_sets(word, address, window)

    def shaped(table, with_r1=True):
        return any((1 in regs or not with_r1) and len(regs - {1}) >= 17 for regs in table.values())
    return {'store': shaped(stores), 'load': shaped(loads), 'switch': shaped(stores) and shaped(loads, False)}[kind]


def context_routines(word, functions):
    """Every analysed function ((address, size) pairs) whose own extent has the store or load
    shape."""
    return {address for address, size in functions
            if any(context_shape(word, address, kind, window=max(1, size // 4)) for kind in ('store', 'load'))}


def configured_routines(config):
    """[(address, kind, name)] from config/fm4.toml [main]: setjmp = store, longjmp = load, a
    runtime routine = switch, or load when noreturn (a context restore)."""
    kinds = {'setjmp_address': 'store', 'longjmp_address': 'load'}
    checks = [(config[key], kind, key) for key, kind in kinds.items() if key in config]
    checks += [(r['address'], 'load' if r.get('noreturn') else 'switch', r['name'])
               for r in config.get('runtime_routines', [])]
    return checks


def check_configured(word, targets, checks):
    """Messages for each configured routine without its shape or not entered by a direct branch
    or call (empty when all hold)."""
    problems = []
    for address, kind, name in checks:
        if not context_shape(word, address, kind):
            problems.append(f'{name} at 0x{address:08X} does not have the context {kind} shape')
        if address not in targets:
            problems.append(f'{name} at 0x{address:08X} is not the target of any direct branch or call')
    return problems


def evidence(word, functions, targets, data_refs):
    """One row per shaped analysed function, for the reviewer who assigns the config keys:
    address, size, the shapes it has, direct calls (bl), direct non-call branches (b/bc), data
    words holding its address (data_refs: {address: count}), and each shaped base register with
    (r1 included, count of r14-r31)."""
    rows = []
    for address, size in sorted(functions):
        window = max(1, size // 4)
        kinds = [k for k in ('store', 'load', 'switch') if context_shape(word, address, k, window)]
        if not kinds:
            continue
        stores, loads = register_sets(word, address, window)
        sources = targets.get(address, [])
        rows.append({
            'address': f'{address:08X}', 'size': size, 'kinds': kinds,
            'calls': sum(1 for _, call in sources if call),
            'branches': sum(1 for _, call in sources if not call),
            'data_refs': data_refs.get(address, 0),
            'stores': {f'r{b}': (1 in r, len(r - {1})) for b, r in sorted(stores.items()) if len(r - {1}) >= 17},
            'loads': {f'r{b}': (1 in r, len(r - {1})) for b, r in sorted(loads.items()) if len(r - {1}) >= 17},
        })
    return rows
