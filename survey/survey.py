#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ForzaRecomp contributors
# Copyright (C) 2026 FM4 Recomp contributors
#
# Adapted from ForzaRecomp scripts/survey.py (GPL-3.0-or-later, same owner).
# FM4 Recomp changes:
#   - modules from a fixed table (MODULES: default.xex, XMediaFacade_default.xex), chosen with
#     --module; no scripts/modules.py; each module's paths fixed and checked;
#   - report.json from tools/survey/xexsurvey (survey/xexsurvey.cpp) instead of xex_report;
#     import records must have no "unexpected" kind;
#   - the context-routine shape check lives in survey/context_routines.py (tested on synthetic
#     words); a mismatch prints and writes the evidence table used to (re)derive the config keys;
#   - the switch TOML goes to config/switch_tables.toml (committed, addresses only), bctr.json to
#     game/survey/;
#   - fail-closed by default (ForzaRecomp's --require-clean); --diagnostic records the census and
#     exits 0 only when nothing crashed and the inputs were consistent;
#   - the census also counts upstream's other drop messages and the '// ERROR' / '#error
#     "XenonRecomp:' markers in ppc/ (reference/recompiler/pipeline_map.md section 5);
#   - every step has a deadline (process group SIGTERM, then SIGKILL);
#   - ppc/ is emptied file by file, only when it holds nothing but XenonRecomp's output, and only
#     if it is a module's own output directory with no symlink on its path;
#   - reproducibility.json (per-file SHA-256 of ppc/ and config/switch_tables.toml, sorted by
#     byte order, no timings) and --compare-with for the two-run check;
#   - the [main] key assertion (a key written after a table header belongs to that table)
#     (ReCarbon): every XenonRecomp key parses as a key of [main].
"""Survey one of FM4's modules and recompile it.

    scripts/survey.sh [--module default|xmediafacade] [--diagnostic] [--compare-with FILE]
    (runs /opt/homebrew/bin/python3 -B -I survey/survey.py with the same arguments; without
    --module, the executable default.xex)

Inputs:  the module's config (config/fm4.toml, config/xmediafacade.toml), the XEX it names (under
         game/files/), tools/survey/xexsurvey, tools/XenonRecomp/build/XenonRecomp/XenonRecomp,
         XenonUtils/ppc_context.h.
Outputs (MODULES): the survey directory (game/survey/, game/modules/<m>/survey/; git-ignored: image.bin,
         report.json, bctr.json, logs, summary.json, reproducibility.json, context_routines.json),
         the switch TOML (config/switch_tables*.toml, committed: addresses only), the translation
         (ppc/, game/modules/<m>/ppc/; git-ignored).

Steps: [main] key check -> xexsurvey -> compiler helpers equal the config (one hit each) ->
import records consistent -> configured context routines have their shape and are branch targets
-> switch_tables.py -> XenonRecomp -> census of problems -> every context-shaped analysed function
is a configured routine -> summary.json, reproducibility.json.
Exit 0: clean (or --diagnostic and no crash/inconsistency); 1 otherwise.
"""
import argparse, collections, hashlib, json, os, re, signal, subprocess, sys, time, tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'survey'))
import context_routines as C  # noqa: E402

