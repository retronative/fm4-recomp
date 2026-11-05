# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Tests of the mutation gate itself (tests/mutate.py) on a toy tree in a temporary folder:
# every verdict, both FAIL-line shapes (unittest "FAIL: test_x" and the loader runner's
# "  FAIL <check>"), INVALID cases, a hanging mutant whose process group must be killed, and
# that the real tree is never touched. Result reuse: a recorded KILLED/EQUIVALENT result is reused
# (and its test not run) only while every input of its fingerprint is unchanged. Plus the
# registry's own consistency.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_mutate.py
import io
import json
import os
import re
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mutate  # noqa: E402
import mutants  # noqa: E402

TOY = '''def add(a, b):
    return a + b


def sign(x):
    if x < 0:
        return -1
    return 1


def wait():
    return 0
'''

# A unittest-shaped test and a runner-shaped test (prints "FAIL <check>" lines itself).
TOY_TEST = '''import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "survey"))
import toy

class T(unittest.TestCase):
    def test_add(self):
        self.assertEqual(toy.add(2, 3), 5)
    def test_sign(self):
        self.assertEqual(toy.sign(-4), -1)
    def test_wait(self):
        self.assertEqual(toy.wait(), 0)

unittest.main()
'''
RUNNER_TEST = '''import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "survey"))
import toy
bad = 0
only = sys.argv[1:]
for name, ok in (("adds two numbers", toy.add(2, 3) == 5), ("signs a negative", toy.sign(-4) == -1)):
    if only and name.split()[0] not in only:
        continue
    if not ok:
        print("  FAIL " + name); bad += 1
print("checked: 2 checks, %d failures" % bad)
sys.exit(1 if bad else 0)
'''
PY = ["/opt/homebrew/bin/python3", "-B", "-I"]
TESTS = {
    "toy": dict(cmd=PY + ["tests/test_toy.py"], timeout=20, build=False, ran=r"^Ran \d+ tests?"),
    "runner": dict(cmd=PY + ["tests/runner_toy.py"], timeout=20, build=False, ran=r"^checked: "),
    "silent": dict(cmd=PY + ["tests/test_toy.py"], timeout=20, build=False, ran=r"^never printed"),
    "short": dict(cmd=PY + ["tests/test_toy.py"], timeout=3, build=False, ran=r"^Ran \d+ tests?"),
    "focused": dict(cmd=PY + ["tests/runner_toy.py"], timeout=20, build=False, ran=r"^checked: ", focus=True),
    "deps": dict(cmd=PY + ["tests/test_toy.py"], timeout=20, build=False, ran=r"^Ran \d+ tests?",
                 deps=["tests/test_toy.py", "survey/helper.py", "tests/fixtures"]),
}


def M(id, old, new, expect, test="toy", **kw):
    return dict(id=id, milestone="T", file="survey/toy.py", test=test, fault=id, old=old, new=new, expect=expect, **kw)


