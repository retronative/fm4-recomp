#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""Mutation gate: plant each fault of tests/mutants.py in a scratch copy of the sources and check
that its test catches it.

  /opt/homebrew/bin/python3 -I tests/mutate.py [--only ID,...] [--milestone M1] [--tsv OUT] [--full]
      [--fresh] [--plan] [--ledger PATH] [--profile OUT]

Focused by default: a mutant of a test marked `focus` in tests/mutants.py runs only the fixtures
named by its `focus` list (default: its expected check, without a trailing ':'), and the baseline
runs each distinct focus once. --full runs every test whole, for milestone close and large steps
(per change, focused tests only).

The real tree is never modified. build/mutate/ holds a copy of survey/, tests/, scripts/,
patches/ and tools/XenonRecomp (sources and .git; tests read the pinned upstream from it), plus
its own XenonRecomp build. Each run re-syncs the copy, runs every needed test unmutated (all must
pass: a failing baseline aborts the run), then for each mutant: apply it, rebuild if it or its
test needs the XenonRecomp build, run its test, restore.

Results (reFrozen's vocabulary):
  KILLED           the test failed, and a line starting with "FAIL" or "ERROR" names the
                   expected check (unittest reports an unexpected exception as ERROR)
  KILLED-OTHER     the test failed, but not on the expected check (review it)
  TIMEOUT          the test did not finish (its process group is killed)
  SURVIVED         the test passed: a gap, to be fixed by a better test
  EQUIVALENT       survived, and the mutant documents why it cannot change behaviour
  WRONG-EQUIVALENT documented equivalent, but killed
  INVALID          the old text is not found exactly once, the mutant equals the original, the
                   build fails, or the test did not run: never counted as a kill
Exit 0 iff every selected mutant is KILLED or EQUIVALENT.

Result reuse. Each KILLED or EQUIVALENT result is recorded in a ledger
(default research/mutation/ledger.json) with a fingerprint: SHA-256 over the mutant's own
definition, its test's TESTS entry and focus, the mutated file, every dependency its test declares
(`deps`: implementation, tests, fixtures, harness, shared code such as tools/XenonRecomp), this gate
(tests/mutate.py) and the toolchain (Python; for tests that build, LLVM, lld, CMake, Ninja, SDK).
A later run reuses a result whose fingerprint is unchanged and runs only the others (no entry, an
input changed, or not KILLED/EQUIVALENT before): an ordinary change re-tests only its affected
mutants, and a change to shared code invalidates every result that depends on it. Reused results
are reported as "reused" with the run that produced them; freshly tested ones as "fresh". --fresh
ignores the ledger (broad changes, release checkpoints); --plan only lists what would run and why.
Only fresh runs write the ledger; a fresh result that is not KILLED/EQUIVALENT removes its entry.

Storage: after a run whose report is written and whose fresh results all pass, the run's outputs
in the scratch tree (RUN_OUTPUTS) are removed and the harness cache is capped (size-based
eviction); a failing run keeps them. Each run appends one line to research/mutation/runs.jsonl
(command, counts fresh/reused, gate and toolchain hashes, time, bytes freed). One gate at a time
per scratch tree (build/mutate/.gate.lock).

Python tests run with -B and the scratch tree's __pycache__ is purged before each run.
Design after reFrozen tests/mutate.py (scratch tree, baseline first, result classes).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mutants import MUTANTS, TESTS  # noqa: E402

SYNC_DIRS = ["survey", "tests", "scripts", "patches", "runtime", "recomp", "tools/XenonRecomp"]
SYNC_FILES = ["CMakeLists.txt"]
LLVM = "/opt/homebrew/opt/llvm/bin"
XR_TARGETS = ["XenonRecomp", "XenonAnalyse"]
LEDGER = os.path.join(ROOT, "research", "mutation", "ledger.json")
# Per-run outputs in the scratch tree, removed after a run whose report was written and whose
# fresh results are all KILLED/EQUIVALENT (a failing run keeps them for diagnosis). The scratch
# sources and the XenonRecomp build stay: they make the next run incremental.
RUN_OUTPUTS = ["build/tests/translator", "build/tests/loader", "build/tests/analysis"]
HARNESS_CACHE = "build/tests/translator_cache"
HARNESS_CACHE_KIB = 204800
SUCCESS = ("KILLED", "EQUIVALENT")
SKIP_DIRS = {".git", "__pycache__"}


def run(cmd, cwd, timeout, env=None):
    """Run cmd in its own process group; on timeout kill the whole group. Returns (code, output);
    code None means timeout."""
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, start_new_session=True)
    try:
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out
    except subprocess.TimeoutExpired:
        try:
            os.killpg(p.pid, signal.SIGTERM)
            time.sleep(1)
            os.killpg(p.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass  # the group is already gone (macOS answers EPERM for a group of only a zombie leader)
        out, _ = p.communicate()
        return None, out


class Tree:
    def __init__(self, scratch, root=ROOT, tests=TESTS):
        self.scratch, self.root, self.tests = scratch, root, tests
        self.configured = False

    def sync(self):
        os.makedirs(self.scratch, exist_ok=True)
        for d in SYNC_DIRS:
            src = os.path.join(self.root, d)
            if not os.path.isdir(src):
                continue
            dst = os.path.join(self.scratch, d)
            os.makedirs(dst, exist_ok=True)
            subprocess.run(["rsync", "-a", "--delete", "--exclude", "/build/", "--exclude", "__pycache__",
                            src + "/", dst + "/"], check=True)
        for f in SYNC_FILES:
            if os.path.isfile(os.path.join(self.root, f)):
                shutil.copy2(os.path.join(self.root, f), os.path.join(self.scratch, f))

    def build(self):
        xr = os.path.join(self.scratch, "tools", "XenonRecomp")
        if not self.configured:
            code, out = run(["cmake", "-Wno-deprecated", "-S", xr, "-B", os.path.join(xr, "build"), "-G", "Ninja",
                             "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={LLVM}/clang",
                             f"-DCMAKE_CXX_COMPILER={LLVM}/clang++", "-DCMAKE_CXX_FLAGS=-include cstdlib"],
                            self.scratch, 600)
            if code != 0:
                return False, out
            self.configured = True
        code, out = run(["ninja", "-C", os.path.join(xr, "build")] + XR_TARGETS, self.scratch, 1800)
        return code == 0, out

    def purge_bytecode(self):
        for dirpath, dirnames, _ in os.walk(self.scratch):
            if os.path.join("tools", "XenonRecomp") in dirpath:
                continue
            for d in list(dirnames):
                if d == "__pycache__":
                    shutil.rmtree(os.path.join(dirpath, d))
                    dirnames.remove(d)

    def test(self, key, args=()):
        t = self.tests[key]
        self.purge_bytecode()
        return run(t["cmd"] + list(args), self.scratch, t["timeout"])


def path_hash(base, rel):
    """SHA-256 of a file, or of a directory's files (relative names and contents; .git,
    __pycache__ and a top-level build/ are skipped), or of "missing"."""
    path = os.path.join(base, rel)
    h = hashlib.sha256()
    if os.path.isfile(path):
        with open(path, "rb") as f:
            h.update(f.read())
    elif os.path.isdir(path):
        for dirpath, dirnames, files in os.walk(path):
            dirnames[:] = sorted(d for d in dirnames
                                 if d not in SKIP_DIRS and not (dirpath == path and d == "build"))
            for name in sorted(files):
                fp = os.path.join(dirpath, name)
                h.update(os.path.relpath(fp, path).encode() + b"\0")
                if os.path.islink(fp):
                    h.update(b"link " + os.readlink(fp).encode())
                else:
                    with open(fp, "rb") as f:
                        h.update(hashlib.sha256(f.read()).digest())
    else:
        h.update(b"missing")
    return h.hexdigest()


def tool_versions(build):
    """Toolchain identity: Python always; for tests that build, the LLVM tools, lld, CMake, Ninja
    and the SDK."""
    parts = ["python " + sys.version]
    if build:
        for cmd in ([LLVM + "/clang", "--version"], [LLVM + "/llvm-mc", "--version"], ["ld.lld", "--version"],
                    ["cmake", "--version"], ["ninja", "--version"], ["xcrun", "--show-sdk-version"]):
            try:
                out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
            except (OSError, subprocess.TimeoutExpired) as e:
                out = "unavailable: %s" % e
            parts.append(" ".join(cmd) + ": " + out.strip())
    return "\n".join(parts)


class Fingerprints:
    """Per-mutant input hashes, each path hashed once per run."""

    def __init__(self, base, tests, gate_file="tests/mutate.py", tools=tool_versions):
        self.base, self.tests, self.gate_file, self.tools = base, tests, gate_file, tools
        self.memo = {}

    def h(self, rel):
        if rel not in self.memo:
            self.memo[rel] = path_hash(self.base, rel)
        return self.memo[rel]

    def components(self, m, focus):
        test = self.tests[m["test"]]
        build = bool(test["build"] or m["file"].startswith("tools/"))
        key = ("tools", build)
        if key not in self.memo:
            self.memo[key] = hashlib.sha256(self.tools(build).encode()).hexdigest()
        c = {
            "mutant": hashlib.sha256(json.dumps(m, sort_keys=True).encode()).hexdigest(),
            "test": hashlib.sha256(json.dumps([test, list(focus)], sort_keys=True).encode()).hexdigest(),
            "file": self.h(m["file"]),
            "gate": self.h(self.gate_file),
            "toolchain": self.memo[key],
        }
        for d in test.get("deps", []):
            c["dep:" + d] = self.h(d)
        return c


def fingerprint(components):
    return hashlib.sha256(json.dumps(components, sort_keys=True).encode()).hexdigest()


def load_ledger(path):
    """The ledger, or an empty one if the file does not exist. A ledger that cannot be read stops
    the run: evidence is never dropped silently."""
    if not os.path.exists(path):
        return {"version": 1, "results": {}}
    with open(path) as f:
        led = json.load(f)
    if led.get("version") != 1 or not isinstance(led.get("results"), dict):
        raise ValueError("unrecognised mutation ledger %s" % path)
    return led


def save_ledger(path, led):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(led, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


def reuse_decision(led, key, comps, fresh):
    """(entry or None, reason): the ledger entry to reuse, or why the mutant must run."""
    if fresh:
        return None, "--fresh"
    e = led["results"].get(key)
    if e is None:
        return None, "no recorded result"
    if e.get("result") not in SUCCESS:
        return None, "recorded result %s" % e.get("result")
    if e.get("fingerprint") != fingerprint(comps):
        old = e.get("components", {})
        changed = sorted(k for k in set(old) | set(comps) if old.get(k) != comps.get(k))
        return None, "changed: " + ", ".join(changed)
    return e, "inputs unchanged"


def classify(m, test, code, out):
    if code is None:
        return "TIMEOUT", "no result within %ds" % test["timeout"]
    if not re.search(test["ran"], out, re.M):
        return "INVALID", "the test did not run"
    if code == 0:
        return ("EQUIVALENT", m["equivalent"]) if m.get("equivalent") else ("SURVIVED", "the test passed")
    fails = [l.strip() for l in out.splitlines() if l.strip().startswith(("FAIL", "ERROR"))]
    hit = [l for l in fails if m["expect"] in l]
    if m.get("equivalent"):
        return "WRONG-EQUIVALENT", (hit or fails or ["exit %d" % code])[0]
    if hit:
        return "KILLED", hit[0]
    return "KILLED-OTHER", (fails[0] if fails else "exit %d without a FAIL line" % code)


PROFILE = []  # (id, phase, seconds) when --profile is given


def timed(mid, phase, fn, *args):
    t0 = time.time()
    result = fn(*args)
    PROFILE.append((mid, phase, time.time() - t0))
    return result


def focus_of(m, tests, full=False):
    """The fixture names a mutant's test runs with, or () for the whole test."""
    if full or not tests[m["test"]].get("focus"):
        return ()
    return tuple(m.get("focus") or [m["expect"].rstrip(":")])


def mutate_one(tree, m, full=False):
    test = tree.tests[m["test"]]
    path = os.path.join(tree.scratch, m["file"])
    with open(path) as f:
        original = f.read()
    if original.count(m["old"]) != 1:
        return "INVALID", "old text found %d times" % original.count(m["old"])
    mutated = original.replace(m["old"], m["new"])
    if hashlib.sha256(mutated.encode()).digest() == hashlib.sha256(original.encode()).digest():
        return "INVALID", "the mutant equals the original"
    try:
        with open(path, "w") as f:
            f.write(mutated)
        if test["build"] or m["file"].startswith("tools/"):
            ok, out = timed(m["id"], "build", tree.build)
            if not ok:
                return "INVALID", "build failed: " + out.strip().splitlines()[-1][:200]
        code, out = timed(m["id"], "test", tree.test, m["test"], focus_of(m, tree.tests, full))
        return classify(m, test, code, out)
    finally:
        with open(path, "w") as f:
            f.write(original)
        # Rebuild at once: a later rsync restores the source's older mtime, and ninja would then
        # keep the mutated object.
        if m["file"].startswith("tools/"):
            ok, out = timed(m["id"], "restore_build", tree.build)
            if not ok:
                raise RuntimeError("rebuild after restoring %s failed:\n%s" % (m["file"], out[-2000:]))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--only", help="comma-separated mutant ids")
    ap.add_argument("--milestone")
    ap.add_argument("--tsv")
    ap.add_argument("--scratch", default=os.path.join(ROOT, "build", "mutate"))
    ap.add_argument("--full", action="store_true", help="run every test whole (milestone close)")
    ap.add_argument("--profile", help="write per-mutant phase times (TSV)")
    ap.add_argument("--fresh", action="store_true", help="ignore recorded results: re-test every selected mutant")
    ap.add_argument("--plan", action="store_true", help="list which mutants would run or be reused, and why")
    ap.add_argument("--ledger", default=LEDGER, help="recorded results (default research/mutation/ledger.json)")
    a = ap.parse_args(argv)
    sel = MUTANTS
    if a.only:
        ids = a.only.split(",")
        unknown = set(ids) - {m["id"] for m in MUTANTS}
        if unknown:
            print("mutate: unknown ids: " + ", ".join(sorted(unknown)), file=sys.stderr)
            return 2
        sel = [m for m in sel if m["id"] in ids]
    if a.milestone:
        sel = [m for m in sel if m["milestone"] == a.milestone]
    if not sel:
        print("mutate: no mutants selected", file=sys.stderr)
        return 2
    ids = [m["id"] for m in MUTANTS]
    if len(ids) != len(set(ids)):
        print("mutate: duplicate mutant ids in tests/mutants.py", file=sys.stderr)
        return 2

    lock = acquire_lock(a.scratch)
    if lock is None:
        return 2
    try:
        return gate_main(a, sel, argv if argv is not None else sys.argv[1:])
    finally:
        os.remove(lock)


def acquire_lock(scratch):
    """build/mutate/.gate.lock holding our pid; refused while another live gate holds it."""
    os.makedirs(scratch, exist_ok=True)
    path = os.path.join(scratch, ".gate.lock")
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        try:
            with open(path) as f:
                pid = int(f.read().strip() or 0)
            os.kill(pid, 0)
            print("mutate: another gate (pid %d) is using %s" % (pid, scratch), file=sys.stderr)
            return None
        except (ValueError, ProcessLookupError):
            os.remove(path)  # left by a gate that is gone
            return acquire_lock(scratch)
        except PermissionError:
            print("mutate: %s is held by another user's process" % path, file=sys.stderr)
            return None
    with os.fdopen(fd, "w") as f:
        f.write(str(os.getpid()))
    return path


def tree_bytes(path):
    total = 0
    for dirpath, _, files in os.walk(path):
        for name in files:
            fp = os.path.join(dirpath, name)
            if not os.path.islink(fp):
                total += os.path.getsize(fp)
    return total


def clean_run_outputs(scratch):
    """Remove the run's outputs from the scratch tree and cap the harness cache. Returns bytes freed.
    Touches only RUN_OUTPUTS and the cache's own entries, inside the scratch tree."""
    before = tree_bytes(os.path.join(scratch, "build"))
    for rel in RUN_OUTPUTS:
        path = os.path.join(scratch, rel)
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path)
    cache = os.path.join(scratch, HARNESS_CACHE)
    lib = os.path.join(scratch, "tests", "translator_lib.sh")
    if os.path.isdir(cache) and os.path.exists(lib):
        subprocess.run(["bash", lib, "cache_prune", cache, str(HARNESS_CACHE_KIB)], check=True, timeout=120)
    return before - tree_bytes(os.path.join(scratch, "build"))


def clean_if_passed(scratch, failures):
    """After the report is written: clean a passing run's outputs; a failing run keeps them."""
    return 0 if failures else clean_run_outputs(scratch)


def gate_main(a, sel, argv):
    t_start = time.time()
    tree = Tree(a.scratch)
    tree.sync()
    try:
        led = load_ledger(a.ledger)
    except (ValueError, json.JSONDecodeError) as e:
        print("mutate: cannot read the ledger: %s" % e, file=sys.stderr)
        return 2
    fps = Fingerprints(tree.scratch, TESTS)
    rows, bad = run_gate(tree, sel, TESTS, led, a.ledger, full=a.full, fresh=a.fresh, plan=a.plan,
                         tsv=a.tsv, fingerprints=fps)
    if rows is None:
        return 1
    if a.plan:
        return 0
    if a.profile:
        with open(a.profile, "w") as f:
            f.write("id\tphase\tseconds\n")
            for mid, phase, sec in PROFILE:
                f.write("%s\t%s\t%.2f\n" % (mid, phase, sec))
    if a.tsv:
        with open(a.tsv, "w") as f:
            f.write("id\tmilestone\ttest\tresult\tsource\tevidence_run\tdetail\n")
            for r in rows:
                f.write("\t".join(r) + "\n")
    freed = clean_if_passed(tree.scratch, bad)
    counts = {}
    for r in rows:
        counts.setdefault(r[4], {}).setdefault(r[3], 0)
        counts[r[4]][r[3]] += 1
    summary = dict(run=rows[0][5] if rows and rows[0][4] == "fresh" else
                   datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                   command="/opt/homebrew/bin/python3 -I tests/mutate.py " + " ".join(argv),
                   selected=len(rows), results=counts, failures=bad, seconds=round(time.time() - t_start, 1),
                   gate=fps.h("tests/mutate.py"),
                   toolchain={str(k[1]): v for k, v in fps.memo.items() if isinstance(k, tuple)},
                   tsv=a.tsv or "", ledger=a.ledger, outputs_kept_for_diagnosis=bool(bad), bytes_freed=freed)
    runs = os.path.join(os.path.dirname(os.path.abspath(a.ledger)), "runs.jsonl")
    os.makedirs(os.path.dirname(runs), exist_ok=True)
    with open(runs, "a") as f:
        f.write(json.dumps(summary, sort_keys=True) + "\n")
    print("mutate: %s; %d bytes of run outputs removed%s" % (
        "report written", freed, "" if not bad else " (none: failures keep their outputs)"))
    return 1 if bad else 0


def say(*args):
    print(*args, flush=True)


def ledger_key(m, full):
    """Focused and whole-test results are recorded separately: neither replaces the other."""
    return m["id"] + ("@full" if full else "")


def run_gate(tree, sel, tests, led, ledger_path, full=False, fresh=False, plan=False, tsv=None,
             fingerprints=None, out=say):
    """Reuse what the ledger proves, run the rest. Returns (rows, failures); rows None when a
    baseline failed. Row: id, milestone, test, result, source (fresh/reused), evidence run, detail."""
    fps = fingerprints or Fingerprints(tree.scratch, tests)
    run_id = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    todo, reused = [], {}
    for m in sel:
        focus = focus_of(m, tests, full)
        comps = fps.components(m, focus)
        entry, reason = reuse_decision(led, ledger_key(m, full), comps, fresh)
        if plan:
            out("%-6s %-6s %s" % (m["id"], "reuse" if entry else "run", reason))
        if entry:
            reused[m["id"]] = entry
        else:
            todo.append((m, comps))
    if plan:
        out("mutate: plan: %d to run, %d to reuse" % (len(todo), len(reused)))
        return [], 0

    if todo:
        needed = sorted({m["test"] for m, _ in todo})
        if any(tests[k]["build"] for k in needed) or any(m["file"].startswith("tools/") for m, _ in todo):
            ok, o = tree.build()
            if not ok:
                print("mutate: BASELINE build failed; nothing was mutated\n" + o[-2000:], file=sys.stderr)
                return None, 1
        for k, focus in sorted({(m["test"], focus_of(m, tests, full)) for m, _ in todo}):
            code, o = tree.test(k, focus)
            if code != 0 or not re.search(tests[k]["ran"], o, re.M):
                print("mutate: BASELINE failed for test %s %s (exit %s); nothing was mutated\n%s"
                      % (k, " ".join(focus), code, o[-3000:]), file=sys.stderr)
                return None, 1
            out("baseline ok: %s %s" % (k, " ".join(focus) or "(whole)"))

    rows, bad = [], 0
    comps_of = {m["id"]: c for m, c in todo}
    for m in sel:
        if m["id"] in reused:
            e = reused[m["id"]]
            rows.append((m["id"], m["milestone"], m["test"], e["result"], "reused", e["run"], e["detail"]))
            out("%-6s %-16s %-6s %-9s  %s (tested %s)" % (m["id"], e["result"], "reused", m["test"], e["detail"], e["run"]))
            continue
        t0 = time.time()
        status, detail = mutate_one(tree, m, full)
        if status in SUCCESS:
            comps = comps_of[m["id"]]
            led["results"][ledger_key(m, full)] = dict(fingerprint=fingerprint(comps), components=comps, result=status,
                                           detail=detail, test=m["test"], full=bool(full), run=run_id,
                                           evidence=tsv or "",
                                           reproduce="/opt/homebrew/bin/python3 -I tests/mutate.py --only %s%s --fresh"
                                           % (m["id"], " --full" if full else ""))
        else:
            bad += 1
            led["results"].pop(ledger_key(m, full), None)
        save_ledger(ledger_path, led)
        rows.append((m["id"], m["milestone"], m["test"], status, "fresh", run_id, detail))
        out("%-6s %-16s %-6s %-9s %5.1fs  %s" % (m["id"], status, "fresh", m["test"], time.time() - t0, detail))
    summary = []
    for source in ("fresh", "reused"):
        counts = {}
        for r in rows:
            if r[4] == source:
                counts[r[3]] = counts.get(r[3], 0) + 1
        if counts:
            summary.append("%s: %s" % (source, ", ".join("%d %s" % (v, k) for k, v in sorted(counts.items()))))
    out("mutate: %d mutants: %s" % (len(rows), "; ".join(summary)))
    return rows, bad


if __name__ == "__main__":
    sys.exit(main())
