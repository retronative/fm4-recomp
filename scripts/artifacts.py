#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""Storage inventory and reviewed cleanup of this repository's test artifacts.

  /opt/homebrew/bin/python3 -I scripts/artifacts.py inventory            # sizes by class (read-only)
  /opt/homebrew/bin/python3 -I scripts/artifacts.py plan                 # dry run: writes a manifest
  /opt/homebrew/bin/python3 -I scripts/artifacts.py apply --manifest F --approve ID

Method: dry run first, a
reviewed manifest and its ID to delete, every path re-validated before deletion, an audit log.

Candidates (only under build/, never followed through a symlink):
  - per-run test outputs (build/tests/{translator,loader,analysis} and the same in the gate's
    scratch tree build/mutate/build/tests/): runners remove them after a passing run, so what
    remains is from a failing run or from before that rule (status not recorded): listed for
    review, never deleted without approval;
  - directories whose .retention.json says class temp-build, status done.
Never candidates: research/ (evidence), ppc/, game/, tools/, sources, the gate's scratch sources
and XenonRecomp build (they keep the next run incremental), the harness caches (capped by
size-based eviction instead), unmarked data. Nothing is touched while a gate or a test runner is
running (a live build/mutate/.gate.lock, or a matching process).
"""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_OUTPUTS = ["build/tests/translator", "build/tests/loader", "build/tests/analysis",
               "build/mutate/build/tests/translator", "build/mutate/build/tests/loader",
               "build/mutate/build/tests/analysis"]
CACHES = ["build/tests/translator_cache", "build/mutate/build/tests/translator_cache"]
RUNNERS = ["tests/mutate.py", "run_translator_tests.sh", "run_loader_tests.sh", "test_analysis.py"]
INVENTORY = [  # (path, class)
    ("build/tests", "per-run outputs + harness cache"),
    ("build/mutate", "gate scratch tree (sources, XenonRecomp build, outputs, cache)"),
    ("ppc", "recompile output (regenerable; current)"),
    ("research", "evidence (kept)"),
    ("tools", "toolchain sources and builds (kept)"),
    ("game", "game data (protected)"),
]


def size(path):
    if os.path.islink(path):
        return 0
    if os.path.isfile(path):
        return os.path.getsize(path)
    total = 0
    for dirpath, _, files in os.walk(path):
        for name in files:
            fp = os.path.join(dirpath, name)
            if not os.path.islink(fp):
                total += os.path.getsize(fp)
    return total


def mib(n):
    return "%.1f MiB" % (n / 1048576)


def active_runs(root, patterns=None):
    """Reasons nothing may be touched now: a live gate lock, or a running gate/test runner."""
    reasons = []
    lock = os.path.join(root, "build", "mutate", ".gate.lock")
    if os.path.exists(lock):
        try:
            with open(lock) as f:
                pid = int(f.read().strip())
            os.kill(pid, 0)
            reasons.append("gate lock held by live pid %d" % pid)
        except (ValueError, ProcessLookupError):
            pass
        except PermissionError:
            reasons.append("gate lock held by another user's process")
    for pattern in RUNNERS if patterns is None else patterns:
        out = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True).stdout.split()
        pids = [p for p in out if int(p) != os.getpid()]
        if pids:
            reasons.append("running: %s (pid %s)" % (pattern, ", ".join(pids)))
    return reasons


def marker(path):
    try:
        with open(os.path.join(path, ".retention.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def candidates(root):
    items = []
    for rel in RUN_OUTPUTS:
        p = os.path.join(root, rel)
        if os.path.isdir(p) and not os.path.islink(p):
            items.append(dict(path=rel, reason="per-run test outputs left by a failing or pre-cleanup run "
                              "(status not recorded): review before deleting"))
    build = os.path.join(root, "build")
    if os.path.isdir(build):
        for name in sorted(os.listdir(build)):
            p = os.path.join(build, name)
            m = marker(p) if os.path.isdir(p) and not os.path.islink(p) else None
            if m and m.get("class") == "temp-build" and m.get("status") == "done":
                items.append(dict(path="build/" + name, reason="temp-build, done %s: %s"
                                  % (m.get("done_at", "?"), m.get("note", ""))))
    for it in items:
        p = os.path.join(root, it["path"])
        it["inode"] = os.stat(p).st_ino
        it["bytes"] = size(p)
    return items


def manifest_id(items):
    canon = json.dumps([[i["path"], i["inode"], i["bytes"]] for i in items], sort_keys=True)
    return hashlib.sha256(canon.encode()).hexdigest()[:12]


def duplicates(root, rel="research"):
    """Identical files under research/ (reported only: evidence is never deleted by this tool)."""
    seen = {}
    base = os.path.join(root, rel)
    for dirpath, _, files in os.walk(base):
        for name in files:
            fp = os.path.join(dirpath, name)
            if os.path.islink(fp) or name == ".retention.json":
                continue
            with open(fp, "rb") as f:
                h = hashlib.sha256(f.read()).hexdigest()
            seen.setdefault(h, []).append(os.path.relpath(fp, root))
    return [sorted(v) for v in seen.values() if len(v) > 1]


def validate(root, it):
    """Why this manifest item may not be deleted now, or None."""
    rel = it["path"]
    p = os.path.join(root, rel)
    real_build = os.path.realpath(os.path.join(root, "build"))
    if os.path.isabs(rel) or ".." in rel.split("/") or not rel.startswith("build/"):
        return "not a path under build/"
    if os.path.islink(p):
        return "symlink"
    if not os.path.isdir(p):
        return "gone"
    if not os.path.realpath(p).startswith(real_build + os.sep):
        return "resolves outside build/"
    if os.stat(p).st_ino != it["inode"]:
        return "replaced since the plan (inode changed)"
    if rel not in RUN_OUTPUTS:
        m = marker(p)
        if not (m and m.get("class") == "temp-build" and m.get("status") == "done"):
            return "marker missing or no longer temp-build/done"
    return None


def cmd_inventory(root):
    total = 0
    for rel, cls in INVENTORY:
        p = os.path.join(root, rel)
        n = size(p) if os.path.exists(p) else 0
        total += n
        print("%-14s %12s  %s" % (rel, mib(n), cls))
    for rel in RUN_OUTPUTS + CACHES:
        p = os.path.join(root, rel)
        if os.path.exists(p):
            print("  %-40s %12s" % (rel, mib(size(p))))
    print("%-14s %12s" % ("total", mib(total)))
    return 0


def cmd_plan(root, out):
    items = candidates(root)
    mid = manifest_id(items)
    for it in items:
        print("candidate %-40s %12s  %s" % (it["path"], mib(it["bytes"]), it["reason"]))
    for group in duplicates(root):
        print("duplicate (kept, report only): " + ", ".join(group))
    busy = active_runs(root)
    for r in busy:
        print("held: " + r)
    print("plan: %d candidates, %s; manifest %s, ID %s (dry run: nothing deleted)"
          % (len(items), mib(sum(i["bytes"] for i in items)), out, mid))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w") as f:
        json.dump(dict(id=mid, root=root, created=datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                       items=items), f, indent=1)
        f.write("\n")
    return 0


def cmd_apply(root, manifest, approve, audit, patterns=None):
    with open(manifest) as f:
        man = json.load(f)
    if man.get("root") != root:
        print("apply: the manifest is for another root: %s" % man.get("root"), file=sys.stderr)
        return 2
    if manifest_id(man["items"]) != man.get("id") or approve != man.get("id"):
        print("apply: the manifest was edited or the approval ID does not match", file=sys.stderr)
        return 2
    busy = active_runs(root, patterns)
    if busy:
        print("apply: refused, active: " + "; ".join(busy), file=sys.stderr)
        return 2
    freed = 0
    os.makedirs(os.path.dirname(os.path.abspath(audit)), exist_ok=True)
    with open(audit, "a") as log:
        for it in man["items"]:
            why = validate(root, it)
            if why is None:
                n = size(os.path.join(root, it["path"]))
                shutil.rmtree(os.path.join(root, it["path"]))
                freed += n
                result = "deleted"
            else:
                result = "skipped: " + why
            print("%-40s %s" % (it["path"], result))
            log.write(json.dumps(dict(time=datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                                      manifest=man["id"], path=it["path"], bytes=it["bytes"],
                                      reason=it["reason"], result=result)) + "\n")
    print("apply: %s freed" % mib(freed))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=["inventory", "plan", "apply"])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--out", help="plan: manifest path (default research/storage/plan-<time>.json)")
    ap.add_argument("--manifest")
    ap.add_argument("--approve")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    if a.command == "inventory":
        return cmd_inventory(root)
    if a.command == "plan":
        out = a.out or os.path.join(root, "research", "storage",
                                    "plan-%s.json" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
        return cmd_plan(root, out)
    if not (a.manifest and a.approve):
        print("apply needs --manifest and --approve", file=sys.stderr)
        return 2
    return cmd_apply(root, a.manifest, a.approve, os.path.join(root, "research", "storage", "audit.jsonl"))


if __name__ == "__main__":
    sys.exit(main())