class ToyTree(unittest.TestCase):
    """A toy repository and its scratch tree (no tests of its own)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = os.path.join(self.tmp.name, "repo")
        for d in ("survey", "tests"):
            os.makedirs(os.path.join(root, d))
        open(os.path.join(root, "survey", "toy.py"), "w").write(TOY)
        open(os.path.join(root, "tests", "test_toy.py"), "w").write(TOY_TEST)
        open(os.path.join(root, "tests", "runner_toy.py"), "w").write(RUNNER_TEST)
        self.root = root
        self.tree = mutate.Tree(os.path.join(self.tmp.name, "scratch"), root=root, tests=TESTS)
        self.tree.sync()

    def tearDown(self):
        self.tmp.cleanup()

    def result(self, m):
        return mutate.mutate_one(self.tree, m)[0]


class Gate(ToyTree):
    def test_baseline_passes(self):
        self.assertEqual(self.tree.test("toy")[0], 0)
        self.assertEqual(self.tree.test("runner")[0], 0)

    def test_killed_unittest_shape(self):
        self.assertEqual(self.result(M("k", "return a + b", "return a - b", "test_add")), "KILLED")

    def test_killed_runner_shape(self):
        self.assertEqual(self.result(M("k", "return a + b", "return a - b", "adds two numbers", test="runner")), "KILLED")

    def test_killed_other(self):
        self.assertEqual(self.result(M("o", "return a + b", "return a - b", "test_sign")), "KILLED-OTHER")

    def test_survived(self):
        self.assertEqual(self.result(M("s", "return 1\n", "return 2\n", "test_sign")), "SURVIVED")

    def test_equivalent_and_wrong_equivalent(self):
        self.assertEqual(self.result(M("e", "return 1\n", "return 2\n", "test_sign", equivalent="positive sign unused")),
                         "EQUIVALENT")
        self.assertEqual(self.result(M("w", "return a + b", "return a - b", "test_add", equivalent="claimed")),
                         "WRONG-EQUIVALENT")

    def test_invalid_cases(self):
        self.assertEqual(self.result(M("n", "not in the file", "x", "test_add")), "INVALID")
        self.assertEqual(self.result(M("d", "return", "return", "test_add")), "INVALID")       # found 6 times
        self.assertEqual(self.result(M("i", "return a + b", "return a + b", "test_add")), "INVALID")  # equal to original
        self.assertEqual(self.result(M("r", "return a + b", "return a - b", "test_add", test="silent")), "INVALID")
        self.assertEqual(self.result(M("y", "return a + b", "return a +", "test_add")), "INVALID")   # syntax error: did not run

    def test_timeout_kills_process_group(self):
        marker = os.path.join(self.tmp.name, "child.pid")
        hang = ("import subprocess, time\n    p = subprocess.Popen(['sleep', '60'])\n"
                "    open(%r, 'w').write(str(p.pid))\n    time.sleep(60)\n    return 0" % marker)
        t0 = time.time()
        self.assertEqual(self.result(M("h", "return 0", hang, "test_wait", test="short")), "TIMEOUT")
        self.assertLess(time.time() - t0, 15)
        with open(marker) as f:
            child = int(f.read())
        time.sleep(0.5)
        alive = True
        try:
            os.kill(child, 0)
        except ProcessLookupError:
            alive = False
        self.assertFalse(alive, "the hanging test's child process survived the timeout")

    def test_restores_and_never_touches_the_real_tree(self):
        self.result(M("k", "return a + b", "return a - b", "test_add"))
        for base in (self.tree.scratch, self.root):
            with open(os.path.join(base, "survey", "toy.py")) as f:
                self.assertEqual(f.read(), TOY)

    def test_focus_runs_only_the_named_check(self):
        # focused on "signs": an add fault is not exercised, so it survives; the sign fault is killed
        self.assertEqual(self.result(M("f", "return a + b", "return a - b", "adds", test="focused", focus=["signs"])),
                         "SURVIVED")
        self.assertEqual(self.result(M("g", "return -1", "return 1", "signs a negative", test="focused", focus=["signs"])),
                         "KILLED")
        self.assertEqual(mutate.focus_of(M("h", "a", "b", "update_forms:", test="focused"), TESTS), ("update_forms",))
        self.assertEqual(mutate.focus_of(M("h", "a", "b", "x", test="focused"), TESTS, full=True), ())
        self.assertEqual(mutate.focus_of(M("h", "a", "b", "x", test="toy"), TESTS), ())

    def test_stale_bytecode_purged(self):
        # Two equal-size mutants written within a second: without the purge, Python may reuse the
        # first one's bytecode. Both must be judged on their own text.
        self.assertEqual(self.result(M("a", "return a + b", "return a - b", "test_add")), "KILLED")
        self.assertEqual(self.result(M("b", "return a + b", "return a + b + 0", "test_add",
                                       equivalent="adds zero")), "EQUIVALENT")


class Reuse(ToyTree):
    """Recorded results and their fingerprints (tests/mutate.py run_gate)."""

    def setUp(self):
        super().setUp()
        for base in (self.root, self.tree.scratch):
            open(os.path.join(base, "survey", "helper.py"), "w").write("SHARED = 1\n")
            open(os.path.join(base, "survey", "unrelated.py"), "w").write("OTHER = 1\n")
            open(os.path.join(base, "tests", "mutate.py"), "w").write("# gate v1\n")
            os.makedirs(os.path.join(base, "tests", "fixtures", "sub"), exist_ok=True)
            open(os.path.join(base, "tests", "fixtures", "sub", "case.s"), "w").write("li r3, 1\n")
        self.ledger = os.path.join(self.tmp.name, "ledger.json")
        self.tools = "toolchain v1"
        self.calls = 0
        real_test = self.tree.test

        def counting(*a, **kw):
            self.calls += 1
            return real_test(*a, **kw)
        self.tree.test = counting
        self.kill = M("k", "return a + b", "return a - b", "test_add", test="deps")

    def gate(self, sel, **kw):
        led = mutate.load_ledger(self.ledger)
        fps = mutate.Fingerprints(self.tree.scratch, TESTS, tools=lambda build: self.tools)
        self.calls = 0
        self.lines = []
        rows, bad = mutate.run_gate(self.tree, sel, TESTS, led, self.ledger, fingerprints=fps,
                                    out=self.lines.append, **kw)
        return {r[0]: r for r in rows}

    def edit(self, rel, text):
        with open(os.path.join(self.tree.scratch, rel), "a") as f:
            f.write(text)

    def assertSource(self, rows, mid, source, result="KILLED"):
        self.assertEqual((rows[mid][3], rows[mid][4]), (result, source))

    def test_records_then_reuses_without_running(self):
        first = self.gate([self.kill])
        self.assertSource(first, "k", "fresh")
        self.assertGreater(self.calls, 0)
        with open(self.ledger) as f:
            self.assertEqual(json.load(f)["results"]["k"]["result"], "KILLED")
        again = self.gate([self.kill])
        self.assertSource(again, "k", "reused")
        self.assertEqual(self.calls, 0, "a reused result ran its test (or a baseline)")
        self.assertEqual(again["k"][5], first["k"][5], "the evidence run is the one that tested it")
        self.assertEqual(again["k"][6], first["k"][6])

    def test_implementation_change_reruns(self):
        self.gate([self.kill])
        self.edit("survey/toy.py", "# changed\n")
        self.assertSource(self.gate([self.kill]), "k", "fresh")

    def test_test_change_reruns(self):
        self.gate([self.kill])
        self.edit("tests/test_toy.py", "# changed\n")
        self.assertSource(self.gate([self.kill]), "k", "fresh")

    def test_shared_dependency_change_reruns(self):
        self.gate([self.kill])
        self.edit("survey/helper.py", "SHARED = 2\n")
        self.assertSource(self.gate([self.kill]), "k", "fresh")

    def test_directory_dependency_change_reruns(self):
        self.gate([self.kill])
        self.edit("tests/fixtures/sub/case.s", "li r4, 2\n")
        self.assertSource(self.gate([self.kill]), "k", "fresh")

    def test_directory_dependency_rename_reruns(self):
        self.gate([self.kill])
        sub = os.path.join(self.tree.scratch, "tests", "fixtures", "sub")
        os.rename(os.path.join(sub, "case.s"), os.path.join(sub, "case2.s"))
        self.assertSource(self.gate([self.kill]), "k", "fresh")

    def test_undeclared_file_change_reuses(self):
        self.gate([self.kill])
        self.edit("survey/unrelated.py", "OTHER = 2\n")
        self.assertSource(self.gate([self.kill]), "k", "reused")

    def test_gate_change_reruns(self):
        self.gate([self.kill])
        self.edit("tests/mutate.py", "# gate v2\n")
        self.assertSource(self.gate([self.kill]), "k", "fresh")

    def test_toolchain_change_reruns(self):
        self.gate([self.kill])
        self.tools = "toolchain v2"
        self.assertSource(self.gate([self.kill]), "k", "fresh")

    def test_own_definition_change_reruns_others_reuse(self):
        other = M("o2", "return 1\n", "return 2\n", "test_sign", test="deps", equivalent="positive sign unused")
        self.gate([self.kill, other])
        changed = dict(self.kill, fault="reworded")
        rows = self.gate([changed, other])
        self.assertSource(rows, "k", "fresh")
        self.assertSource(rows, "o2", "reused", "EQUIVALENT")

    def test_survivor_is_never_recorded_or_reused(self):
        s = M("s", "return 1\n", "return 2\n", "test_sign", test="deps")
        self.assertSource(self.gate([s]), "s", "fresh", "SURVIVED")
        self.assertSource(self.gate([s]), "s", "fresh", "SURVIVED")
        self.assertNotIn("s", mutate.load_ledger(self.ledger)["results"])

    def test_fresh_failure_removes_the_record(self):
        self.gate([self.kill])
        self.edit("tests/test_toy.py", "")  # unchanged content: still reused
        with open(os.path.join(self.tree.scratch, "tests", "test_toy.py")) as f:
            text = f.read()
        with open(os.path.join(self.tree.scratch, "tests", "test_toy.py"), "w") as f:
            f.write(text.replace("self.assertEqual(toy.add(2, 3), 5)", "pass"))
        self.assertSource(self.gate([self.kill]), "k", "fresh", "SURVIVED")
        self.assertNotIn("k", mutate.load_ledger(self.ledger)["results"])

    def test_fresh_flag_ignores_the_ledger(self):
        self.gate([self.kill])
        self.assertSource(self.gate([self.kill], fresh=True), "k", "fresh")
        self.assertGreater(self.calls, 0)

    def test_whole_and_focused_results_are_separate(self):
        f = M("g", "return -1", "return 1", "signs a negative", test="focused", focus=["signs"])
        self.gate([f])
        self.assertSource(self.gate([f], full=True), "g", "fresh")
        self.assertSource(self.gate([f]), "g", "reused")
        self.assertSource(self.gate([f], full=True), "g", "reused")

    def test_plan_runs_nothing(self):
        self.gate([self.kill])
        self.edit("survey/helper.py", "SHARED = 3\n")
        rows = self.gate([self.kill, M("n", "return 1\n", "return 2\n", "test_sign", test="deps")], plan=True)
        self.assertEqual(rows, {})
        self.assertEqual(self.calls, 0)
        self.assertTrue(any(l.startswith("k ") and "run" in l and "dep:survey/helper.py" in l for l in self.lines), self.lines)
        self.assertTrue(any(l.startswith("n ") and "no recorded result" in l for l in self.lines), self.lines)

    def test_unreadable_ledger_stops(self):
        with open(self.ledger, "w") as f:
            f.write("{not json")
        with self.assertRaises(json.JSONDecodeError):
            mutate.load_ledger(self.ledger)
        with open(self.ledger, "w") as f:
            f.write('{"version": 2, "results": {}}')
        with self.assertRaises(ValueError):
            mutate.load_ledger(self.ledger)


class Storage(unittest.TestCase):
    """Post-run cleanup and the run lock (tests/mutate.py)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.s = os.path.join(self.tmp.name, "scratch")
        for rel in ("build/tests/translator/fx/out/a.cpp", "build/tests/loader/xex_a", "build/tests/analysis/c/x",
                    "build/keep/me.txt", "tools/XenonRecomp/build/XenonRecomp/XenonRecomp", "tests/fixture.s"):
            p = os.path.join(self.s, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w") as f:
                f.write("x" * 100)
        lib = os.path.join(HERE, "translator_lib.sh")
        os.makedirs(os.path.join(self.s, "tests"), exist_ok=True)
        with open(lib) as src, open(os.path.join(self.s, "tests", "translator_lib.sh"), "w") as dst:
            dst.write(src.read())
        self.cache = os.path.join(self.s, mutate.HARNESS_CACHE)
        for i in range(3):
            p = os.path.join(self.cache, "%064x" % i, "harness")
            os.makedirs(os.path.dirname(p))
            with open(p, "w") as f:
                f.write("h")

    def tearDown(self):
        self.tmp.cleanup()

    def exists(self, rel):
        return os.path.exists(os.path.join(self.s, rel))

    def test_passing_run_removes_only_its_outputs(self):
        freed = mutate.clean_if_passed(self.s, 0)
        self.assertGreaterEqual(freed, 300)
        for rel in mutate.RUN_OUTPUTS:
            self.assertFalse(self.exists(rel), rel)
        for rel in ("build/keep/me.txt", "tools/XenonRecomp/build/XenonRecomp/XenonRecomp", "tests/fixture.s",
                    "tests/translator_lib.sh"):
            self.assertTrue(self.exists(rel), rel)
        self.assertEqual(len(os.listdir(self.cache)), 3, "cache entries within the cap stay")

    def test_failing_run_keeps_its_outputs(self):
        self.assertEqual(mutate.clean_if_passed(self.s, 1), 0)
        for rel in mutate.RUN_OUTPUTS:
            self.assertTrue(self.exists(rel), rel)

    def test_cache_is_capped(self):
        old = mutate.HARNESS_CACHE_KIB
        mutate.HARNESS_CACHE_KIB = 0
        try:
            mutate.clean_if_passed(self.s, 0)
        finally:
            mutate.HARNESS_CACHE_KIB = old
        self.assertEqual(os.listdir(self.cache), [])

    def test_symlinked_output_is_not_followed(self):
        outside = os.path.join(self.tmp.name, "outside")
        os.makedirs(outside)
        with open(os.path.join(outside, "keep.txt"), "w") as f:
            f.write("unrelated")
        loader = os.path.join(self.s, "build/tests/loader")
        for name in os.listdir(loader):
            os.remove(os.path.join(loader, name))
        os.rmdir(loader)
        os.symlink(outside, loader)
        mutate.clean_if_passed(self.s, 0)
        self.assertTrue(os.path.exists(os.path.join(outside, "keep.txt")))

    def test_lock_refuses_a_live_gate_and_reclaims_a_dead_one(self):
        lock = mutate.acquire_lock(self.s)
        self.assertIsNotNone(lock)
        self.assertIsNone(mutate.acquire_lock(self.s), "a second gate on the same scratch tree")
        os.remove(lock)
        with open(lock, "w") as f:
            f.write("999999")  # no such process
        again = mutate.acquire_lock(self.s)
        self.assertEqual(again, lock)
        with open(lock) as f:
            self.assertEqual(int(f.read()), os.getpid())


class Registry(unittest.TestCase):
    def test_ids_unique_and_fields_present(self):
        ids = [m["id"] for m in mutants.MUTANTS]
        self.assertEqual(len(ids), len(set(ids)))
        for m in mutants.MUTANTS:
            for k in ("id", "milestone", "file", "test", "fault", "old", "new", "expect"):
                self.assertIn(k, m, m.get("id"))
            self.assertIn(m["test"], mutants.TESTS, m["id"])

    def test_each_mutant_applies_once_to_the_real_tree(self):
        root = os.path.dirname(HERE)
        for m in mutants.MUTANTS:
            path = os.path.join(root, m["file"])
            if m["file"].startswith("tools/") and not os.path.exists(path):
                continue  # tools/ is built by scripts/setup_tools.sh
            with open(path) as f:
                self.assertEqual(f.read().count(m["old"]), 1, m["id"])

    def test_tests_declare_their_dependencies(self):
        root = os.path.dirname(HERE)
        for name, t in mutants.TESTS.items():
            self.assertTrue(t.get("deps"), name)
            script = next(a for a in t["cmd"] if a.startswith("tests/"))
            self.assertIn(script, t["deps"], name)
            for d in t["deps"]:
                if not d.startswith("tools/"):
                    self.assertTrue(os.path.exists(os.path.join(root, d)), (name, d))

    def test_classify_shapes(self):
        t = dict(timeout=1, ran=r"^Ran \d+")
        m = dict(expect="test_add")
        self.assertEqual(mutate.classify(m, t, 1, "FAIL: test_add (x.T.test_add)\nRan 3 tests")[0], "KILLED")
        self.assertEqual(mutate.classify(m, t, 1, "ERROR: test_add (x.T.test_add)\nRan 3 tests")[0], "KILLED")
        self.assertEqual(mutate.classify(m, t, 1, "FAIL: test_sign (x.T.test_sign)\nRan 3 tests")[0], "KILLED-OTHER")
        self.assertEqual(mutate.classify(m, t, 0, "Ran 3 tests")[0], "SURVIVED")
        self.assertEqual(mutate.classify(m, t, None, "")[0], "TIMEOUT")
        self.assertEqual(mutate.classify(m, t, 1, "Traceback")[0], "INVALID")
        lt = dict(timeout=1, ran=r"^(pass|FAIL): xex_")
        lm = dict(expect="xex_basic_wrap_test")
        self.assertEqual(mutate.classify(lm, lt, 1, "pass: xex_a\nFAIL: xex_basic_wrap_test\n  FAIL x")[0], "KILLED")


if __name__ == "__main__":
    unittest.main(verbosity=2)
