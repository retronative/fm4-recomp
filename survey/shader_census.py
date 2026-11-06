# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Offline shader census: parse every .fxobj under a directory with survey/fxobj.py and write a
# JSON inventory (per file: containers, stage, microcode size, hash; totals; distinct shaders per
# stage; errors). Reads the game files only; the output holds hashes and sizes, no game bytes.
#
#   /opt/homebrew/bin/python3 -I survey/shader_census.py <shader dir> <out.json>
#
# Deterministic: directories and files are walked in sorted order, paths are relative with "/",
# and every list is sorted. Exit status 1 when any file has an error (the JSON is still written).
import hashlib
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("fxobj", os.path.join(HERE, "fxobj.py"))
fxobj = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fxobj)


def walk(root):
    fx, other = [], []
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            rel = os.path.relpath(os.path.join(d, name), root).replace(os.sep, "/")
            (fx if name.lower().endswith(".fxobj") else other).append(rel)
    return sorted(fx), sorted(other)


def census(root):
    fx, other = walk(root)
    files, distinct = [], {"vs": {}, "ps": {}}
    totals = {"files": len(fx), "files_with_errors": 0, "bytes": 0, "errors": 0,
              "containers": {"vs": 0, "ps": 0}, "ucode_bytes": {"vs": 0, "ps": 0}}
    for rel in fx:
        with open(os.path.join(root, rel), "rb") as f:
            data = f.read()
        info = fxobj.parse_fxobj(data)
        totals["bytes"] += len(data)
        totals["errors"] += len(info["errors"])
        totals["files_with_errors"] += bool(info["errors"])
        containers = []
        for k in info["containers"]:
            cf = k["cf"]
            containers.append({"offset": k["offset"], "stage": k["stage"], "target": k["ctab"]["target"],
                               "creator": k["ctab"]["creator"], "ucode_bytes": k["ucode_bytes"],
                               "literal_bytes": k["literal_bytes"], "hash": k["hash"]})
            totals["containers"][k["stage"]] += 1
            totals["ucode_bytes"][k["stage"]] += k["ucode_bytes"]
            d = distinct[k["stage"]].get(k["hash"])
            if d is None:
                d = distinct[k["stage"]][k["hash"]] = {
                    "hash": k["hash"], "ucode_dwords": k["ucode_dwords"], "literal_bytes": k["literal_bytes"],
                    "alu": cf["alu"], "vfetch": cf["vfetch"], "tfetch": cf["tfetch"],
                    "fetch_other": cf["fetch_other"], "loops": cf["loops"], "calls": cf["calls"],
                    "jumps": cf["jumps"], "conditional": cf["conditional"], "allocs": cf["allocs"],
                    "samplers": sorted(c["name"] for c in k["ctab"]["constants"] if c["set"] == "sampler"),
                    "creators": [], "occurrences": 0, "first": {"path": rel, "offset": k["offset"]}}
            d["occurrences"] += 1
            if k["ctab"]["creator"] not in d["creators"]:
                d["creators"] = sorted(d["creators"] + [k["ctab"]["creator"]])
        files.append({"path": rel, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                      "outer": info["outer"], "containers": containers, "errors": info["errors"]})
    if not fx:
        totals["errors"] += 1
    stages = {s: sorted(distinct[s].values(), key=lambda d: d["hash"]) for s in ("vs", "ps")}
    totals["distinct"] = {s: len(stages[s]) for s in stages}
    totals["distinct_with_loops"] = {s: sum(d["loops"] for d in stages[s]) for s in stages}
    return {"format": 1, "files": files, "other_files": other, "totals": totals, "distinct": stages,
            "errors": ([] if fx else ["no .fxobj file"]) +
                      ["%s: %s" % (f["path"], e) for f in files for e in f["errors"]]}


def main(argv):
    if len(argv) != 3:
        print("usage: shader_census.py <shader dir> <out.json>", file=sys.stderr)
        return 2
    if not os.path.isdir(argv[1]):
        print("not a directory: %s" % argv[1], file=sys.stderr)
        return 2
    result = census(argv[1])
    with open(argv[2], "w") as f:
        json.dump(result, f, indent=1, sort_keys=True)
        f.write("\n")
    t = result["totals"]
    print("files %d (%d bytes, %d with errors)  containers vs %d ps %d  distinct vs %d ps %d  "
          "loops vs %d ps %d  errors %d"
          % (t["files"], t["bytes"], t["files_with_errors"], t["containers"]["vs"], t["containers"]["ps"],
             t["distinct"]["vs"], t["distinct"]["ps"], t["distinct_with_loops"]["vs"],
             t["distinct_with_loops"]["ps"], t["errors"]))
    for e in result["errors"]:
        print("ERROR " + e)
    return 1 if result["errors"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
