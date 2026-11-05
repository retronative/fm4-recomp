#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""Mutation gate: plant each fault of tests/mutants.py in a scratch copy of the sources and check
that its test catches it.

  /opt/homebrew/bin/python3 -I tests/mutate.py [--only ID,...] [--milestone M1] [--tsv OUT] [--full]

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

Python tests run with -B and the scratch tree's __pycache__ is purged before each run.
Design after reFrozen tests/mutate.py (scratch tree, baseline first, result classes).
"""
import argparse
import hashlib
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

SYNC_DIRS = ["survey", "tests", "scripts", "patches", "tools/XenonRecomp"]
LLVM = "/opt/homebrew/opt/llvm/bin"
XR_TARGETS = ["XenonRecomp", "XenonAnalyse"]


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
            ok, out = tree.build()
            if not ok:
                return "INVALID", "build failed: " + out.strip().splitlines()[-1][:200]
        code, out = tree.test(m["test"], focus_of(m, tree.tests, full))
        return classify(m, test, code, out)
    finally:
        with open(path, "w") as f:
            f.write(original)
        # Rebuild at once: a later rsync restores the source's older mtime, and ninja would then
        # keep the mutated object.
        if m["file"].startswith("tools/"):
            ok, out = tree.build()
            if not ok:
                raise RuntimeError("rebuild after restoring %s failed:\n%s" % (m["file"], out[-2000:]))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--only", help="comma-separated mutant ids")
    ap.add_argument("--milestone")
    ap.add_argument("--tsv")
    ap.add_argument("--scratch", default=os.path.join(ROOT, "build", "mutate"))
    ap.add_argument("--full", action="store_true", help="run every test whole (milestone close)")
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

    tree = Tree(a.scratch)
    tree.sync()
    needed = sorted({m["test"] for m in sel})
    if any(TESTS[k]["build"] for k in needed) or any(m["file"].startswith("tools/") for m in sel):
        ok, out = tree.build()
        if not ok:
            print("mutate: BASELINE build failed; nothing was mutated\n" + out[-2000:], file=sys.stderr)
            return 1
    for k, focus in sorted({(m["test"], focus_of(m, TESTS, a.full)) for m in sel}):
        code, out = tree.test(k, focus)
        if code != 0 or not re.search(TESTS[k]["ran"], out, re.M):
            print("mutate: BASELINE failed for test %s %s (exit %s); nothing was mutated\n%s" % (k, " ".join(focus), code, out[-3000:]),
                  file=sys.stderr)
            return 1
        print("baseline ok: %s %s" % (k, " ".join(focus) or "(whole)"), flush=True)

    rows, bad = [], 0
    for m in sel:
        t0 = time.time()
        status, detail = mutate_one(tree, m, a.full)
        if status not in ("KILLED", "EQUIVALENT"):
            bad += 1
        rows.append((m["id"], m["milestone"], m["test"], status, detail))
        print("%-6s %-16s %-9s %5.1fs  %s" % (m["id"], status, m["test"], time.time() - t0, detail), flush=True)
    counts = {}
    for r in rows:
        counts[r[3]] = counts.get(r[3], 0) + 1
    print("mutate: %d mutants: %s" % (len(rows), ", ".join("%d %s" % (v, k) for k, v in sorted(counts.items()))))
    if a.tsv:
        with open(a.tsv, "w") as f:
            f.write("id\tmilestone\ttest\tresult\tdetail\n")
            for r in rows:
                f.write("\t".join(r) + "\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
