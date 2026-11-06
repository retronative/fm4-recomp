#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ForzaRecomp contributors
# Copyright (C) 2026 FM4 Recomp contributors
#
# Taken from ForzaRecomp scripts/gen_runtime_tables.py (GPL-3.0-or-later, same owner),
# reviewed and re-tested here. FM4 Recomp changes: an import kind other than thunk, function slot or
# variable stops the generation; the survey's thunk names must equal the __imp__ functions ppc/
# declares (ppc_recomp_shared.h), in both directions; the stubs mark their unused parameter; the
# survey script is scripts/survey.sh. M3: the executable's XEX header (size and CRC-32, the TLS
# directory, the default stack size) is read from default.xex; the stubs are weak, so an import the
# runtime implements (runtime/kernel, GUEST_IMPORT) overrides its stub (ForzaRecomp).
# After ForzaRecomp (gen_runtime_tables.py:82-94 and 148-172): further modules
# (--module): each gets <name>_symbols.h, which renames the names every translation defines alike, and
# the stubs cover the union of all modules' imports.
"""Generate the runtime's title tables from the survey of the player's own default.xex.

    /opt/homebrew/bin/python3 -B -I scripts/gen_runtime_tables.py <survey dir> <ppc dir> <default.xex> <out dir>
        [--module <name> <survey dir> <ppc dir> <xex>]...

Writes (into the Git-ignored build tree; nothing here is committed):
  fm4_image.h      the image layout, its entry point and CRC-32 (the runtime refuses an
                   image.bin that does not match the one translated), and every import record
  fm4_imports.cpp  one strict stub per function import of any module: calling it stops the run (M2-T04)
  <name>_symbols.h for each --module: force-included into that module's library, it renames every
                   name the translations define alike (PPCFuncMappings, _xstart, the save/restore
                   helpers: every declared function but sub_XXXXXXXX and __imp__*, each with its __imp__ body),
                   so two libraries
                   never define one symbol (scripts/check_module_symbols.py checks the result)

Every title address the runtime uses comes from here, never from runtime source (M2-T03).
"""
import json, re, struct, sys, zlib
from pathlib import Path

XEX_TLS_INFO, XEX_DEFAULT_STACK_SIZE = 0x00020104, 0x00020200  # Xenia xex2_info.h xex2_header_keys


def xex_header_bytes(xex):
    """The XEX2 header (magic, module flags, header size at +8, ...; Xenia xex2_info.h xex2_header)."""
    if xex[:4] != b'XEX2':
        sys.exit('gen_runtime_tables: not an XEX2 file')
    return xex[:struct.unpack_from('>I', xex, 8)[0]]


def xex_header(xex):
    """The executable's XEX2 header, its TLS directory {slot count, raw data address, data size, raw data
    size} and default stack size (both required for the executable)."""
    header = xex_header_bytes(xex)
    count = struct.unpack_from('>I', header, 0x14)[0]
    fields = dict(struct.unpack_from('>II', header, 0x18 + 8 * i) for i in range(count))
    if XEX_TLS_INFO not in fields or XEX_DEFAULT_STACK_SIZE not in fields:
        sys.exit('gen_runtime_tables: the XEX header has no TLS info or default stack size')
    tls = struct.unpack_from('>4I', header, fields[XEX_TLS_INFO])
    return header, tls, fields[XEX_DEFAULT_STACK_SIZE]


MODULE_NAME = re.compile(r'[a-z][a-z0-9]*')
GENERATED_NAME = re.compile(r'sub_[0-9A-F]{8}|__imp__\w+')


