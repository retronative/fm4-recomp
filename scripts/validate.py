#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ForzaRecomp contributors
# Copyright (C) 2026 FM4 Recomp contributors
#
# Adapted from ForzaRecomp scripts/validate.py (GPL-3.0-or-later, same owner).
# FM4 Recomp changes (marked "FM4 Recomp:" below):
#   - no module support; explicit --config/--switch-tables/--survey/--ppc paths, and check() /
#     self_test() take every input as an argument (nothing read at import), so
#     tests/test_validate.py runs them on a synthetic translation;
#   - 'experimental' removed (ForzaRecomp patches 0030/0031 not imported);
#   - 'fatal_residuals' (strict_stops, patch 0029, not imported) replaced by 'no_fatal': no FATAL
#     line and no upstream drop message in recomp.log, whatever its category (frsqrte is
#     translated by our patch 0034);
#   - 'no_error_markers' also rejects upstream's '// ERROR' comments (ReCarbon
#     validate_recomp.py:34-35; patch 0004 removed them, this guards a regression);
#   - new 'switch_tables' (patch 0025 README: the proven switch trusts that table words equal
#     the labels; the validator must check it): an in-code address table's words equal its labels,
#     every label is aligned code, an unproven entry has no data range;
#   - switch tables come from config/switch_tables.toml (not game/survey/);
#   - --syntax has no -DPPC_EXPERIMENTAL pass;
#   - self-test: no hard-coded FM4 address (ForzaRecomp used 0x823C2398 for address_taken; now
#     the first address-taken secondary entry of the input), the coverage injection uses the
#     first uncovered zero word of a code section (ForzaRecomp: the first zero word after the
#     entry point, which may lie inside a function), every fixture it cannot build is a FAIL line,
#     never an exception or a skip; strict-stop and experimental injections removed; injections
#     added for no_fatal, switch_tables and the '// ERROR' marker; a missing VMX helper key does
#     not raise (synthetic inputs have none).
"""Structural validator for the FM4 recompilation, run after scripts/survey.sh.

    /opt/homebrew/bin/python3 -B -I scripts/validate.py [--report-only] [--self-test] [--syntax]
        [--config config/fm4.toml] [--switch-tables config/switch_tables.toml]
        [--survey game/survey] [--ppc ppc]

Inputs (git-ignored except the two config files): <survey>/image.bin and report.json (from
tools/survey/xexsurvey), <survey>/recomp.log, XenonRecomp's output in <ppc> (ppc_functions.txt
from patch 0016, the generated .cpp/.h). .pdata is parsed from the image here, not taken from the
recompiler's analysis. Nothing is written except <survey>/validation.json (not with --self-test).

HARD checks (any failure exits 1 unless --report-only):
  pdata_functions   every .pdata entry is an analysed function of FunctionLength * 4 bytes (the
                    VMX save/restore helpers, split per register, excepted)
  no_overlap        no two analysed functions overlap (the save/restore helpers' shared tails
                    excepted)
  no_code_over_data no function extends over an explained data range (a switch table may lie in
                    the function of its own bctr)
  secondary_entries every secondary entry lies strictly inside the function whose body it
                    enters, is emitted, and is not an inline switch table
  runtime_routines  each context routine in config/fm4.toml (setjmp, longjmp, runtime_routines) is
                    a function whose body is only its runtime handling, with no secondary entry
  address_taken     every code address stored in data or built with lis/addi that follows an
                    unconditional terminator is a function start or a secondary entry
  no_tail_cycles    no cycle of control transfers between functions (branches that are not calls,
                    switch cases, and fall-through into the next function)
  coverage          every non-zero word of a code section lies in a function, an import thunk, the
                    XEX export table, the exception-handler words before a flagged .pdata
                    function (not a helper), a proven switch's address table, or EXPLAINED_RANGES
  switch_tables     every proven in-code address table's words equal its labels; every label is
                    aligned and in a code section; an unproven entry has table_size 0
  no_error_markers  no '#error' (except ppc_context.h's own guards) and no '// ERROR' in the output
  defined_labels    every "goto loc_X" targets a label its function defines
  no_fatal          recomp.log has no FATAL line and no upstream drop message
  syntax (--syntax) every generated .cpp passes clang++ -fsyntax-only
REPORT: functions by source, secondary entries, mid-code address-taken words, bctr count.
--self-test injects one fault per HARD check (except syntax) into the inputs, in memory, and
requires that check to catch it; it prints 'self-test <name>: detected' or
'FAIL self-test <name>: ...', and exits 1 if any is not detected.
"""
import argparse, bisect, collections, concurrent.futures, json, os, re, shutil, struct, subprocess, sys, tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# (start, end, reason) ranges of non-zero code-section words that are data, each with evidence.
EXPLAINED_RANGES = []
# .pdata functions the recompiler deliberately splits: the VMX register save/restore helpers are
# one .pdata entry each but one entry function per register (__savevmx_14 .. __savevmx_31).
SPLIT_PDATA = {'savevmx_14_address', 'restvmx_14_address'}
# FM4 Recomp: every recompiler complaint (patch 0004's FATAL and upstream's own messages).
PROBLEM = re.compile(r'^(FATAL: .*|Unrecognized instruction at .*|Unable to decode instruction.*|ERROR: .*)$', re.M)
BCTR, BLR = 0x4E800420, 0x4E800020


