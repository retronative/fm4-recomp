#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Checks the 24 prompt string keys (runtime/kernel/keyboard_prompts.h kPromptKeys) against OUR disc's English string tables: each
# key must name the expected IDS_ entry in the expected table, and its text is written to a TSV (table, key, name, text) for
# tests/run_keyboard_prompt_tests.sh <tsv> and for review. Game text: write the TSV under research/, never commit it.
#
# The .str format (LSB2) as the reference's test reads it (FM4 Static Recomp 0.3.x host/tests/string_tables_test.cpp:41-75, BSD-3; facts
# only): "LSB2" at 0; at +0x0C the texts' chunk offset: [size u32][count u32], then (u16 key, u32 character offset) x count and a
# sentinel entry, then a UTF-16BE pool (a text runs to a 0 character); at +0x18 the names' chunk offset: [size][count],
# (u16 key, u16 index, u32 byte offset) x count, then an ASCII pool. Big-endian throughout.
#
# The EN tables are LZX zip members (method 21), so first extract them with our own LDI decoder (no game bytes leave the machine):
# (from the repository root; the folder name is the zip path with '/' as '_', tests/ldi/crosscheck.cpp:12-13):
#   LDI_EXTRACT=research/keyboard_prompts/str LDI_EXTRACT_SUFFIX=.str \
#       tests/ldi/run_crosscheck.sh research/keyboard_prompts/ldi game/files/Media/StringTables/EN.zip
#   /opt/homebrew/bin/python3 -B -I tests/check_prompt_string_keys.py \
#       research/keyboard_prompts/str/game_files_Media_StringTables_EN.zip research/keyboard_prompts/en_prompts.tsv
#   /opt/homebrew/bin/python3 -B -I tests/check_prompt_string_keys.py --self-test
import struct
import sys
from pathlib import Path

# kPromptKeys with the reference's table and name for each (prompt_strings.cpp:22-47, string_tables_test.cpp:136-159).
EXPECTED = [
    (0x3D74, 'carexperience.str', 'IDS_Explore_Description'),
    (0x42FB, 'carexperience.str', 'IDS_Interact_Description1'),
    (0x437B, 'carexperience.str', 'IDS_Interact_Description2'),
    (0xBA58, 'carexperience.str', 'IDS_Getin_Description'),
    (0xDE8C, 'carexperience.str', 'IDS_Move_Description2'),
    (0xDF0C, 'carexperience.str', 'IDS_Move_Description1'),
    (0xEC34, 'carexperience.str', 'IDS_Look_Description2'),
    (0xEDB4, 'carexperience.str', 'IDS_Look_Description1'),
    (0x4D92, 'ingame.str', 'IDS_Accelerate_Message'),
    (0x76B3, 'ingame.str', 'IDS_View_Message'),
    (0xF762, 'ingame.str', 'IDS_Steering_Message'),
    (0xFC27, 'lobby.str', 'IDS_OptionBucketNameDesc'),
    (0x5222, 'main.str', 'IDS_LB'),
    (0xD225, 'main.str', 'IDS_RB'),
    (0xC0F5, 'main.str', 'IDS_Rivals_Message'),
    (0x90AB, 'mainmenu.str', 'IDS_A_Start'),
    (0x206F, 'myprofile.str', 'IDS_Rewind_Message'),
    (0x6877, 'myprofile.str', 'IDS_Controller_Discription'),
    (0x63B4, 'storefront.str', 'IDS_GamertagDefault'),
    (0xE49E, 'storefront.str', 'IDS_ManualKeywordDefault'),
    (0x2ADE, 'telemetry.str', 'IDS_Press_Dpad_Down'),
    (0xD422, 'tuning.str', 'IDS_SetupFileLocked_Remove_Msg'),
    (0xEC3B, 'upgradeareas.str', 'IDS_Description_3'),
    (0xED3B, 'upgradeareas.str', 'IDS_Description_1'),
]


def parse(data):
    """{key: [name, text]} of an LSB2 table; ValueError when it is not one."""
    if len(data) < 0x1C or data[:4] != b'LSB2':
        raise ValueError('not an LSB2 string table')
    u16 = lambda at: struct.unpack_from('>H', data, at)[0]
    u32 = lambda at: struct.unpack_from('>I', data, at)[0]
    entries = {}
    texts = u32(0x0C)
    count = u32(texts + 4)
    pool = texts + 8 + 6 * (count + 1)
    if pool > len(data):
        raise ValueError('texts chunk past the end')
    for i in range(count):
        entry = texts + 8 + 6 * i
        at, chars = pool + 2 * u32(entry + 2), []
        while at + 1 < len(data) and u16(at):
            chars.append(chr(u16(at)))
            at += 2
        entries[u16(entry)] = ['', ''.join(chars)]
    names = u32(0x18)
    if names and names + 8 <= len(data) and u32(names + 4) == count:
        name_pool = names + 8 + 8 * count
        for i in range(count):
            entry = names + 8 + 8 * i
            at = name_pool + u32(entry + 4)
            end = data.index(b'\0', at) if b'\0' in data[at:] else len(data)
            entries.setdefault(u16(entry), ['', ''])[0] = data[at:end].decode('ascii')
    return entries


