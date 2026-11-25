#!/usr/bin/env python3
"""Compare ordinary symbols, linkage, code signatures and private path bytes across release products."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess

def out(*cmd):
    return subprocess.check_output(cmd, text=True)

def rpaths(file):
    return re.findall(r"\bcmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", out("otool", "-l", str(file)))

def named_symbols(path, flags):
    return Counter(out("nm", *flags, str(path)).splitlines())

def qt(name, original, candidate):
    old, new = original / name, candidate / name
    b = new.read_bytes()
    paths = {s.decode(): b.count(s) for s in (b"/Users/", b"/private/tmp/", b"/var/folders/")}
    exports = named_symbols(old, ["-gUj"]) == named_symbols(new, ["-gUj"])
    imports = named_symbols(old, ["-uj"]) == named_symbols(new, ["-uj"])
    base = named_symbols(old, ["-j"])
    actual = named_symbols(new, ["-j"])
    old_loads = out("otool", "-L", str(old)).splitlines()[1:]
    new_loads = out("otool", "-L", str(new)).splitlines()[1:]
    same_loads = old_loads == new_loads and rpaths(old) == rpaths(new)
    verified = subprocess.run(["codesign", "--verify", "--strict", str(new)]).returncode == 0
    result = dict(candidate_sha256=hashlib.sha256(b).hexdigest(), bytes=len(b), private_path_occurrences=paths,
                  exports_equal=exports, imports_equal=imports, load_commands_equal=same_loads,
                  signature_verified=verified, symbol_count_before=sum(base.values()),
                  symbol_count_after=sum(actual.values()), ordinary_symbol_multiset_equal=base == actual,
                  removed_symbols=sum((base-actual).values()), added_symbols=sum((actual-base).values()))
    success = (not any(paths.values()) and exports and imports and same_loads and verified)
    return result, success

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--reference", type=Path, required=True)
parser.add_argument("--candidate", type=Path, required=True)
parser.add_argument("--receipt", type=Path, required=True)
args = parser.parse_args()
report = {}
ok = True
for path in sorted(args.candidate.iterdir()):
    if path.name == "fm4_static_recomp" or path.suffix == ".dylib":
        report[path.name], checked = qt(path.name, args.reference, args.candidate)
        ok = ok and checked
if len(report) != 8:
    raise ValueError(f"Expected 8 images, found {len(report)}")
args.receipt.write_text(json.dumps(report, indent=2) + "\n")
for name,r in report.items():
    print(name, "paths", sum(r["private_path_occurrences"].values()), "symbols", r["symbol_count_before"],
          r["symbol_count_after"], "identical names", r["ordinary_symbol_multiset_equal"])
if not ok:
    raise SystemExit("One or more images failed verification")
