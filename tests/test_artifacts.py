# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Tests of scripts/artifacts.py (storage inventory, dry-run plan, reviewed apply) on synthetic
# repository trees in temporary folders. Expected behaviour comes from the requirement and the
# data-retention method: dry run first; delete only a reviewed manifest's paths,
# each re-validated; never evidence, sources, unmarked data, symlinks or anything in use.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_artifacts.py
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "scripts"))
import artifacts  # noqa: E402

IDLE = ["no-such-runner-pattern-7f3a"]


def put(root, rel, text="x" * 1000):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        f.write(text)


class Artifacts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.realpath(self.tmp.name)
        r = self.root
        put(r, "build/tests/translator/fx/out/a.cpp")
        put(r, "build/mutate/build/tests/loader/xex_a.log")
        put(r, "build/mutate/tools/XenonRecomp/build/XenonRecomp/XenonRecomp")
        put(r, "build/tests/translator_cache/%064x/harness" % 1)
        put(r, "build/old-temp/.retention.json", json.dumps({"class": "temp-build", "status": "done",
                                                               "done_at": "2026-11-05T08:00:00Z", "note": "results in research"}))
        put(r, "build/old-temp/bin")
        put(r, "build/active-temp/.retention.json", json.dumps({"class": "temp-build", "status": "active"}))
        put(r, "build/unmarked/bin")
        put(r, "research/m1/a.log", "same")
        put(r, "research/m1/b.log", "same")
        put(r, "ppc/x.cpp")
        put(r, "tests/translator/fixture.s")
        self.manifest = os.path.join(r, "research", "storage", "plan.json")
        self.audit = os.path.join(r, "research", "storage", "audit.jsonl")

    def tearDown(self):
        self.tmp.cleanup()

    def run_quiet(self, fn, *args, **kw):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            code = fn(*args, **kw)
        return code, buf.getvalue()

    def plan(self):
        code, out = self.run_quiet(artifacts.cmd_plan, self.root, self.manifest)
        self.assertEqual(code, 0, out)
        with open(self.manifest) as f:
            return json.load(f), out

    def apply(self, approve=None, man=None):
        man = man or json.load(open(self.manifest))
        return self.run_quiet(artifacts.cmd_apply, self.root, self.manifest, approve or man["id"], self.audit,
                              patterns=IDLE)

    def exists(self, rel):
        return os.path.lexists(os.path.join(self.root, rel))

    def test_plan_is_a_dry_run_listing_only_disposable_paths(self):
        man, out = self.plan()
        self.assertEqual(sorted(i["path"] for i in man["items"]),
                         ["build/mutate/build/tests/loader", "build/old-temp", "build/tests/translator"])
        self.assertIn("dry run: nothing deleted", out)
        self.assertIn("duplicate (kept, report only): research/m1/a.log, research/m1/b.log", out)
        for rel in ("build/tests/translator", "build/old-temp", "research/m1/a.log", "research/m1/b.log"):
            self.assertTrue(self.exists(rel), rel)

    def test_apply_deletes_exactly_the_manifest(self):
        self.plan()
        code, out = self.apply()
        self.assertEqual(code, 0, out)
        for rel in ("build/tests/translator", "build/mutate/build/tests/loader", "build/old-temp"):
            self.assertFalse(self.exists(rel), rel)
        for rel in ("build/mutate/tools/XenonRecomp/build/XenonRecomp/XenonRecomp",
                    "build/tests/translator_cache", "build/active-temp", "build/unmarked/bin",
                    "research/m1/a.log", "ppc/x.cpp", "tests/translator/fixture.s"):
            self.assertTrue(self.exists(rel), rel)
        with open(self.audit) as f:
            self.assertEqual(sum(1 for l in f if '"deleted"' in l), 3)

    def test_wrong_approval_or_edited_manifest_refuses(self):
        man, _ = self.plan()
        self.assertEqual(self.apply(approve="000000000000")[0], 2)
        man["items"].append(dict(path="build/unmarked", inode=0, bytes=1, reason="added"))
        with open(self.manifest, "w") as f:
            json.dump(man, f)
        self.assertEqual(self.apply(approve=man["id"])[0], 2)
        self.assertTrue(self.exists("build/unmarked/bin"))
        self.assertTrue(self.exists("build/tests/translator"))

    def test_replaced_directory_is_skipped(self):
        self.plan()
        p = os.path.join(self.root, "build/tests/translator")
        os.rename(p, p + ".moved")
        put(self.root, "build/tests/translator/new.txt")
        code, out = self.apply()
        self.assertIn("inode changed", out)
        self.assertTrue(self.exists("build/tests/translator/new.txt"))

    def test_symlink_and_escape_are_never_deleted(self):
        self.plan()
        outside = os.path.join(self.root, "research", "precious")
        put(self.root, "research/precious/keep.txt")
        p = os.path.join(self.root, "build/tests/translator")
        os.rename(p, p + ".moved")
        os.symlink(outside, p)
        code, out = self.apply()
        self.assertTrue(self.exists("research/precious/keep.txt"))
        man = json.load(open(self.manifest))
        for path in ("/etc", "build/../research", "research/m1"):
            self.assertIsNotNone(artifacts.validate(self.root, dict(path=path, inode=0)), path)

    def test_each_path_check_on_its_own(self):
        r = self.root
        ino = lambda rel: os.stat(os.path.join(r, rel)).st_ino
        # a symlink inside build/ to another build/ directory: only the symlink check stops it
        os.symlink(os.path.join(r, "build/unmarked"), os.path.join(r, "build/link-in"))
        self.assertEqual(artifacts.validate(r, dict(path="build/link-in", inode=ino("build/link-in"))), "symlink")
        # a real directory reached through a symlinked parent: only the resolved-path check stops it
        put(r, "research/elsewhere/translator/f")
        os.symlink(os.path.join(r, "research/elsewhere"), os.path.join(r, "build/linkdir"))
        self.assertEqual(artifacts.validate(r, dict(path="build/linkdir/translator", inode=ino("build/linkdir/translator"))),
                         "resolves outside build/")
        # '..' that resolves back inside build/, with the right inode: only the path check stops it
        self.assertEqual(artifacts.validate(r, dict(path="build/../build/old-temp", inode=ino("build/old-temp"))),
                         "not a path under build/")

    def test_marker_change_after_plan_is_skipped(self):
        self.plan()
        put(self.root, "build/old-temp/.retention.json", json.dumps({"class": "evidence", "status": "active"}))
        code, out = self.apply()
        self.assertIn("marker missing or no longer temp-build/done", out)
        self.assertTrue(self.exists("build/old-temp/bin"))

    def test_live_gate_lock_or_runner_refuses(self):
        self.plan()
        put(self.root, "build/mutate/.gate.lock", str(os.getpid()))
        self.assertEqual(self.apply()[0], 2)
        self.assertTrue(self.exists("build/tests/translator"))
        os.remove(os.path.join(self.root, "build/mutate/.gate.lock"))
        # "; true": otherwise bash execs sleep in its own place and the runner name leaves argv
        p = subprocess.Popen(["bash", "-c", "sleep 30; true", "run_translator_tests.sh"])
        try:
            reasons = artifacts.active_runs(self.root, ["run_translator_tests.sh"])
            self.assertTrue(any("run_translator_tests.sh" in r for r in reasons), reasons)
        finally:
            p.kill()
            p.wait()

    def test_inventory_reports_sizes(self):
        code, out = self.run_quiet(artifacts.cmd_inventory, self.root)
        self.assertEqual(code, 0)
        self.assertRegex(out, r"research\s+0\.0 MiB")
        self.assertIn("build/tests/translator_cache", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
