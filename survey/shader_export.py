# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Export every distinct shader FM4 ships for the offline translation census
# (tests/shader_census/census.cpp): parse every .fxobj under a directory with survey/fxobj.py
# and write, into a new output folder (under research/, never committed: it holds game bytes):
#   <vs|ps>_<hash>.bin   the microcode exactly as stored on the disc (big-endian dwords)
#   index.json           per distinct shader: stage, Xenos ShaderType, hash, dwords, file name,
#                        and every occurrence (file, container offset, target, creator, the
#                        constant table's constants, the literal block as hex) plus whether all
#                        occurrences agree on constants and literals.
#
#   /opt/homebrew/bin/python3 -B -I survey/shader_export.py <shader dir> <out dir>
#
# The translator (runtime/shader/shader_glsl.h) needs only the microcode and the stage; the
# constants and literal blocks are recorded for the host side (which constants a draw must set),
# not consumed by the census. Fail closed: any parse error, or two containers with one hash and
# different microcode, exits 1 and writes nothing. The output folder must not exist or be empty.
# Deterministic: shaders sorted by (stage, hash), occurrences by (path, offset), sort_keys JSON.
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fxobj = load("fxobj")
shader_census = load("shader_census")


class ExportError(ValueError):
    pass


def collect(root):
    """Returns {(stage, hash): {"ucode": bytes, "occurrences": [...]}}; raises ExportError."""
    fx, _ = shader_census.walk(root)
    if not fx:
        raise ExportError("no .fxobj file under %s" % root)
    shaders, errors = {}, []
    for rel in fx:
        with open(os.path.join(root, rel), "rb") as f:
            data = f.read()
        info = fxobj.parse_fxobj(data)
        errors += ["%s: %s" % (rel, e) for e in info["errors"]]
        for k in info["containers"]:
            ucode = data[k["ucode_offset"]:k["ucode_offset"] + k["ucode_bytes"]]
            literal_off = k["ucode_offset"] - k["literal_bytes"]
            occurrence = {"path": rel, "offset": k["offset"], "target": k["ctab"]["target"],
                          "creator": k["ctab"]["creator"], "constants": k["ctab"]["constants"],
                          "literals": data[literal_off:k["ucode_offset"]].hex()}
            entry = shaders.setdefault((k["stage"], k["hash"]), {"ucode": ucode, "occurrences": []})
            if entry["ucode"] != ucode:
                errors.append("%s: container at 0x%X: %s hash %s already seen with different microcode"
                              % (rel, k["offset"], k["stage"], k["hash"]))
            entry["occurrences"].append(occurrence)
    if errors:
        raise ExportError("\n".join(errors))
    return shaders


def file_name(stage, hash_hex):
    return "%s_%s.bin" % (stage, hash_hex)


def index(shaders):
    out = []
    for (stage, hash_hex), entry in sorted(shaders.items()):
        occ = sorted(entry["occurrences"], key=lambda o: (o["path"], o["offset"]))
        out.append({"stage": stage, "type": fxobj.SHADER_TYPE[stage], "hash": hash_hex,
                    "dwords": len(entry["ucode"]) // 4, "file": file_name(stage, hash_hex),
                    "occurrences": occ,
                    "constants_agree": all(o["constants"] == occ[0]["constants"] for o in occ),
                    "literals_agree": all(o["literals"] == occ[0]["literals"] for o in occ)})
    return out


def export(root, out_dir):
    if os.path.exists(out_dir) and (not os.path.isdir(out_dir) or os.listdir(out_dir)):
        raise ExportError("output %s exists and is not an empty directory" % out_dir)
    shaders = collect(root)
    entries = index(shaders)
    os.makedirs(out_dir, exist_ok=True)
    for (stage, hash_hex), entry in sorted(shaders.items()):
        with open(os.path.join(out_dir, file_name(stage, hash_hex)), "xb") as f:
            f.write(entry["ucode"])
    counts = {s: sum(e["stage"] == s for e in entries) for s in ("vs", "ps")}
    with open(os.path.join(out_dir, "index.json"), "x") as f:
        json.dump({"format": 1, "counts": counts, "shaders": entries}, f, indent=1, sort_keys=True)
        f.write("\n")
    return counts


def main(argv):
    if len(argv) != 3:
        print("usage: shader_export.py <shader dir> <out dir>", file=sys.stderr)
        return 2
    if not os.path.isdir(argv[1]):
        print("not a directory: %s" % argv[1], file=sys.stderr)
        return 2
    try:
        counts = export(argv[1], argv[2])
    except ExportError as e:
        for line in str(e).splitlines():
            print("ERROR " + line, file=sys.stderr)
        return 1
    print("exported vs %d ps %d to %s" % (counts["vs"], counts["ps"], argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
