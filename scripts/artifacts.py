#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""Storage inventory and reviewed cleanup of this repository's test artifacts.

  /opt/homebrew/bin/python3 -I scripts/artifacts.py inventory            # sizes by class (read-only)
  /opt/homebrew/bin/python3 -I scripts/artifacts.py plan                 # dry run: writes a manifest
  /opt/homebrew/bin/python3 -I scripts/artifacts.py plan --captures frame-ring[,keep-frames] [--keep s1,s2]
  /opt/homebrew/bin/python3 -I scripts/artifacts.py plan --evidence metal-trace,profile-xml,shader-dumps,saves [--milestone mN] [--before N] [--keep s1,s2]
  /opt/homebrew/bin/python3 -I scripts/artifacts.py apply --manifest F --approve ID

Method: dry run first, a
reviewed manifest and its ID to delete, every path re-validated before deletion, an audit log.

Candidates (only under build/, never followed through a symlink):
  - per-run test outputs (build/tests/{translator,loader,analysis} and the same in the gate's
    scratch tree build/mutate/build/tests/): runners remove them after a passing run, so what
    remains is from a failing run or from before that rule (status not recorded): listed for
    review, never deleted without approval;
  - directories whose .retention.json says class temp-build, status done.
Capture PNGs of recorded GPU runs (only with `plan --captures`): under
research/m<N>/<scenario>/run/<run>.captures, the per-frame ring `frame_<N>.png` (class frame-ring:
the last swaps before the stop) and, only when asked, the every-Nth `keep_<N>.png` (class
keep-frames). A run qualifies when its scenario directory carries a .retention.json (registered
evidence), its log <run>.err lies beside the captures (the recorded result stays) and the scenario
is not named in --keep. Only files of the chosen classes go; the directory, the logs, the shader
dumps, traces, user-data and every other file stay.
Old run evidence (traces, profile XML, shader dumps and saves of old runs; only with `plan --evidence`): in a research/m<N>/scenario<K>/ directory that is marked (.retention.json) and
keeps a run log (run/*.err), the classes metal-trace (<scenario>/*.trace bundles), profile-xml (<scenario>/*.xml exported from
them), shader-dumps (<scenario>/shaders, <scenario>/run/shaders) and saves (<scenario>/user-data, the archived save after a run).
--before K limits it to scenarios numbered below K; --keep names scenarios left out. Run logs, result notes and every other file
stay, and only scenario<K> directories qualify (named saves such as save_c14 never do).
Never candidates: anything else under research/ (evidence), ppc/, game/, tools/, sources, the
gate's scratch sources and XenonRecomp build (they keep the next run incremental), the harness
caches (capped by size-based eviction instead), unmarked data. Nothing is touched while a gate, a
test runner or a game run is running (a live build/mutate/.gate.lock, or a matching process).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_OUTPUTS = ["build/tests/translator", "build/tests/loader", "build/tests/analysis",
               "build/mutate/build/tests/translator", "build/mutate/build/tests/loader",
               "build/mutate/build/tests/analysis"]
CACHES = ["build/tests/translator_cache", "build/mutate/build/tests/translator_cache"]
RUNNERS = ["tests/mutate.py", "run_translator_tests.sh", "run_loader_tests.sh", "test_analysis.py",
           "gpu_run.sh", "run_attempt.sh", "fm4recomp"]
CAPTURES_RE = re.compile(r"^research/m\d+/[^/]+/run/([^/]+)\.captures$")
CAPTURE_CLASSES = {"frame-ring": re.compile(r"^frame_\d+\.png$"), "keep-frames": re.compile(r"^keep_\d+\.png$")}
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


def capture_files(path, classes):
    """The files of the given capture classes directly in a captures directory (never symlinks)."""
    out = []
    for name in sorted(os.listdir(path)):
        fp = os.path.join(path, name)
        if os.path.isfile(fp) and not os.path.islink(fp) and any(CAPTURE_CLASSES[c].match(name) for c in classes):
            out.append(fp)
    return out


def capture_run_ok(root, rel):
    """Why this captures path is not a recorded run's captures directory, or None."""
    m = CAPTURES_RE.match(rel)
    p = os.path.join(root, rel)
    if not m or ".." in rel.split("/"):
        return "not research/m<N>/<scenario>/run/<run>.captures"
    if os.path.islink(p):
        return "symlink"
    if not os.path.isdir(p):
        return "gone"
    research = os.path.realpath(os.path.join(root, "research"))
    if not os.path.realpath(p).startswith(research + os.sep):
        return "resolves outside research/"
    scenario = os.path.dirname(os.path.dirname(p))
    if marker(scenario) is None:
        return "scenario has no .retention.json"
    if not os.path.isfile(os.path.join(os.path.dirname(p), m.group(1) + ".err")):
        return "no run log beside the captures"
    return None


def capture_candidates(root, classes, keep):
    items = []
    for ms in sorted(os.listdir(os.path.join(root, "research"))) if os.path.isdir(os.path.join(root, "research")) else []:
        if not re.match(r"^m\d+$", ms):
            continue
        for scenario in sorted(os.listdir(os.path.join(root, "research", ms))):
            run = os.path.join(root, "research", ms, scenario, "run")
            if scenario in keep or not os.path.isdir(run):
                continue
            for name in sorted(os.listdir(run)):
                rel = "research/%s/%s/run/%s" % (ms, scenario, name)
                if not name.endswith(".captures") or capture_run_ok(root, rel):
                    continue
                files = capture_files(os.path.join(root, rel), classes)
                if files:
                    items.append(dict(path=rel, kind="captures", classes=list(classes), files=len(files),
                                      inode=os.stat(os.path.join(root, rel)).st_ino,
                                      bytes=sum(os.path.getsize(f) for f in files),
                                      reason="capture PNGs (%s) of a recorded run, %d files" % (",".join(classes), len(files))))
    return items


EVIDENCE_CLASSES = ("metal-trace", "profile-xml", "shader-dumps", "saves")
SCENARIO_RE = re.compile(r"^research/m\d+/scenario(\d+)$")


def evidence_paths(scenario_dir, cls):
    """The paths of class `cls` in a scenario directory (absolute)."""
    out = []
    names = sorted(os.listdir(scenario_dir))
    if cls == "metal-trace":
        out = [os.path.join(scenario_dir, n) for n in names if n.endswith(".trace") and os.path.isdir(os.path.join(scenario_dir, n))]
    elif cls == "profile-xml":
        out = [os.path.join(scenario_dir, n) for n in names if n.endswith(".xml") and os.path.isfile(os.path.join(scenario_dir, n))]
    elif cls == "shader-dumps":
        out = [p for p in (os.path.join(scenario_dir, "shaders"), os.path.join(scenario_dir, "run", "shaders")) if os.path.isdir(p)]
    elif cls == "saves":
        out = [p for p in (os.path.join(scenario_dir, "user-data"),) if os.path.isdir(p)]
    return [p for p in out if not os.path.islink(p)]


def evidence_scenario_ok(root, rel_scenario):
    """Why this scenario's old evidence may not be planned or deleted, or None."""
    if not SCENARIO_RE.match(rel_scenario):
        return "not research/m<N>/scenario<K>"
    p = os.path.join(root, rel_scenario)
    if os.path.islink(p) or not os.path.isdir(p):
        return "symlink or gone"
    if marker(p) is None:
        return "scenario has no .retention.json"
    run = os.path.join(p, "run")
    if not (os.path.isdir(run) and any(n.endswith(".err") for n in os.listdir(run))):
        return "no run log in the scenario"
    return None


def evidence_candidates(root, classes, keep, before=None, milestone=None):
    items = []
    research = os.path.join(root, "research")
    for ms in sorted(os.listdir(research)) if os.path.isdir(research) else []:
        if not re.match(r"^m\d+$", ms) or (milestone is not None and ms != milestone):
            continue
        for scenario in sorted(os.listdir(os.path.join(research, ms))):
            rel_s = "research/%s/%s" % (ms, scenario)
            m = SCENARIO_RE.match(rel_s)
            if not m or scenario in keep or (before is not None and int(m.group(1)) >= before) or evidence_scenario_ok(root, rel_s):
                continue
            for cls in classes:
                for p in evidence_paths(os.path.join(root, rel_s), cls):
                    items.append(dict(path=os.path.relpath(p, root), kind="evidence", classes=[cls], inode=os.stat(p).st_ino,
                                      bytes=size(p) if os.path.isdir(p) else os.path.getsize(p),
                                      reason="old run evidence (%s) of a marked scenario with its run log" % cls))
    return items


def manifest_id(items):
    canon = json.dumps([[i["path"], i["inode"], i["bytes"], i.get("kind"), i.get("classes")] for i in items], sort_keys=True)
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
    if it.get("kind") == "evidence":
        if os.path.isabs(rel) or ".." in rel.split("/"):
            return "absolute path or escape"
        cls = (it.get("classes") or [None])[0]
        if cls not in EVIDENCE_CLASSES:
            return "unknown evidence class"
        scenario = "/".join(rel.split("/")[:3])
        why = evidence_scenario_ok(root, scenario)
        if why:
            return why
        if not os.path.lexists(p):
            return "gone"
        if os.path.islink(p):
            return "symlink"
        if p not in evidence_paths(os.path.join(root, scenario), cls):
            return "not a %s path of its scenario" % cls
        if os.stat(p).st_ino != it["inode"]:
            return "replaced since the plan (inode changed)"
        return None
    if it.get("kind") == "captures":
        if os.path.isabs(rel):
            return "absolute path"
        why = capture_run_ok(root, rel)
        if why:
            return why
        if not it.get("classes") or any(c not in CAPTURE_CLASSES for c in it["classes"]):
            return "unknown capture class"
        if os.stat(p).st_ino != it["inode"]:
            return "replaced since the plan (inode changed)"
        return None
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


def cmd_plan(root, out, captures=(), keep=(), evidence=(), before=None, milestone=None):
    items = candidates(root) + (capture_candidates(root, captures, set(keep)) if captures else []) \
        + (evidence_candidates(root, evidence, set(keep), before, milestone) if evidence else [])
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
            if why is None and it.get("kind") == "captures":
                files = capture_files(os.path.join(root, it["path"]), it["classes"])
                n = sum(os.path.getsize(f) for f in files)
                for f in files:
                    os.remove(f)
                freed += n
                result = "deleted %d files (%s), directory kept" % (len(files), mib(n))
            elif why is None and it.get("kind") == "evidence":
                p = os.path.join(root, it["path"])
                n = size(p) if os.path.isdir(p) else os.path.getsize(p)
                if os.path.isdir(p):
                    shutil.rmtree(p)
                else:
                    os.remove(p)
                freed += n
                result = "deleted"
            elif why is None:
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
    ap.add_argument("--captures", help="plan: capture classes to list under research/, comma-separated: %s"
                    % ", ".join(CAPTURE_CLASSES))
    ap.add_argument("--keep", default="", help="plan --captures/--evidence: scenario directory names left out, comma-separated")
    ap.add_argument("--evidence", help="plan: old run evidence classes, comma-separated: %s" % ", ".join(EVIDENCE_CLASSES))
    ap.add_argument("--before", type=int, help="plan --evidence: only scenarios numbered below this")
    ap.add_argument("--milestone", help="plan --evidence: only research/<milestone> (e.g. m5)")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    captures = [c for c in (a.captures or "").split(",") if c]
    if any(c not in CAPTURE_CLASSES for c in captures):
        print("unknown capture class; known: %s" % ", ".join(CAPTURE_CLASSES), file=sys.stderr)
        return 2
    evidence = [c for c in (a.evidence or "").split(",") if c]
    if any(c not in EVIDENCE_CLASSES for c in evidence):
        print("unknown evidence class; known: %s" % ", ".join(EVIDENCE_CLASSES), file=sys.stderr)
        return 2
    if a.command == "inventory":
        return cmd_inventory(root)
    if a.command == "plan":
        out = a.out or os.path.join(root, "research", "storage",
                                    "plan-%s.json" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
        return cmd_plan(root, out, captures, [k for k in a.keep.split(",") if k], evidence, a.before, a.milestone)
    if not (a.manifest and a.approve):
        print("apply needs --manifest and --approve", file=sys.stderr)
        return 2
    return cmd_apply(root, a.manifest, a.approve, os.path.join(root, "research", "storage", "audit.jsonl"))


if __name__ == "__main__":
    sys.exit(main())
