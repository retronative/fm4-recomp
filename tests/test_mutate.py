# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Tests of the mutation gate itself (tests/mutate.py) on a toy tree in a temporary folder:
# every verdict, both FAIL-line shapes (unittest "FAIL: test_x" and the loader runner's
# "  FAIL <check>"), INVALID cases, a hanging mutant whose process group must be killed, and
# that the real tree is never touched. Plus the registry's own consistency.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_mutate.py
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
for name, ok in (("adds two numbers", toy.add(2, 3) == 5), ("signs a negative", toy.sign(-4) == -1)):
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
}


def M(id, old, new, expect, test="toy", **kw):
    return dict(id=id, milestone="T", file="survey/toy.py", test=test, fault=id, old=old, new=new, expect=expect, **kw)


class Gate(unittest.TestCase):
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

    def test_stale_bytecode_purged(self):
        # Two equal-size mutants written within a second: without the purge, Python may reuse the
        # first one's bytecode. Both must be judged on their own text.
        self.assertEqual(self.result(M("a", "return a + b", "return a - b", "test_add")), "KILLED")
        self.assertEqual(self.result(M("b", "return a + b", "return a + b + 0", "test_add",
                                       equivalent="adds zero")), "EQUIVALENT")


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