def load(survey, ppc):
    """A module's survey report, image and import thunks, checked against its translation; and every
    function its translation declares."""
    report = json.loads((survey / 'report.json').read_text())
    image = (survey / 'image.bin').read_bytes()
    config = (ppc / 'ppc_config.h').read_text()
    layout = {k: int(v, 16) for k, v in re.findall(r'#define (PPC_\w+) (0x[0-9A-Fa-f]+)ull', config)}
    if layout.get('PPC_IMAGE_BASE') != report['base'] or layout.get('PPC_IMAGE_SIZE') != report['size'] \
            or len(image) != report['size']:
        sys.exit(f'gen_runtime_tables: {ppc} and {survey} describe different images; re-run scripts/survey.sh')
    slots = report['import_slots']
    kinds = {s['kind'] for s in slots} - {'thunk', 'function_slot', 'variable'}
    if kinds:
        sys.exit(f'gen_runtime_tables: unexpected import record kinds {sorted(kinds)}')
    thunks = {s['name']: s['address'] for s in slots if s['kind'] == 'thunk'}
    functions = set(re.findall(r'^PPC_EXTERN_FUNC\((\w+)\);', (ppc / 'ppc_recomp_shared.h').read_text(), re.M))
    declared = {f for f in functions if f.startswith('__imp__')}
    if declared != set(thunks):
        sys.exit(f'gen_runtime_tables: import thunks and {ppc} differ: only in the survey {sorted(set(thunks) - declared)[:5]}, '
                 f'only in ppc/ {sorted(declared - set(thunks))[:5]}')
    return report, image, thunks, functions


def symbols_header(name, functions):
    """Renames, for module `name`'s library, every name the translations define alike."""
    # Each such function is a weak wrapper f around its body __imp__f (XenonRecomp PPC_WEAK_FUNC,
    # PPC_FUNC_IMPL): both are renamed.
    shared = [n for f in sorted(f for f in functions if not GENERATED_NAME.fullmatch(f)) for n in (f, '__imp__' + f)]
    lines = '\n'.join(f'#define {f} {name}_{f}' for f in shared + ['PPCFuncMappings'])
    return f"""// Generated by scripts/gen_runtime_tables.py from the player's own files; do not edit.
// Force-included into the {name} library: the names every translation defines alike, renamed.
#pragma once
{lines}
"""


def module_image(name, report, image, thunks, ppc, file_name, xex):
    """One runtime::ModuleImage initializer (kernel/loader.h) and its tables, for a module default.xex loads."""
    layout = {k: int(v, 16) for k, v in re.findall(r'#define (PPC_\w+) (0x[0-9A-Fa-f]+)ull', (ppc / 'ppc_config.h').read_text())}
    header = xex_header_bytes(xex)
    sections = ''.join(f'    {{0x{s["start"]:08X}u, 0x{s["end"]:08X}u, {"true" if s["code"] else "false"}}},\n' for s in report['sections'])
    imports = ''.join(f'    {{0x{s["address"]:08X}u, {"true" if s["kind"] == "variable" else "false"}, 0x{thunks.get(s["name"], 0) if s["kind"] == "function_slot" else 0:08X}u, "{s["name"]}"}},\n'
                      for s in report['import_slots'] if s['kind'] != 'thunk')
    return f"""extern PPCFuncMapping {name}_PPCFuncMappings[];
inline constexpr runtime::Section k{name}Sections[] = {{
{sections}}};
inline constexpr runtime::ModuleImport k{name}Imports[] = {{
{imports}}};
""", f"""    {{"{name}", "{file_name}", 0x{report['base']:08X}u, 0x{report['size']:08X}u, 0x{report['entry']:08X}u,
     0x{layout['PPC_CODE_BASE']:08X}u, 0x{layout['PPC_CODE_SIZE']:08X}u, 0x{zlib.crc32(image):08X}u, 0x{len(header):08X}u,
     0x{zlib.crc32(header):08X}u, 0x{len(xex):08X}u, 0x{zlib.crc32(xex):08X}u, 0x{report.get('export_table', 0):08X}u,
     k{name}Sections, std::size(k{name}Sections),
     k{name}Imports, std::size(k{name}Imports), {name}_PPCFuncMappings}},
"""


def modules_header(images):
    tables = ''.join(t for t, _ in images)
    entries = ''.join(e for _, e in images)
    return f"""// Generated by scripts/gen_runtime_tables.py from the player's own files; do not edit.
// The modules default.xex loads (XexLoadImage): their translated layout and the CRC-32 of the files
// they were translated from (kernel/loader.h).
#pragma once
#include "kernel/loader.h"
#include <iterator>

{tables}
namespace fm4 {{

inline const runtime::ModuleImage kModules[] = {{
{entries}}};

}}  // namespace fm4
"""


