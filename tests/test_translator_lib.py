# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Tests of tests/translator_lib.sh (the translator runner's harness cache and step deadline), on
# temporary folders with fake toolchains: no build, no game data.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_translator_lib.py [test ...]
#
# Expected values are derived from the requirement, not from the code: the cache key must change
# whenever anything a harness is compiled from changes, and must not change otherwise; a command
# past its deadline exits 124 (coreutils timeout's convention) within about the deadline, and a
# signal death exits 128 + the signal number (bash's convention), so a hang never looks like a trap.
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "translator_lib.sh")


def lib(*args, timeout=30):
    p = subprocess.run(["bash", LIB] + [str(a) for a in args], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout.strip(), p.stderr


def write(path, text, mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    if mode:
        os.chmod(path, mode)


class CacheKey(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.tmp.name
        self.xr = os.path.join(t, "XenonRecomp")
        write(os.path.join(self.xr, "XenonUtils", "ppc_context.h"), "#define A 1\n")
        write(os.path.join(self.xr, "thirdparty", "simde", "simde", "x86", "sse.h"), "// simde\n")
        write(os.path.join(self.xr, "thirdparty", "fmt", "include", "fmt", "core.h"), "// fmt\n")
        write(os.path.join(self.xr, "build", "thirdparty", "fmt", "libfmt.a"), "archive v1\n")
        self.doubles = os.path.join(t, "harness_doubles.h")
        write(self.doubles, "// doubles\n")
        self.runner = os.path.join(t, "runner.sh")
        write(self.runner, "# runner v1\n")
        self.cxx = os.path.join(t, "cxx")
        write(self.cxx, "#!/bin/sh\necho 'fake clang 1.0'\n", 0o755)
        self.flags = ["-O1", "-std=c++20"]
        self.out = os.path.join(t, "out")
        write(os.path.join(self.out, "main.cpp"), "int main() {}\n")
        write(os.path.join(self.out, "fixture.cpp"), "void f() {}\n")

    def tearDown(self):
        self.tmp.cleanup()

    def tool(self, flags=None):
        code, out, err = lib("cache_tool_key", self.xr, self.doubles, self.cxx, self.runner, "--",
                             *(self.flags if flags is None else flags))
        self.assertEqual(code, 0, err)
        self.assertRegex(out, r"^[0-9a-f]{64}$")
        return out

    def harness(self, tool=None):
        code, out, err = lib("cache_harness_key", tool or self.tool(), self.out)
        self.assertEqual(code, 0, err)
        self.assertRegex(out, r"^[0-9a-f]{64}$")
        return out

    def assertChanges(self, change):
        before = self.harness()
        change()
        self.assertNotEqual(self.harness(), before)

    def test_stable(self):
        self.assertEqual(self.harness(), self.harness())

    def test_generated_code(self):
        self.assertChanges(lambda: write(os.path.join(self.out, "fixture.cpp"), "void f() { g(); }\n"))

    def test_generated_file_name(self):
        # Same bytes in the same order, but a .h is not compiled (the runner compiles out/*.cpp).
        self.assertChanges(lambda: os.rename(os.path.join(self.out, "fixture.cpp"), os.path.join(self.out, "fixture.h")))

    def test_generated_file_added(self):
        self.assertChanges(lambda: write(os.path.join(self.out, "extra.h"), "\n"))

    def test_xenonutils_header(self):
        self.assertChanges(lambda: write(os.path.join(self.xr, "XenonUtils", "ppc_context.h"), "#define A 2\n"))

    def test_simde_header(self):
        self.assertChanges(lambda: write(os.path.join(self.xr, "thirdparty", "simde", "simde", "x86", "sse.h"), "// 2\n"))

    def test_fmt_header(self):
        self.assertChanges(lambda: write(os.path.join(self.xr, "thirdparty", "fmt", "include", "fmt", "core.h"), "// 2\n"))

    def test_libfmt(self):
        self.assertChanges(lambda: write(os.path.join(self.xr, "build", "thirdparty", "fmt", "libfmt.a"), "archive v2\n"))

    def test_doubles_header(self):
        self.assertChanges(lambda: write(self.doubles, "// doubles 2\n"))

    def test_runner(self):
        self.assertChanges(lambda: write(self.runner, "# runner v2\n"))

    def test_compiler_version(self):
        self.assertChanges(lambda: write(self.cxx, "#!/bin/sh\necho 'fake clang 2.0'\n", 0o755))

    def test_flags(self):
        before = self.harness()
        self.assertNotEqual(self.harness(self.tool(["-O2", "-std=c++20"])), before)

    def test_unrelated_file_ignored(self):
        before = self.harness()
        write(os.path.join(self.xr, "XenonRecomp", "recompiler.cpp"), "// not compiled into a harness\n")
        self.assertEqual(self.harness(), before)


class Prune(unittest.TestCase):
    def test_size_cap_keeps_the_newest(self):
        # Five entries of 64 KiB each (du -sk counts whole blocks, so each is at least 64 KiB), the
        # newest last; a 200 KiB cap keeps the three newest (192 KiB) and nothing else is touched.
        with tempfile.TemporaryDirectory() as t:
            names = ["%064x" % i for i in range(5)]
            for i, n in enumerate(names):
                write(os.path.join(t, n, "harness"), "x" * 65536)
                os.utime(os.path.join(t, n), (1000 + i, 1000 + i))
            write(os.path.join(t, "README"), "not an entry\n")
            code, _, err = lib("cache_prune", t, 200)
            self.assertEqual(code, 0, err)
            self.assertEqual(sorted(os.listdir(t)), sorted(names[2:] + ["README"]))

    def test_entry_larger_than_the_cap_is_removed(self):
        with tempfile.TemporaryDirectory() as t:
            n = "%064x" % 7
            write(os.path.join(t, n, "harness"), "x" * 65536)
            self.assertEqual(lib("cache_prune", t, 16)[0], 0)
            self.assertEqual(os.listdir(t), [])


class Bounded(unittest.TestCase):
    def test_exit_code_passes_through(self):
        self.assertEqual(lib("bounded", 5, "sh", "-c", "exit 3")[0], 3)
        self.assertEqual(lib("bounded", 5, "true")[0], 0)

    def test_output_passes_through(self):
        self.assertEqual(lib("bounded", 5, "echo", "hello")[1], "hello")

    def test_deadline(self):
        t0 = time.time()
        code, _, err = lib("bounded", 1, "sleep", "20")
        self.assertEqual(code, 124)
        self.assertIn("TIMEOUT after 1s", err)
        self.assertLess(time.time() - t0, 5)

    def test_signal_is_not_a_timeout(self):
        code = lib("bounded", 5, "sh", "-c", "kill -TRAP $$")[0]
        self.assertEqual(code, 128 + signal.SIGTRAP)


if __name__ == "__main__":
    unittest.main(verbosity=2)
