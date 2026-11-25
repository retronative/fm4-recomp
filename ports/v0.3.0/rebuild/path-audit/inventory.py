#!/usr/bin/env python3
"""Inspect only Mach-O cstrings containing private build paths, preserving symbol tables."""
import argparse
import collections
import json
from pathlib import Path
import re
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--package", type=Path, required=True)
parser.add_argument("--cache", type=Path, required=True)
parser.add_argument("--build-result", type=Path, required=True)
parser.add_argument("--receipt", type=Path, required=True)
args = parser.parse_args()
original = json.loads(args.build_result.read_text())["commands"]
patterns = re.compile(rb'(?:/Users/|/private/tmp/|/var/folders/)[^\x00\x0a\x0d]*')
def sections(b):
    if len(b) < 32 or b[:4] != b'\xcf\xfa\xed\xfe':
        return []
    ofs = 32
    result = []
    for _ in range(struct.unpack_from('<I', b, 16)[0]):
        cmd, size = struct.unpack_from('<II', b, ofs)
        if cmd == 25:
            for n in range(struct.unpack_from('<I', b, ofs+64)[0]):
                s = ofs+72+80*n
                name = b[s:s+16].split(b'\0')[0].decode()
                length = struct.unpack_from('<Q',b,s+40)[0]
                offset = struct.unpack_from('<I',b,s+48)[0]
                result.append((name, offset, length))
        ofs += size
    return result

def paths(b):
    return {m[0] for name,offset,length in sections(b) if name=='__cstring'
            for m in patterns.finditer(b[offset:offset+length])}

def members(b):
    if b[:8] != b'!<arch>\n':
        raise ValueError("Unsupported archive")
    offset = 8
    while offset < len(b):
        size = int(b[offset+48:offset+58])
        tag = b[offset:offset+16].decode().strip()
        payload = b[offset+60:offset+60+size]
        if tag.startswith('#1/'):
            n = int(tag[3:]); name = payload[:n].rstrip(b'\0').decode(); payload = payload[n:]
        else: name = tag.rstrip('/')
        if payload[:4] in (b'\xcf\xfa\xed\xfe', bytes.fromhex("dec0170b"), b"BC\xc0\xde"):
            yield name, payload
        offset += 60 + size + size%2

def matches(blob, strings, object_file=None):
    if blob[:4] in (bytes.fromhex("dec0170b"), b"BC\xc0\xde"):
        if object_file is None:
            return {s for s in strings if s in blob}
        assembly = subprocess.check_output(["xcrun", "clang", "-S", "-emit-llvm", "-x", "ir",
                                            str(object_file), "-o", "-"], stderr=subprocess.DEVNULL)
        # The LLVM writer abbreviates common bitcode strings. Decode static
        # constants as text to recover the strings actually handed to LTO.
        literals = b"\n".join(l for l in assembly.splitlines() if b"constant " in l and b'c"' in l)
        return {s for s in strings if s in literals}
    return paths(blob) & strings

report = {}
for target,cmd in original.items():
    if target.endswith('.o'):
        continue
    name = Path(target).name
    final = (args.package / name).read_bytes()
    strings = paths(final)
    found = set()
    totals = collections.Counter()
    groups = collections.defaultdict(list)
    used = list(dict.fromkeys(token for token in cmd if token.endswith(('.o','.a'))))
    for token in used:
        p = args.cache / token
        if p.name in {'liblibavcodec.a','liblibavutil.a','libmspack.a'}:
            continue
        data=p.read_bytes()
        if p.suffix=='.a':
            for member,blob in members(data):
                if matches(blob, strings):
                    found.update(matches(blob,strings)); groups[p.name].append(member)
        elif (object_matches := matches(data, strings, p)):
            group = 'guest' if '/.local/generated/' in token or 'recomp.dir/.local/' in token else 'host/SDK'
            found.update(object_matches)
            counts = len(object_matches)
            groups[group].append({'object': str(token).split('/objects/')[-1],'matching_strings': counts})
            totals[group] += p.stat().st_size
    report[name] = {'cstring_paths': len(strings),'total_occurrences':len(list(patterns.finditer(final))),
                    'unassigned_strings':[s.decode() for s in strings-found],
                    'input_count': len([t for t in used if t.endswith('.o')]),
                    'affected_inputs':dict(groups), 'combined_object_bytes':dict(totals)}
    print(name, 'distinct_cstrings',len(strings),'all_obj',report[name]['input_count'],
          'affected',{k:len(v) for k,v in groups.items()}, 'MiB', {k:round(v/(1024*1024),1) for k,v in totals.items()})
args.receipt.write_text(json.dumps(report,indent=2)+'\n')
