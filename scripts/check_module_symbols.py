#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""Fail the build if two module libraries define one translated symbol.

    /opt/homebrew/bin/python3 -B -I scripts/check_module_symbols.py <nm> <library> <ppc_recomp_shared.h> [<library> <header>]...

The translated functions are weak wrappers (XenonRecomp PPC_WEAK_FUNC) around their bodies: if two
libraries defined one name, the linker would keep one silently and the other module would run the wrong
code. So a symbol defined by two libraries is an error when it is a translated name: a function its
header declares, that function's __imp__ body, or PPCFuncMappings (each module's own names are renamed
by its generated <name>_symbols.h). Other duplicates are inline definitions from the shared headers,
allowed only if every module's ppc_context.h (next to its header) is byte-identical.
"""
import re, subprocess, sys
from pathlib import Path

MANGLED = re.compile(r'_Z(\d+)')


def identifier(symbol):
    """The source identifier of a Mach-O symbol: the leading underscore removed; a C++ name
    (Itanium _Z<length><name>...) reduced to its name."""
    name = symbol[1:] if symbol.startswith('_') else symbol
    m = MANGLED.match(name)
    if m:
        start = m.end()
        return name[start:start + int(m.group(1))]
    return name


def defined(nm_output):
    """Defined external symbols from `nm -gUj` output (archive member headers and blank lines skipped)."""
    return {line.strip() for line in nm_output.splitlines() if line.strip() and not line.rstrip().endswith(':')}


def translated(header_text):
    """The names a translation defines: every declared function, its __imp__ body, PPCFuncMappings."""
    functions = set(re.findall(r'^PPC_EXTERN_FUNC\((\w+)\);', header_text, re.M))
    return functions | {'__imp__' + f for f in functions} | {'PPCFuncMappings'}


def check(libraries):
    """libraries: [(label, defined symbols, translated names, ppc_context.h bytes)]. Returns problems."""
    owners = {}
    for label, symbols, _, _ in libraries:
        for s in symbols:
            owners.setdefault(s, []).append(label)
    names = set().union(*(t for _, _, t, _ in libraries))
    duplicates = sorted(s for s, o in owners.items() if len(o) > 1)
    problems = [f'{s} ({identifier(s)}) is defined by {", ".join(owners[s])}' for s in duplicates if identifier(s) in names]
    contexts = {c for _, _, _, c in libraries}
    if duplicates and len(contexts) > 1:
        problems.append(f'{len(duplicates)} shared definitions, but the modules\' ppc_context.h differ')
    return problems


def main():
    args = sys.argv[1:]
    if len(args) < 3 or len(args) % 2 != 1:
        sys.exit('usage: check_module_symbols.py <nm> <library> <ppc_recomp_shared.h> [<library> <header>]...')
    nm, pairs = args[0], [(Path(args[i]), Path(args[i + 1])) for i in range(1, len(args), 2)]
    libraries = []
    for library, header in pairs:
        r = subprocess.run([nm, '-gUj', str(library)], capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            sys.exit(f'check_module_symbols: {nm} failed on {library}: {r.stderr.strip()[:300]}')
        libraries.append((library.name, defined(r.stdout), translated(header.read_text()),
                          (header.parent / 'ppc_context.h').read_bytes()))
    problems = check(libraries)
    for p in problems[:20]:
        print(f'check_module_symbols: {p}', file=sys.stderr)
    if problems:
        sys.exit(f'check_module_symbols: {len(problems)} problems')
    print(f'check_module_symbols: {len(libraries)} libraries, no translated symbol defined twice')


if __name__ == '__main__':
    main()