def main():
    args = sys.argv[1:]
    if len(args) < 4 or (len(args) - 4) % 5 or any(a != '--module' for a in args[4::5]):
        sys.exit('usage: gen_runtime_tables.py <survey dir> <ppc dir> <default.xex> <out dir> '
                 '[--module <name> <survey dir> <ppc dir> <xex>]...')
    survey, ppc, xex, out = (Path(a) for a in args[:4])
    modules = [(args[i + 1], Path(args[i + 2]), Path(args[i + 3]), Path(args[i + 4])) for i in range(4, len(args), 5)]
    names = [m[0] for m in modules]
    bad = [n for n in names if not MODULE_NAME.fullmatch(n) or n == 'fm4']
    if bad or len(set(names)) != len(names):
        sys.exit(f'gen_runtime_tables: module names must be distinct, [a-z][a-z0-9]* and not fm4: {names}')
    header, tls, stack = xex_header(xex.read_bytes())
    report, image, thunks, _ = load(survey, ppc)
    slots = report['import_slots']
    texts = {}
    stubbed = set(thunks)
    images = []
    for name, msurvey, mppc, mxex in modules:
        mreport, mimage, mthunks, functions = load(msurvey, mppc)
        stubbed |= set(mthunks)
        texts[f'{name}_symbols.h'] = symbols_header(name, functions)
        images.append(module_image(name, mreport, mimage, mthunks, mppc, mxex.name, mxex.read_bytes()))
    texts['fm4_modules.h'] = modules_header(images)
    out.mkdir(parents=True, exist_ok=True)

    rows = []
    for s in slots:
        kind = {'thunk': 'Thunk', 'function_slot': 'FunctionSlot', 'variable': 'Variable'}[s['kind']]
        target = thunks.get(s['name'], 0) if s['kind'] == 'function_slot' else 0
        rows.append(f'    {{0x{s["address"]:08X}u, ImportKind::{kind}, 0x{target:08X}u, "{s["library"]}", "{s["name"]}"}},')
    header = f"""// Generated by scripts/gen_runtime_tables.py from the player's own default.xex; do not edit.
#pragma once
#include "layout.h"
#include "kernel/thread_state.h"

namespace fm4 {{

constexpr uint32_t kImageBase = 0x{report['base']:08X}u;
constexpr uint32_t kImageSize = 0x{report['size']:08X}u;
constexpr uint32_t kEntry = 0x{report['entry']:08X}u;
constexpr uint32_t kImageCrc32 = 0x{zlib.crc32(image):08X}u;
constexpr uint32_t kHeaderSize = 0x{len(header):08X}u;
constexpr uint32_t kHeaderCrc32 = 0x{zlib.crc32(header):08X}u;
constexpr runtime::TlsInfo kTls = {{{tls[0]}u, 0x{tls[1]:08X}u, 0x{tls[2]:X}u, 0x{tls[3]:X}u}};  // slots, raw data, sizes
constexpr uint32_t kDefaultStackSize = 0x{stack:X}u;

constexpr runtime::Section kSections[] = {{
{chr(10).join(f'    {{0x{s["start"]:08X}u, 0x{s["end"]:08X}u, {"true" if s["code"] else "false"}}},' for s in report['sections'])}
}};

enum class ImportKind {{ Thunk, FunctionSlot, Variable }};
struct ImportRecord {{ uint32_t address; ImportKind kind; uint32_t thunk; const char* library; const char* name; }};
constexpr ImportRecord kImports[] = {{
{chr(10).join(rows)}
}};
constexpr size_t kImportCount = sizeof(kImports) / sizeof(kImports[0]);

}}  // namespace fm4
"""
    stubs = '\n'.join(f'__attribute__((weak)) PPC_FUNC({name}) {{ (void)base; PpcImportStop("{name.removeprefix("__imp__")}", ctx); }}'
                      for name in sorted(stubbed))
    source = f"""// Generated by scripts/gen_runtime_tables.py from the player's own default.xex; do not edit.
// Strict import stubs, one per import of any module: every call stops the run with a report. They are
// weak: an import the runtime implements (GUEST_IMPORT, runtime/kernel) overrides its stub.
#include "stops.h"
#include "ppc_recomp_shared.h"  // every stub must match the translation's declaration (C++ linkage)

{stubs}
"""
    texts.update({'fm4_image.h': header, 'fm4_imports.cpp': source})
    for name, text in texts.items():
        path = out / name
        if not path.exists() or path.read_text() != text:  # unchanged output keeps dependants built
            path.write_text(text)


if __name__ == '__main__':
    main()