def escaped(text):
    """One TSV field: backslash, tab, line breaks and non-ASCII as escapes (\\, \\t, \\n, \\r, \\uXXXX), so a multi-line text keeps
    its row. The test reads the field as is: an escape is never a phrase, and the phrases are ASCII on one line."""
    out = []
    for c in text:
        if c == '\\':
            out.append('\\\\')
        elif c in '\t\n\r':
            out.append({'\t': '\\t', '\n': '\\n', '\r': '\\r'}[c])
        elif not (' ' <= c <= '~'):
            out.append(f'\\u{ord(c):04X}')
        else:
            out.append(c)
    return ''.join(out)


def check(str_dir, out_tsv):
    files = {p.name.lower(): p for p in Path(str_dir).iterdir() if p.is_file()}
    tables, rows, problems = {}, [], []
    for key, table, name in EXPECTED:
        if table not in files:
            problems.append(f'{table}: not in {str_dir}')
            continue
        if table not in tables:
            tables[table] = parse(files[table].read_bytes())
        entry = tables[table].get(key)
        if entry is None:
            problems.append(f'{table} {key:04X}: no such key')
            continue
        if entry[0] != name:
            problems.append(f'{table} {key:04X}: named {entry[0]!r}, not {name!r}')
        rows.append(f'{table}\t{key:04X}\t{entry[0]}\t{escaped(entry[1])}')
    if out_tsv.exists():
        sys.exit(f'{out_tsv} exists: refusing to overwrite')
    out_tsv.write_text('\n'.join(rows) + '\n', encoding='utf-8')
    for row in rows:
        print(row)
    for p in problems:
        print('PROBLEM', p)
    print(f'{len(rows)} of {len(EXPECTED)} keys read, {len(problems)} problems')
    return 1 if problems else 0


def synthetic(entries):
    """An LSB2 table built from [(key, name, text)] by the layout above (written independently of parse)."""
    pool_chars, offsets = [], []
    for _, _, text in entries:
        offsets.append(len(pool_chars))
        pool_chars += [ord(c) for c in text] + [0]
    texts = 0x20
    body = struct.pack('>II', 0, len(entries)) + b''.join(struct.pack('>HI', k, o) for (k, _, _), o in zip(entries, offsets))
    body += struct.pack('>HI', 0xFFFF, len(pool_chars)) + b''.join(struct.pack('>H', c) for c in pool_chars)
    names = texts + len(body)
    name_pool, name_offsets = b'', []
    for _, name, _ in entries:
        name_offsets.append(len(name_pool))
        name_pool += name.encode('ascii') + b'\0'
    chunk = struct.pack('>II', 0, len(entries)) + b''.join(struct.pack('>HHI', k, i, o) for i, ((k, _, _), o) in enumerate(zip(entries, name_offsets)))
    header = b'LSB2' + b'\0' * 8 + struct.pack('>I', texts) + b'\0' * 8 + struct.pack('>I', names) + b'\0' * 4
    assert len(header) == texts
    return header + body + chunk + name_pool


def self_test():
    table = parse(synthetic([(0x90AB, 'IDS_A_Start', 'Press A to go.'), (0x1234, 'IDS_Other', 'x')]))
    ok = table == {0x90AB: ['IDS_A_Start', 'Press A to go.'], 0x1234: ['IDS_Other', 'x']}
    esc = escaped('a\tb\nc\\d\u00e9') == 'a\\tb\\nc\\\\d\\u00E9'
    try:
        parse(b'LSB1' + b'\0' * 40)
        refused = False
    except ValueError:
        refused = True
    print('pass' if ok else 'FAIL', 'a synthetic two-entry table: keys, names and texts')
    print('pass' if refused else 'FAIL', 'a table without LSB2 is refused (CONTROL)')
    print('pass' if esc else 'FAIL', 'a TSV field escapes tab, newline, backslash and non-ASCII')
    return 0 if ok and refused and esc else 1


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        sys.exit(self_test())
    if len(sys.argv) != 3:
        sys.exit('usage: check_prompt_string_keys.py <extracted EN .str dir> <out tsv> | --self-test')
    sys.exit(check(sys.argv[1], Path(sys.argv[2])))