def load(survey, ppc):
    report = json.loads((survey / 'report.json').read_text())
    image = (survey / 'image.bin').read_bytes()
    functions, entries = [], []
    for line in (ppc / 'ppc_functions.txt').read_text().splitlines():
        base, size, source = line.split()
        if source.startswith('secondary@'):
            entries.append((int(base, 16), int(source.split('@')[1], 16)))
        else:
            functions.append((int(base, 16), int(size, 16), source))
    sources = {path.name: path.read_text(errors='replace') for path in sorted(ppc.glob('*.cpp')) + sorted(ppc.glob('*.h'))}
    log = (survey / 'recomp.log').read_text(errors='replace')
    return report, image, (functions, entries), sources, log


def pdata_entries(report, image):
    base = report['base']
    section = next(s for s in report['sections'] if s['name'] == '.pdata')
    data = image[section['start'] - base:section['end'] - base]
    for offset in range(0, len(data) - 7, 8):
        begin, word = struct.unpack_from('>II', data, offset)
        if begin == 0:
            continue
        yield begin, ((word >> 8) & 0x3FFFFF) * 4, bool(word >> 31)


def export_table(report, image):
    """The XEX export table, if the security header declares one with the supported layout
    (0x48000000, "HVE", 0x48000000, module and version words, image base >> 16, count, ordinal
    base, then one offset per export); an unsupported layout explains nothing."""
    start, base = report.get('export_table', 0), report['base']
    if not start:
        return []
    words = struct.unpack_from('>11I', image, start - base)
    if words[:3] != (0x48000000, 0x00485645, 0x48000000) or words[8] << 16 != base or words[9] > 0x10000:
        return []
    return [(start, start + 44 + 4 * words[9], 'XEX export table')]


def tail_cycles(report, image, functions, tables):
    """Cycles in the graph of non-call transfers between functions (decoded from the image)."""
    base = report['base']
    ordered = sorted((b, s) for b, s, _ in functions if s)
    starts = [b for b, _ in ordered]

    def owner(address):
        i = bisect.bisect_right(starts, address) - 1
        return ordered[i][0] if i >= 0 and address < ordered[i][0] + ordered[i][1] else None

    # Switch cases are transfers too: from the function of the bctr to the function of each case.
    switch_cases = collections.defaultdict(list)
    for table in tables:
        o = owner(table['base'])
        if o is not None:
            switch_cases[o] += table['labels']

    edges = collections.defaultdict(set)
    for b, size in ordered:
        for a in range(b, b + size, 4):
            w = struct.unpack_from('>I', image, a - base)[0]
            op = w >> 26
            if op not in (16, 18) or w & 1:
                continue
            d = (w & 0x3FFFFFC) if op == 18 else (w & 0xFFFC)
            sign = 0x2000000 if op == 18 else 0x8000
            d = d - 2 * sign if d & sign else d
            target = d if w & 2 else (a + d) & 0xFFFFFFFF
            o = owner(target)
            if o is not None and o != b:
                edges[b].add(o)
        for case in switch_cases.get(b, ()):
            o = owner(case)
            if o is not None and o != b:
                edges[b].add(o)
        last = struct.unpack_from('>I', image, b + size - 4 - base)[0]
        terminator = (last >> 26 == 18 and not last & 1) or last in (BLR, BCTR) or (last >> 26 == 19 and (last >> 21) & 0x14 == 0x14 and not last & 1)
        if not terminator and owner(b + size) == b + size:
            edges[b].add(b + size)
    # Tarjan's strongly connected components, iteratively.
    index, low, on, stack, counter, cycles = {}, {}, set(), [], [0], []
    for root in list(edges):
        if root in index:
            continue
        work = [(root, iter(edges[root]))]
        index[root] = low[root] = counter[0]; counter[0] += 1; stack.append(root); on.add(root)
        while work:
            node, it = work[-1]
            child = next(it, None)
            if child is not None:
                if child not in index:
                    index[child] = low[child] = counter[0]; counter[0] += 1; stack.append(child); on.add(child)
                    work.append((child, iter(edges.get(child, ()))))
                elif child in on:
                    low[node] = min(low[node], index[child])
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[node])
            if low[node] == index[node]:
                component = []
                while True:
                    x = stack.pop(); on.discard(x); component.append(x)
                    if x == node:
                        break
                if len(component) > 1:
                    cycles.append(' -> '.join(f'{x:08X}' for x in sorted(component)))
    return cycles