Module = collections.namedtuple('Module', 'config switch out ppc')
MODULES = {
    'default': Module(ROOT / 'config/fm4.toml', ROOT / 'config/switch_tables.toml', ROOT / 'game/survey', ROOT / 'ppc'),
    'xmediafacade': Module(ROOT / 'config/xmediafacade.toml', ROOT / 'config/switch_tables_xmediafacade.toml',
                           ROOT / 'game/modules/xmediafacade/survey', ROOT / 'game/modules/xmediafacade/ppc'),
    # FM4 Recomp (playable alpha): the module FM4 loads after a race.
    'speechfacade': Module(ROOT / 'config/speechfacade.toml', ROOT / 'config/switch_tables_speechfacade.toml',
                           ROOT / 'game/modules/speechfacade/survey', ROOT / 'game/modules/speechfacade/ppc'),
}
TOOLS = {
    'xexsurvey': ROOT / 'tools/survey/xexsurvey',
    'XenonRecomp': ROOT / 'tools/XenonRecomp/build/XenonRecomp/XenonRecomp',
}
PPC_CONTEXT = ROOT / 'tools/XenonRecomp/XenonUtils/ppc_context.h'
PYTHON = '/opt/homebrew/bin/python3'
# Deadlines in seconds. HYPOTHESIS: generous until the first run is timed (ForzaRecomp's
# summary.json timings are the reference to check); then set to about 3x the observed time.
DEADLINES = {'xexsurvey': 300, 'switch_tables': 900, 'XenonRecomp': 3600}
# Keys XenonRecomp reads from [main] (upstream recompiler_config.cpp + patches 0024): each must
# parse as [main]'s, never as a later table's.
MAIN_KEYS = ['file_path', 'out_directory_path', 'switch_table_file_path',
             'skip_lr', 'skip_msr', 'ctr_as_local', 'xer_as_local', 'reserved_as_local', 'cr_as_local',
             'non_argument_as_local', 'non_volatile_as_local',
             'restgprlr_14_address', 'savegprlr_14_address', 'restfpr_14_address', 'savefpr_14_address',
             'restvmx_14_address', 'savevmx_14_address', 'restvmx_64_address', 'savevmx_64_address']
# What the recompiler reports: FATAL (patch 0004 on), and upstream's own drop messages
# (recompiler.cpp "Unrecognized instruction at", "Unable to decode instruction", "ERROR: Switch
# case at ... outside function"), in case a patch regresses to them.
PROBLEM = re.compile(r'^(FATAL: .*|Unrecognized instruction at .*|Unable to decode instruction.*|ERROR: .*)$')
NOTE = re.compile(r'^NOTE: .*$')
MARKERS = re.compile(r'^\s*(// ERROR\b.*|#error "XenonRecomp: .*)$', re.M)
ADDRESS = re.compile(r'0x[0-9A-Fa-f]+|\b[0-9A-F]{8}\b')
PPC_SUFFIXES = {'.cpp', '.h', '.txt'}


def fail(message):
    sys.exit(f'survey: {message}')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def run(step, args, log, timings, out):
    """Run one step with its deadline, in its own process group; output to out/log."""
    start = time.monotonic()
    with open(out / log, 'w') as f:
        p = subprocess.Popen([str(a) for a in args], cwd=ROOT, stdout=f, stderr=subprocess.STDOUT,
                             start_new_session=True)
        try:
            code = p.wait(timeout=DEADLINES[step])
        except subprocess.TimeoutExpired:
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(p.pid, sig)
                except (ProcessLookupError, PermissionError):
                    break
                time.sleep(1)
            p.wait()
            fail(f'{step} passed its {DEADLINES[step]} s deadline; see {out / log}')
    timings[step] = round(time.monotonic() - start, 2)
    if code < 0:
        fail(f'{step} crashed (signal {-code}); see {out / log}')
    return code


def check_config(text, module):
    """[main] holds every XenonRecomp key; paths resolve where this pipeline expects them."""
    name = module.config.relative_to(ROOT)
    parsed = tomllib.loads(text)
    main = parsed.get('main', {})
    missing = [k for k in MAIN_KEYS if k not in main]
    if missing:
        fail(f'{name}: {missing} are not keys of [main] (a key written after a table header '
             f'belongs to that table)')
    # FM4 Recomp: XenonRecomp reads only [main]; any other top-level table is silently ignored
    # (a [[runtime_routines]] header once dropped two routines).
    others = sorted(k for k in parsed if k != 'main')
    if others:
        fail(f'{name}: {others} are outside [main]; XenonRecomp ignores them')
    if (module.config.parent / main['out_directory_path']).resolve() != module.ppc:
        fail(f'{name} out_directory_path must be {os.path.relpath(module.ppc, module.config.parent)}')
    if (module.config.parent / main['switch_table_file_path']).resolve() != module.switch:
        fail(f'{name} switch_table_file_path must be {module.switch.name} (config/)')
    if any(main[k] for k in MAIN_KEYS[3:11]):
        fail(f'{name}: skip_* and *_as_local must stay false (patch 0023 with secondary entries)')
    return main


