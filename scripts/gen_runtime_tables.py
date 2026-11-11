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


FLAG_NAME = re.compile(r'FM4_[A-Z0-9_]+')
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


def title_hooks(config_text, image, base):
    """fm4_hooks.cpp from config/title_hooks.toml: one strong override per hook of a translated function's weak definition. Each
    hook's function bytes must hash to its recorded SHA-256 in this image (fail closed: a different disc or build refuses to build).
    With its flag (an environment variable equal to "1") a `returns` hook answers the recorded value in r3, and a `stop_on_press` hook, called
    from its `caller` after a new A press, calls its `stop` function with the same r3 and answers 0; otherwise the original runs."""
    import hashlib, tomllib
    hooks = tomllib.loads(config_text).get('hook', [])
    defs, reports = [], []

    def checked(name, fn, n, digest):
        if n <= 0 or n % 4:
            sys.exit(f'gen_runtime_tables: title hook {name!r}: bad size')
        start = fn - base
        if start < 0 or start + n > len(image):
            sys.exit(f'gen_runtime_tables: title hook {name!r}: {fn:08X} is outside the image')
        got = hashlib.sha256(image[start:start + n]).hexdigest()
        if got != digest:
            sys.exit(f'gen_runtime_tables: title hook {name!r}: the {n} bytes at {fn:08X} hash to {got}, not the recorded {digest}')

    for h in hooks:
        kind = h.get('kind', 'returns')
        fn, n, flag = int(h['function']), int(h['bytes']), str(h['flag'])
        if not FLAG_NAME.fullmatch(flag) or kind not in ('returns', 'stop_on_press'):
            sys.exit(f'gen_runtime_tables: title hook {h["name"]!r}: bad flag or kind')
        checked(h['name'], fn, n, h['sha256'])
        log = f"""    static std::atomic<uint64_t> calls{{0}};
    const uint64_t call = ++calls;
    if (call <= 8 || (call & (call - 1)) == 0)  // the first eight calls, then every power of two
        std::fprintf(stderr, "[hook] {h['name']}: call %llu at vblank %llu (lr=%08X, %s)\\n", (unsigned long long)call,
                     (unsigned long long)runtime::gpu::VblankCount(), uint32_t(ctx.lr), on ? "on" : "original");"""
        if kind == 'returns':
            value = int(h['returns'])
            if not 0 <= value <= 0xFFFFFFFF:
                sys.exit(f'gen_runtime_tables: title hook {h["name"]!r}: bad value')
            body = f"""    if (on)
    {{
        ctx.r3.u64 = 0x{value:X}u;
        return;
    }}"""
            what = f'-> {value}'
        else:
            # stop_on_press: called from `caller` (its return address), a new A press since this caller's calls began makes the hook
            # call `stop` with the same r3 and answer 0; a gap of over a second in the calls starts a new baseline (a new screen).
            caller, stop = int(h['caller']), int(h['stop'])
            checked(h['name'] + ' (stop)', stop, int(h['stop_bytes']), h['stop_sha256'])
            body = f"""    if (on && uint32_t(ctx.lr) == 0x{caller:08X}u)
    {{
        static uint64_t baseline = 0, lastVblank = 0;
        const uint64_t now = runtime::gpu::VblankCount(), presses = runtime::input::APresses();
        if (lastVblank == 0 || now > lastVblank + 60)
            baseline = presses;
        lastVblank = now;
        if (presses > baseline)
        {{
            baseline = presses;
            std::fprintf(stderr, "[hook] {h['name']}: A pressed at vblank %llu: sub_{stop:08X}\\n", (unsigned long long)now);
            sub_{stop:08X}(ctx, base);
            ctx.r3.u64 = 0;
            return;
        }}
    }}"""
            what = f'A from {caller:08X} -> sub_{stop:08X}, 0'
        defs.append(f"""PPC_FUNC_IMPL(__imp__sub_{fn:08X});  // the translation of the original (extern "C", ppc_recomp_*.cpp)
PPC_FUNC(sub_{fn:08X})  // {h['name']} (config/title_hooks.toml)
{{
    static const bool on = HookFlag("{flag}");
{log}
{body}
    __imp__sub_{fn:08X}(ctx, base);
}}""")
        reports.append(f'    {{"{h["name"]}", "{flag}", 0x{fn:08X}u, "{what}"}},')
    return f"""// Generated by scripts/gen_runtime_tables.py from config/title_hooks.toml and the player's own default.xex; do not edit.
// Title hooks: strong definitions that override the translation's weak ones (ppc_recomp_shared.h); each is opt-in by its flag.
#include "graphics.h"
#include "input_live.h"
#include "ppc_recomp_shared.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {{
bool HookFlag(const char* name)
{{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}}
struct HookReport {{ const char* name; const char* flag; uint32_t function; const char* what; }};
constexpr HookReport kHooks[] = {{
{chr(10).join(reports) if reports else '    {nullptr, nullptr, 0, nullptr},'}
}};
}}  // namespace

namespace fm4 {{
int TitleHookCount() {{ return {len(hooks)}; }}
void ReportTitleHooks()
{{
    for (const HookReport& h : kHooks)
        if (h.name != nullptr)
            std::fprintf(stderr, "[hook] %s (sub_%08X %s): %s\\n", h.name, h.function, h.what,
                         HookFlag(h.flag) ? "on" : "off (set the flag to 1 to enable)");
}}
}}  // namespace fm4

{chr(10).join(defs)}
"""


def main():
    args = sys.argv[1:]
    hooks_config = None
    if '--title-hooks' in args:
        i = args.index('--title-hooks')
        if i + 1 >= len(args):
            sys.exit('gen_runtime_tables: --title-hooks needs a file')
        hooks_config = Path(args[i + 1])
        del args[i:i + 2]
    if len(args) < 4 or (len(args) - 4) % 5 or any(a != '--module' for a in args[4::5]):
        sys.exit('usage: gen_runtime_tables.py <survey dir> <ppc dir> <default.xex> <out dir> '
                 '[--module <name> <survey dir> <ppc dir> <xex>]... [--title-hooks <toml>]')
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
    texts['fm4_hooks.cpp'] = title_hooks(hooks_config.read_text() if hooks_config else '', image, report['base'])
    texts.update({'fm4_image.h': header, 'fm4_imports.cpp': source})
    for name, text in texts.items():
        path = out / name
        if not path.exists() or path.read_text() != text:  # unchanged output keeps dependants built
            path.write_text(text)


if __name__ == '__main__':
    main()