def address_taken(report, image):
    """Code addresses stored in data sections or built with lis and addi/ori in code, split into
    those after an unconditional terminator or padding (entry candidates: thunks, callbacks) and
    those in the middle of code (e.g. exception-handling state tables)."""
    base = report['base']
    code = [(s['start'], s['end']) for s in report['sections'] if s['code']]
    in_code = lambda a: any(x <= a < y for x, y in code)
    word = lambda a: struct.unpack_from('>I', image, a - base)[0]
    found = set()
    for s in report['sections']:
        if s['end'] == s['start']:  # an empty section may lie past the image
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
                    v = (high + (lo - 0x10000 if x >> 26 == 14 and lo & 0x8000 else lo)) & 0xFFFFFFFF
                    if not v & 3 and in_code(v):
                        found.add(v)
                    break

    def terminator(x):
        return x == 0 or (x >> 26 == 18 and not x & 1) or x in (BLR, BCTR) or \
            (x >> 26 == 19 and (x >> 21) & 0x14 == 0x14 and (x >> 1) & 0x3FF in (16, 528) and not x & 1)
    after = {a for a in found if terminator(word(a - 4))}
    return after, found - after


def switch_tables(report, image, tables):
    """Address tables of proven switches in code sections (survey/switch_tables.py) whose words
    are exactly the case addresses: explained data."""
    base, ranges = report['base'], []
    code = [(s['start'], s['end']) for s in report['sections'] if s['code']]
    for table in tables:
        start, size, labels = table.get('table', 0), table.get('table_size', 0), table['labels']
        if not labels or size != 4 * len(labels) or not any(a <= start and start + size <= b for a, b in code):
            continue
        words = list(struct.unpack_from(f'>{len(labels)}I', image, start - base))
        if words == labels:
            ranges.append((start, start + size, f'switch table of {table["base"]:08X}'))
    return ranges


