#!/usr/bin/env python3
"""Verify runtime's local dylib linkage, original engine exports and a stored LZX block."""
import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--libraries", type=Path, required=True)
parser.add_argument("--original", type=Path, required=True, help="Matching original static runtime")
parser.add_argument("--consumers", type=Path, help="Folder containing original engine binaries to check")
args = parser.parse_args()
libs = args.libraries.resolve()
runtime = libs / "librexruntime.dylib"
names = ("libavutil.56.dylib", "libavcodec.58.dylib", "libshiftglue_lzx.dylib")
def output(*argv):
    return subprocess.check_output(argv, text=True)

def exports(path):
    return {l.split()[-1] for l in output("nm", "-gU", str(path)).splitlines() if l}

for name in ("librexruntime.dylib", *names):
    path = libs / name
    subprocess.run(["codesign", "--verify", "--strict", str(path)], check=True)
    print(name, path.stat().st_size, "bytes", hashlib.sha256(path.read_bytes()).hexdigest())
loads = output("otool", "-L", str(runtime))
for name in names:
    assert "\t@rpath/" + name + " (" in loads, (name, loads)
assert "path @loader_path (" in output("otool", "-l", str(runtime))
assert "\t@rpath/libavutil.56.dylib (" in output("otool", "-L", str(libs / names[1]))
symbol_lines = output("nm", "-m", str(runtime)).splitlines()
matching = [l for l in symbol_lines if re.search(r' _(av(?:codec_|_)|ff_xmaframes_decoder|lzxd_)', l)]
assert matching and all("(undefined)" in line for line in matching)
removed = exports(args.original) - exports(runtime)
assert removed and all(s.startswith(("_ff_", "_lzxd_")) for s in removed), removed
if args.consumers:
    for path in args.consumers.iterdir():
        if not path.is_file() or path.name == "librexruntime.dylib":
            continue
        if path.suffix == ".dylib" or path.name == "fm4_static_recomp":
            imports = set(output("nm", "-uj", str(path)).splitlines())
            assert not (imports & removed), (path, imports & removed)
print(f"{len(matching)} dynamic imports; engine export differences confined to {len(removed)} codec/LZX symbols")
dll = ctypes.CDLL(str(runtime), mode=os.RTLD_NOW | os.RTLD_LOCAL)
decode = getattr(dll, "_Z14lzx_decompressPKvmPvmjS1_m")
decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                   ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t]
decode.restype = ctypes.c_int
plain = b"FM4 runtime LZX check.\n" * 7
bits = "0" + "011" + f"{len(plain):024b}"
bits += "0" * (-len(bits) % 16)
packet = (b"".join(int(bits[i:i+16], 2).to_bytes(2, "little") for i in range(0, len(bits), 16))
          + struct.pack("<III", 1, 1, 1) + plain)
out = ctypes.create_string_buffer(len(plain))
assert decode(packet, len(packet), out, len(plain), 1 << 15, None, 0) == 0 and out.raw == plain
print("Runtime LZX call returned all 161 original fixture bytes")