def empty_ppc(ppc, allowed):
    """Delete XenonRecomp's previous output, file by file. Refuse, deleting nothing: a directory that
    is not one of `allowed` (the modules' own output directories), a path with a symlink on it (game/files
    is one), or anything XenonRecomp does not write (a subdirectory, a symlink, another suffix)."""
    if ppc.absolute() not in allowed:
        fail(f'{ppc} is not a module output directory')
    ppc.parent.mkdir(parents=True, exist_ok=True)
    if ppc.is_symlink() or ppc.parent.resolve() != ppc.parent.absolute():
        fail(f'{ppc} is a symlink or lies under one')
    ppc.mkdir(exist_ok=True)
    entries = list(ppc.iterdir())
    foreign = sorted(e.name for e in entries if e.is_symlink() or not e.is_file() or e.suffix not in PPC_SUFFIXES)
    if foreign:
        fail(f'{ppc} holds something XenonRecomp did not write: {foreign[:5]}; move it away first')
    for e in entries:
        e.unlink()


def tree_hashes(ppc, switch, root=ROOT):
    """{path relative to root: SHA-256} of the translation and the switch TOML, in byte order of the path."""
    files = {f'{ppc.relative_to(root).as_posix()}/{p.name}': p for p in ppc.iterdir() if p.is_file()}
    files[switch.relative_to(root).as_posix()] = switch
    return {name: sha256(files[name]) for name in sorted(files, key=lambda n: n.encode())}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', choices=sorted(MODULES), default='default',
                        help='the module to survey (default: the executable, default.xex)')
    parser.add_argument('--diagnostic', action='store_true',
                        help='record problems without failing (never for an acceptance run)')
    parser.add_argument('--compare-with', type=Path,
                        help='a reproducibility.json from an earlier run: fail on any difference')
    args = parser.parse_args()
    for name, path in TOOLS.items():
        if not path.is_file():
            fail(f'{path.relative_to(ROOT)} is missing: run scripts/setup_tools.sh')
    module = MODULES[args.module]
    CONFIG, SWITCH, OUT, PPC = module
    config = check_config(CONFIG.read_text(), module)
    xex = (CONFIG.parent / config['file_path']).resolve()
    if not xex.is_file():
        fail(f'{config["file_path"]} (from {CONFIG.relative_to(ROOT)}) is missing: see scripts/extract_game.sh')
    OUT.mkdir(parents=True, exist_ok=True)
    timings, inconsistent = {}, []

    if run('xexsurvey', [TOOLS['xexsurvey'], xex, OUT], 'xexsurvey.log', timings, OUT):
        fail(f'xexsurvey failed; see {OUT / "xexsurvey.log"}')
    report = json.loads((OUT / 'report.json').read_text())
    for key, hits in report['helpers'].items():
        if len(hits) != 1:
            fail(f'{key}: {len(hits)} matches in the image, expected exactly 1')
        if config.get(key) != hits[0]:
            fail(f'{key}: {CONFIG.relative_to(ROOT)} has {config.get(key)}, the image has 0x{hits[0]:08X}')
    slots = collections.Counter(s['kind'] for s in report['import_slots'])
    if slots['unexpected'] or slots['thunk'] != len(report['imports']) or slots['function_slot'] != slots['thunk']:
        fail(f'import records inconsistent: {dict(slots)}, {len(report["imports"])} thunks')

    image = (OUT / 'image.bin').read_bytes()
    word = C.image_word(image, report['base'])
    code_sections = [(s['start'], s['end']) for s in report['sections'] if s['code']]
    targets = C.branch_targets(word, code_sections)
    checks = C.configured_routines(config)
    problems = C.check_configured(word, targets, checks)
    if problems:
        fail('; '.join(problems))

    if run('switch_tables', [PYTHON, '-B', '-I', ROOT / 'survey/switch_tables.py', OUT, SWITCH], 'switch_tables.log', timings, OUT):
        fail(f'switch table recognition failed; see {OUT / "switch_tables.log"}')
    bctr = collections.Counter(e['class'] for e in json.loads((OUT / 'bctr.json').read_text()))

    empty_ppc(PPC, {m.ppc for m in MODULES.values()})  # check_config made sure PPC is this module's
    code = run('XenonRecomp', [TOOLS['XenonRecomp'], CONFIG, PPC_CONTEXT], 'recomp.log', timings, OUT)
    census, notes = collections.Counter(), collections.Counter()
    for line in (OUT / 'recomp.log').read_text(errors='replace').splitlines():
        for pattern, counter in ((PROBLEM, census), (NOTE, notes)):
            if pattern.match(line):
                counter[ADDRESS.sub('<address>', line)] += 1
    markers = collections.Counter()
    for path in sorted(PPC.glob('*.cpp')) + sorted(PPC.glob('*.h')):
        if path.name == 'ppc_context.h':
            continue  # the copied runtime header guards itself with #error
        for m in MARKERS.findall(path.read_text(errors='replace')):
            markers[ADDRESS.sub('<address>', m.strip())] += 1

    # Every analysed function with the context shape must be a configured routine: otherwise it
    # would be translated as ordinary code that cannot switch stacks or return twice.
    listing = PPC / 'ppc_functions.txt'
    if not listing.is_file():
        fail(f'{listing.relative_to(ROOT)} is missing (patch 0016): XenonRecomp did not finish')
    functions = [(int(a, 16), int(n, 16)) for a, n, *_ in (l.split() for l in listing.read_text().splitlines())]
    shaped = C.context_routines(word, functions)
    configured = {address for address, _, _ in checks}
    if shaped != configured:
        data_refs = collections.Counter()
        for s in report['sections']:
            if not s['code']:
                data_refs.update(word(a) for a in range(s['start'], s['end'], 4))
        rows = C.evidence(word, [f for f in functions if f[0] in shaped | configured], targets, data_refs)
        (OUT / 'context_routines.json').write_text(json.dumps(rows, indent=1) + '\n')
        for row in rows:
            print('context routine candidate:', json.dumps(row))
        inconsistent.append(f'context-shaped functions {sorted(map(hex, shaped))} differ from the configured '
                            f'routines {sorted(map(hex, configured))}; evidence in {OUT / "context_routines.json"}')

    hashes = tree_hashes(PPC, SWITCH)
    (OUT / 'reproducibility.json').write_text(json.dumps(hashes, indent=1) + '\n')
    summary = {
        'input': {xex.name: sha256(xex), CONFIG.relative_to(ROOT).as_posix(): sha256(CONFIG)},
        'image': {'image.bin': sha256(OUT / 'image.bin')},
        'tools': {name: sha256(path) for name, path in TOOLS.items()},
        'timings_seconds': timings,
        'xenonrecomp_exit_code': code,
        'bctr_by_class': dict(sorted(bctr.items())),
        'problems_total': sum(census.values()),
        'problems_by_category': dict(census.most_common()),
        'markers_in_output': dict(markers.most_common()),
        'notes_by_category': dict(notes.most_common()),
        'inconsistent': inconsistent,
        'ppc_files': len(hashes) - 1,
    }
    (OUT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'survey: XenonRecomp exit {code}, {summary["problems_total"]} problems ({len(census)} categories), '
          f'{sum(markers.values())} error markers, bctr {dict(sorted(bctr.items()))}; see {OUT / "summary.json"}')

    failed = bool(inconsistent)
    if args.compare_with is not None:
        before = json.loads(args.compare_with.read_text())
        differ = sorted(set(before) ^ set(hashes) | {k for k in set(before) & set(hashes) if before[k] != hashes[k]})
        print(f'reproducibility: {len(differ)} files differ from {args.compare_with}' + (f': {differ[:10]}' if differ else ''))
        failed |= bool(differ)
    if not args.diagnostic:
        failed |= code != 0 or bool(census) or bool(markers)
    for message in inconsistent:
        print(f'survey: {message}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