def check(report, image, analysis, sources, log, config, tables):
    """FM4 Recomp: config ([main] of config/fm4.toml) and tables (the [[switch]] entries) are
    arguments."""
    functions, entries = analysis
    results = {}
    by_base = {base: size for base, size, _ in functions}

    split = {hits[0] for key in SPLIT_PDATA if (hits := report['helpers'].get(key))}
    bad = [f'{b:08X}' for b, size, _ in pdata_entries(report, image)
           if by_base.get(b) != size and not (b in split and by_base.get(b) and by_base[b] < size)]
    results['pdata_functions'] = bad

    # The register save/restore helpers are entered at every register and share one tail.
    ordered = sorted(functions)
    results['no_overlap'] = [f'{a:08X}+{sa:X} overlaps {b:08X}' for (a, sa, xa), (b, _, xb) in zip(ordered, ordered[1:])
                             if a + sa > b and not (xa == xb == 'helper')]

    base = report['base']
    code = [(s['start'], s['end']) for s in report['sections'] if s['code']]
    covered = bytearray(len(image) // 4)
    for b, size, _ in functions:
        covered[(b - base) // 4:(b + size - base) // 4] = b'\x01' * (size // 4)
    thunks = [(i['address'], i['address'] + 16, 'import thunk') for i in report['imports']]
    helpers = {b for b, _, source in functions if source == 'helper'}
    metadata = [(b - 8, b, 'exception handler and data') for b, _, flag in pdata_entries(report, image)
                if flag and b not in helpers]
    data = EXPLAINED_RANGES + thunks + metadata + export_table(report, image) + switch_tables(report, image, tables)
    for start, end, _ in data:
        covered[(start - base) // 4:(end - base) // 4] = b'\x01' * ((end - start) // 4)
    uncovered = []
    for start, end in code:
        for address in range(start, end, 4):
            if not covered[(address - base) // 4] and image[address - base:address - base + 4] != b'\0\0\0\0':
                uncovered.append(address)
    results['coverage'] = [f'{a:08X}' for a in uncovered]

    starts = sorted((b, b + size) for b, size, _ in functions)
    over = []
    for start, end, reason in data:
        i = bisect.bisect_right(starts, (end, 0))
        for b, e in starts[max(0, i - 64):i]:
            # A switch table may lie inside the function of its own bctr, and in no other.
            own_table = reason.startswith('switch table of ') and b <= int(reason.split()[-1], 16) < e
            if b < end and start < e and not own_table:
                over.append(f'{b:08X} covers {reason} at {start:08X}')
    results['no_code_over_data'] = over

    text = ''.join(sources.values())
    emitted = set(re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)', text, re.M))
    results['secondary_entries'] = [f'{e:08X} not inside {o:08X}' for e, o in entries
                                    if not (o in by_base and o < e < o + by_base[o])]
    results['secondary_entries'] += [f'{e:08X} not emitted' for e, _ in entries if f'{e:X}' not in emitted]

    results['no_tail_cycles'] = tail_cycles(report, image, functions, tables)

    # Address-taken code after a terminator must be enterable: a function start or a secondary
    # entry (switch cases and data ranges excepted, which are not entered through pointers). An
    # address table right after its bctr is data, never an entry, even when its bound is unproven.
    after, middle = address_taken(report, image)
    inline_tables = {t['table'] for t in tables if t['table'] == t['base'] + 4 and struct.unpack_from('>I', image, t['base'] - base)[0] == BCTR}
    cases = {l for t in tables for l in t['labels']} | inline_tables
    results['secondary_entries'] += [f'{e:08X} is the switch table after {e - 4:08X}' for e, _ in entries if e in inline_tables]
    enterable = {b for b, _, _ in functions} | {e for e, _ in entries}
    data_words = set()
    for start, end, _ in data:
        data_words.update(range(start, end, 4))
    results['address_taken'] = [f'{a:08X}' for a in sorted(after - enterable - cases - data_words)]

    # FM4 Recomp: the table/label consistency patch 0025 relies on.
    in_code = lambda a: any(x <= a < y for x, y in code)
    bad = []
    for t in tables:
        labels, start, size = t['labels'], t.get('table', 0), t.get('table_size', 0)
        if not labels:
            if size:
                bad.append(f'{t["base"]:08X}: unproven, but its table is a {size}-byte data range')
            continue
        bad += [f'{t["base"]:08X}: case {l:08X} is not aligned code' for l in labels if l & 3 or not in_code(l)]
        if size == 4 * len(labels) and in_code(start) and \
                list(struct.unpack_from(f'>{len(labels)}I', image, start - base)) != labels:
            bad.append(f'{t["base"]:08X}: table words at {start:08X} differ from its labels')
    results['switch_tables'] = bad

    expected = {config[k]: marker for k, marker in (('setjmp_address', '__builtin_trap();'),
                                                     ('longjmp_address', 'longjmp(PpcJumpBuffer(ctx.r3.u32), ctx.r4.s32);'))
                if k in config}
    expected.update({r['address']: f"PpcRuntime_{r['name']}(ctx, base);" for r in config.get('runtime_routines', [])})
    problems = []
    for address, marker in expected.items():
        body = re.search(rf'^PPC_FUNC_IMPL\(__imp__sub_{address:X}\) {{\n(.*?)^}}', text, re.M | re.S)
        lines = [l.strip() for l in body.group(1).splitlines()] if body else []
        if lines[1:] != [marker] and lines[1:] != [marker, '__builtin_trap();']:
            problems.append(f'{address:08X}: body is not only its runtime handling')
        if any(o == address for _, o in entries):
            problems.append(f'{address:08X}: has a secondary entry')
    results['runtime_routines'] = problems

    # No #error in any generated file, except the copied runtime header ppc_context.h, whose own
    # preprocessor guards use #error. FM4 Recomp: no '// ERROR' either.
    results['no_error_markers'] = [f'{name}: {message}' for name, t in sources.items()
                                   for message in re.findall(r'^#error(.*)$', t, re.M)
                                   if message.startswith(' "XenonRecomp: ') or name != 'ppc_context.h']
    results['no_error_markers'] += [f'{name}: {m.strip()}' for name, t in sources.items()
                                    for m in re.findall(r'^\s*(// ERROR\b.*)$', t, re.M)]

    undefined = []
    for name, t in sources.items():
        for body in re.split(r'\nPPC_FUNC_IMPL\(', t)[1:]:
            fn = body.split(')', 1)[0]
            used = set(re.findall(r'goto loc_([0-9A-F]+);', body))
            defined = set(re.findall(r'^loc_([0-9A-F]+):', body, re.M))
            undefined += [f'{fn}: loc_{label}' for label in sorted(used - defined)]
    results['defined_labels'] = undefined

    # FM4 Recomp: any complaint fails; accepted residuals are not FATAL (patch 0035), see below.
    results['no_fatal'] = PROBLEM.findall(log)

    # FM4 Recomp (patch 0035): the residual stops are exactly the configured list. Every listed
    # dispatch is an unproven switch table emitted as a stop with its name; every emitted stop and
    # every unproven switch table is listed.
    listed = {r['address']: r['name'] for r in config.get('residual_stops', [])}
    unproven = {t['base'] for t in tables if not t['labels']}
    stops = {}
    for t in sources.values():
        for address, name in re.findall(r'PpcResidualStop\(0x([0-9A-F]+)u, "([A-Za-z0-9_]+)"', t):
            stops[int(address, 16)] = name
    problems = [f'{a:08X}: listed, but not an unproven switch table' for a in sorted(set(listed) - unproven)]
    problems += [f'{a:08X}: unproven switch table not listed' for a in sorted(unproven - set(listed))]
    problems += [f'{a:08X}: listed, but no stop emitted' for a in sorted(set(listed) - set(stops))]
    problems += [f'{a:08X}: stop emitted, but not listed' for a in sorted(set(stops) - set(listed))]
    problems += [f'{a:08X}: stop named {stops[a]}, listed as {listed[a]}' for a in sorted(set(stops) & set(listed))
                 if stops[a] != listed[a]]
    results['residual_stops'] = problems

    report_only = {
        'functions_by_source': dict(collections.Counter(source for _, _, source in functions)),
        'secondary_entries': len(entries),
        'address_taken_mid_code': len(middle),
        'bctr': sum(len(re.findall(r'^\t// bctr $', t, re.M)) for t in sources.values()),
    }
    return results, report_only


def loop_split(report, image, functions):
    """The function list with one loop split in two: a function with a backward branch from X to
    T is cut at S (T < S <= X) where the word before S falls through; None if there is none."""
    base = report['base']
    word = lambda a: struct.unpack_from('>I', image, a - base)[0]
    for b, size, source in sorted(functions):
        for x in range(b, b + size, 4):
            w = word(x)
            if w >> 26 != 16 or w & 3:
                continue
            d = w & 0xFFFC
            d = d - 0x10000 if d & 0x8000 else d
            t = x + d
            if not b < t < x:
                continue
            for cut in range(t + 4, x + 4, 4):
                if word(cut - 4) >> 26 not in (16, 18, 19):
                    return [f for f in functions if f[0] != b] + [(b, cut - b, source), (cut, b + size - cut, source)]
    return None


def self_test(report, image, analysis, sources, log, config, tables):
    """Inject one fault per HARD check; each must be caught by its check. FM4 Recomp: every
    fixture is derived from the inputs; one that cannot be built is a failure."""
    functions, entries = analysis
    base = report['base']
    clean, _ = check(report, image, analysis, sources, log, config, tables)
    # Detection is a change in a check's failure count, so an input that already fails a check
    # can still be tested; tests/test_validate.py requires a clean synthetic baseline separately.
    already = [k for k, v in clean.items() if v]
    if already:
        print('self-test note: the unmodified inputs already fail ' + ', '.join(already))
    ok, mutations, missing = True, {}, []

    def fixture(name, build):
        try:
            value = build()
        except (StopIteration, IndexError, KeyError, TypeError, ValueError) as e:
            value, why = None, f'{type(e).__name__} {e}'
        else:
            why = 'not found in the inputs'
        if value is None:
            missing.append((name, why))
        else:
            mutations[name] = value

    starts = sorted((b, b + size) for b, size, _ in functions)
    proven = switch_tables(report, image, tables)
    f0 = sorted(functions)[0] if functions else None
    first = next((b for b, _, _ in pdata_entries(report, image)), None)
    covered = bytearray(len(image) // 4)  # one byte per word, as check() does
    metadata = [(b - 8, b) for b, _, flag in pdata_entries(report, image) if flag]
    for start, end in starts + metadata + [(s, e) for s, e, _ in proven + export_table(report, image) + EXPLAINED_RANGES] + \
            [(i['address'], i['address'] + 16) for i in report['imports']]:
        covered[(start - base) // 4:(end - base) // 4] = b'\x01' * ((end - start) // 4)
    code = [(s['start'], s['end']) for s in report['sections'] if s['code']]
    after = address_taken(report, image)[0]

    def put(img, address, value):
        return img[:address - base] + struct.pack('>I', value) + img[address - base + 4:]

    fixture('pdata_functions', lambda: (image, ([x if x[0] != first else (x[0], x[1] - 4, x[2]) for x in functions], entries), sources, log, config, tables) if first in {b for b, _, _ in functions} else None)
    fixture('no_overlap', lambda: (image, (functions + [(f0[0] + 4, 4, 'mutant')], entries), sources, log, config, tables))
    fixture('secondary_entries', lambda: (image, (functions, entries + [(f0[0] + f0[1] + 0x100000, f0[0])]), sources, log, config, tables))
    fixture('secondary_entries/switch table', lambda: (lambda inline, owner: (
        image, (functions, entries + [(inline, owner)]),
        {**sources, 'mutant.cpp': f'\nPPC_FUNC_IMPL(__imp__sub_{inline:X}) {{\n}}\n'}, log, config, tables))(
        *next((start, b) for start, _, reason in proven if start == int(reason.split()[-1], 16) + 4
              for b, e in starts if b < start < e)))
    split = loop_split(report, image, functions)
    fixture('no_tail_cycles', lambda: (image, (split, entries), sources, log, config, tables) if split else None)
    # FM4 Recomp: the first address-taken secondary entry of the inputs (ForzaRecomp: 0x823C2398).
    victim = min((e for e, _ in entries if e in after), default=None)
    fixture('address_taken', lambda: (image, (functions, [x for x in entries if x[0] != victim]), sources, log, config, tables) if victim else None)
    outside = next((start for start, end, _ in proven if not any(b <= start < e for b, e in starts)), None)
    fixture('coverage/switch table', lambda: (put(image, outside, 4), analysis, sources, log, config, tables) if outside else None)
    fixture('runtime_routines', lambda: (image, analysis, {n: t.replace('PpcRuntime_', 'PpcMutant_') for n, t in sources.items()}, log, config, tables)
            if any('PpcRuntime_' in t for t in sources.values()) else None)
    fixture('no_code_over_data', lambda: (image, (functions + [(report['imports'][0]['address'], 16, 'mutant')], entries), sources, log, config, tables))
    # FM4 Recomp: the first zero word of a code section that nothing covers (padding).
    zero = next((a for s, e in code for a in range(s, e, 4)
                 if not covered[(a - base) // 4] and image[a - base:a - base + 4] == b'\0\0\0\0'), None)
    fixture('coverage', lambda: (put(image, zero, 0x60000000), analysis, sources, log, config, tables) if zero else None)
    fixture('no_error_markers', lambda: (image, analysis, {**sources, 'mutant.cpp': '#error "mutant"\n'}, log, config, tables))
    fixture('no_error_markers/comment', lambda: (image, analysis, {**sources, 'mutant.cpp': '\t// ERROR 82000000\n'}, log, config, tables))
    fixture('defined_labels', lambda: (image, analysis, {**sources, 'mutant.cpp': '\nPPC_FUNC_IMPL(__imp__sub_0) {\n\tgoto loc_1;\n}\n'}, log, config, tables))
    fixture('no_fatal', lambda: (image, analysis, sources, log + '\nFATAL: mutant problem\n', config, tables))
    fixture('no_fatal/upstream', lambda: (image, analysis, sources, log + '\nUnrecognized instruction at 0x82000000: mutant\n', config, tables))
    # FM4 Recomp: a proven table whose labels no longer equal its words.
    swapped = next(([dict(t, labels=t['labels'][::-1]) if t is s else t for t in tables]
                    for s in tables if len(set(s['labels'])) > 1 and s.get('table_size') == 4 * len(s['labels'])), None)
    fixture('switch_tables', lambda: (image, analysis, sources, log, config, swapped) if swapped else None)
    # FM4 Recomp (patch 0035): a stop nobody listed, and a listed address with no unproven table.
    fixture('residual_stops', lambda: (image, analysis, {**sources, 'mutant.cpp': '\tPpcResidualStop(0x82000000u, "mutant", ctx.r3.u32, uint32_t(ctx.lr));\n'},
                                       log, config, tables))
    fixture('residual_stops/listed', lambda: (image, analysis, sources, log,
                                              {**config, 'residual_stops': config.get('residual_stops', []) + [{'address': 0x82000000, 'name': 'mutant'}]},
                                              tables))
    unproven = [dict(t, labels=[], table_size=4) if i == 0 else t for i, t in enumerate(tables)]
    fixture('switch_tables/unproven data', lambda: (image, analysis, sources, log, config, unproven) if tables else None)

    for name, why in missing:
        print(f'FAIL self-test {name}: fixture missing ({why})')
    ok &= not missing
    for name, (i, a, s, l, c, t) in mutations.items():
        results, _ = check(report, i, a, s, l, c, t)
        changed = [k for k in results if len(results[k]) != len(clean[k])]
        passed = name.split('/')[0] in changed
        ok &= passed
        print(f'{"" if passed else "FAIL "}self-test {name}: {"detected" if passed else "NOT DETECTED"} (changed: {", ".join(changed) or "none"})')
    return ok


def syntax_check(ppc):
    """Files with clang++ -fsyntax-only errors."""
    clang = Path(os.environ['LLVM']) / 'clang++' if 'LLVM' in os.environ else Path('/opt/homebrew/opt/llvm/bin/clang++')
    if not clang.exists():
        clang = Path(shutil.which('clang++') or 'clang++')
    flags = ['-std=c++20', '-fsyntax-only', '-ferror-limit=0', '-Wno-unused-label', '-Werror=asm-operand-widths', '-include', 'cstdlib',
             f'-I{ppc}', f'-I{ROOT / "tools/XenonRecomp/thirdparty/simde"}']

    def run(path):
        result = subprocess.run([str(clang), *flags, str(path)], capture_output=True, text=True)
        return path.name, [line.split('error: ', 1)[1] for line in result.stderr.splitlines() if 'error: ' in line]
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        return [f'{name}: {bad[0]}' for name, bad in pool.map(run, sorted(ppc.glob('*.cpp'))) if bad]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', type=Path, default=ROOT / 'config/fm4.toml')
    parser.add_argument('--switch-tables', type=Path, default=ROOT / 'config/switch_tables.toml')
    parser.add_argument('--survey', type=Path, default=ROOT / 'game/survey')
    parser.add_argument('--ppc', type=Path, default=ROOT / 'ppc')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--syntax', action='store_true')
    parser.add_argument('--report-only', action='store_true')
    args = parser.parse_args()
    data = load(args.survey, args.ppc)
    config = tomllib.loads(args.config.read_text())['main']
    tables = tomllib.loads(args.switch_tables.read_text()).get('switch', [])
    if args.self_test:
        sys.exit(0 if self_test(*data, config, tables) else 1)
    results, report_only = check(*data, config, tables)
    if args.syntax:
        results['syntax'] = syntax_check(args.ppc)
    summary = {'hard': {k: {'failures': len(v), 'examples': v[:10]} for k, v in results.items()}, 'report': report_only}
    (args.survey / 'validation.json').write_text(json.dumps(summary, indent=2) + '\n')
    for name, failures in results.items():
        print(f'{"PASS" if not failures else "FAIL"} {name}: {len(failures)}' + (f' (e.g. {", ".join(failures[:3])})' if failures else ''))
    print('report:', json.dumps(report_only['functions_by_source']), f"bctr {report_only['bctr']}")
    if any(results.values()) and not args.report_only:
        sys.exit(1)


if __name__ == '__main__':
    main()
