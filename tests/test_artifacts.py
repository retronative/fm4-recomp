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


class Captures(unittest.TestCase):
    """plan --captures / apply on capture PNGs of recorded GPU runs: only the chosen
    classes, only in a marked scenario with its run log, never anything else under research/."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.realpath(self.tmp.name)
        r = self.root
        marker = json.dumps({"kind": "evidence", "created": "2026-11-09", "keep": "until M7 close"})
        # a recorded run: marker, log, ring + kept frames + another file
        put(r, "research/m5/scenario10/.retention.json", marker)
        put(r, "research/m5/scenario10/run/run1.err", "[stop 11] swap 5400")
        put(r, "research/m5/scenario10/run/run1.captures/frame_5390.png", "f" * 3000)
        put(r, "research/m5/scenario10/run/run1.captures/frame_5391.png", "f" * 3000)
        put(r, "research/m5/scenario10/run/run1.captures/keep_5340.png", "k" * 5000)
        put(r, "research/m5/scenario10/run/run1.captures/selfcheck_planted_GPU_STOP", "s")
        put(r, "research/m5/scenario10/shaders/vs.bin")
        # unmarked scenario (never), marked scenario without its log (never), marked scenario named in --keep
        put(r, "research/m4/scenario2/run/run1.err")
        put(r, "research/m4/scenario2/run/run1.captures/frame_1.png", "f" * 3000)
        put(r, "research/m5/scenario11/.retention.json", marker)
        put(r, "research/m5/scenario11/run/run1.captures/frame_1.png", "f" * 3000)
        put(r, "research/m5/scenario12/.retention.json", marker)
        put(r, "research/m5/scenario12/run/run1.err")
        put(r, "research/m5/scenario12/run/run1.captures/frame_1.png", "f" * 3000)
        put(r, "build/tests/translator/fx/out/a.cpp")
        self.manifest = os.path.join(r, "research", "storage", "plan.json")
        self.audit = os.path.join(r, "research", "storage", "audit.jsonl")

    def tearDown(self):
        self.tmp.cleanup()

    def quiet(self, fn, *args, **kw):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            code = fn(*args, **kw)
        return code, buf.getvalue()

    def plan(self, captures=("frame-ring",), keep=("scenario12",)):
        code, out = self.quiet(artifacts.cmd_plan, self.root, self.manifest, captures, keep)
        self.assertEqual(code, 0, out)
        with open(self.manifest) as f:
            return json.load(f), out

    def apply(self, approve=None):
        with open(self.manifest) as f:
            man = json.load(f)
        return self.quiet(artifacts.cmd_apply, self.root, self.manifest, approve or man["id"], self.audit, patterns=IDLE)

    def exists(self, rel):
        return os.path.lexists(os.path.join(self.root, rel))

    def test_plan_without_captures_never_lists_research(self):
        code, out = self.quiet(artifacts.cmd_plan, self.root, self.manifest)
        with open(self.manifest) as f:
            man = json.load(f)
        self.assertEqual([i["path"] for i in man["items"]], ["build/tests/translator"])

    def test_plan_lists_only_the_ring_of_recorded_unkept_runs(self):
        man, out = self.plan()
        caps = [i for i in man["items"] if i.get("kind") == "captures"]
        self.assertEqual([i["path"] for i in caps], ["research/m5/scenario10/run/run1.captures"])
        self.assertEqual(caps[0]["files"], 2)
        self.assertEqual(caps[0]["bytes"], 6000)  # the two ring frames, not the kept frame or the other file
        self.assertEqual(caps[0]["classes"], ["frame-ring"])
        self.assertIn("dry run: nothing deleted", out)
        self.assertTrue(self.exists("research/m5/scenario10/run/run1.captures/frame_5390.png"))

    def test_apply_removes_the_class_files_and_keeps_everything_else(self):
        self.plan()
        code, out = self.apply()
        self.assertEqual(code, 0, out)
        self.assertIn("deleted 2 files", out)
        for rel in ("research/m5/scenario10/run/run1.captures/frame_5390.png",
                    "research/m5/scenario10/run/run1.captures/frame_5391.png", "build/tests/translator"):
            self.assertFalse(self.exists(rel), rel)
        for rel in ("research/m5/scenario10/run/run1.captures", "research/m5/scenario10/run/run1.captures/keep_5340.png",
                    "research/m5/scenario10/run/run1.captures/selfcheck_planted_GPU_STOP", "research/m5/scenario10/run/run1.err",
                    "research/m5/scenario10/shaders/vs.bin", "research/m5/scenario10/.retention.json",
                    "research/m4/scenario2/run/run1.captures/frame_1.png", "research/m5/scenario11/run/run1.captures/frame_1.png",
                    "research/m5/scenario12/run/run1.captures/frame_1.png"):
            self.assertTrue(self.exists(rel), rel)
        with open(self.audit) as f:
            lines = [json.loads(l) for l in f]
        self.assertEqual([l["bytes"] for l in lines if "captures" in l["path"]], [6000])

    def test_keep_frames_class_only_when_asked(self):
        man, _ = self.plan(captures=("frame-ring", "keep-frames"))
        caps = [i for i in man["items"] if i.get("kind") == "captures"][0]
        self.assertEqual((caps["files"], caps["bytes"]), (3, 11000))
        code, out = self.apply()
        self.assertFalse(self.exists("research/m5/scenario10/run/run1.captures/keep_5340.png"))
        self.assertTrue(self.exists("research/m5/scenario10/run/run1.captures/selfcheck_planted_GPU_STOP"))

    def test_edited_classes_change_the_id(self):
        man, _ = self.plan()
        for it in man["items"]:
            if it.get("kind") == "captures":
                it["classes"] = ["frame-ring", "keep-frames"]
        with open(self.manifest, "w") as f:
            json.dump(man, f)
        self.assertEqual(self.apply(approve=man["id"])[0], 2)
        self.assertTrue(self.exists("research/m5/scenario10/run/run1.captures/keep_5340.png"))
        self.assertTrue(self.exists("research/m5/scenario10/run/run1.captures/frame_5390.png"))

    def test_each_capture_check_on_its_own(self):
        r = self.root
        good = "research/m5/scenario10/run/run1.captures"
        ino = os.stat(os.path.join(r, good)).st_ino
        item = lambda **kw: dict(dict(path=good, kind="captures", classes=["frame-ring"], inode=ino), **kw)
        self.assertIsNone(artifacts.validate(r, item()))
        self.assertEqual(artifacts.validate(r, item(path="research/m5/scenario10/run")), "not research/m<N>/<scenario>/run/<run>.captures")
        self.assertEqual(artifacts.validate(r, item(path="research/m5/scenario10/run/../run/run1.captures")),
                         "not research/m<N>/<scenario>/run/<run>.captures")
        self.assertEqual(artifacts.validate(r, item(path="research/m5/scenario11/run/run1.captures")), "no run log beside the captures")
        self.assertEqual(artifacts.validate(r, item(path="research/m4/scenario2/run/run1.captures")), "scenario has no .retention.json")
        self.assertEqual(artifacts.validate(r, item(classes=["everything"])), "unknown capture class")
        self.assertEqual(artifacts.validate(r, item(inode=ino + 1)), "replaced since the plan (inode changed)")
        # a symlinked captures directory, and one that resolves outside research/
        os.symlink(os.path.join(r, "build"), os.path.join(r, "research/m5/scenario10/run/run2.captures"))
        put(r, "research/m5/scenario10/run/run2.err")
        self.assertEqual(artifacts.validate(r, item(path="research/m5/scenario10/run/run2.captures")), "symlink")
        os.symlink(os.path.join(r, "build"), os.path.join(r, "research/m5/scenario13"))
        self.assertEqual(artifacts.validate(r, item(path="research/m5/scenario13/run/run1.captures", inode=0)), "gone")

    def test_marker_removed_after_plan_is_skipped(self):
        self.plan()
        os.remove(os.path.join(self.root, "research/m5/scenario10/.retention.json"))
        code, out = self.apply()
        self.assertIn("scenario has no .retention.json", out)
        self.assertTrue(self.exists("research/m5/scenario10/run/run1.captures/frame_5390.png"))

    def test_game_run_refuses_apply(self):
        self.plan()
        p = subprocess.Popen(["bash", "-c", "sleep 30; true", "gpu_run.sh"])
        try:
            reasons = artifacts.active_runs(self.root, ["gpu_run.sh"])
            self.assertTrue(any("gpu_run.sh" in x for x in reasons), reasons)
        finally:
            p.kill()
            p.wait()

    def test_cli_rejects_unknown_class(self):
        code, out = self.quiet(artifacts.main, ["plan", "--root", self.root, "--captures", "all", "--out", self.manifest])
        self.assertEqual(code, 2)
        self.assertFalse(self.exists("research/storage/plan.json"))


class Evidence(unittest.TestCase):
    """plan --evidence / apply on old run evidence: traces, profile XML, shader dumps and saves of
    marked scenario<K> directories that keep their run log; never logs, notes, unmarked or kept scenarios, named saves."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.realpath(self.tmp.name)
        r = self.root
        marker = json.dumps({"kind": "evidence", "created": "2026-11-09", "keep": "until M7 close"})
        put(r, "research/m5/scenario150/.retention.json", marker)
        put(r, "research/m5/scenario150/run/run1.err", "[stop 11]")
        put(r, "research/m5/scenario150/metal.trace/Trace1.run/data", "t" * 4000)
        put(r, "research/m5/scenario150/time_profile.xml", "x" * 2000)
        put(r, "research/m5/scenario150/shaders/vs.bin", "s" * 500)
        put(r, "research/m5/scenario150/user-data/content/1/save", "u" * 300)
        put(r, "research/m5/scenario150/cp_buckets.txt", "notes")
        put(r, "research/m5/scenario150/launcher.txt", "launcher")
        put(r, "research/m5/scenario250/.retention.json", marker)  # numbered 250: outside --before 200
        put(r, "research/m5/scenario250/run/run1.err")
        put(r, "research/m5/scenario250/metal.trace/d", "t" * 4000)
        put(r, "research/m5/scenario142/.retention.json", marker)  # named in --keep
        put(r, "research/m5/scenario142/run/run1.err")
        put(r, "research/m5/scenario142/metal.trace/d", "t" * 4000)
        put(r, "research/m5/scenario151/metal.trace/d", "t" * 4000)  # unmarked
        put(r, "research/m5/scenario151/run/run1.err")
        put(r, "research/m5/scenario152/.retention.json", marker)  # no run log
        put(r, "research/m5/scenario152/metal.trace/d", "t" * 4000)
        put(r, "research/m4/save_c14/.retention.json", marker)  # a named save: never
        put(r, "research/m4/save_c14/user-data/content/1/save", "u" * 300)
        put(r, "research/m4/save_c14/run/run1.err")
        self.manifest = os.path.join(r, "research", "storage", "plan.json")
        self.audit = os.path.join(r, "research", "storage", "audit.jsonl")

    def tearDown(self):
        self.tmp.cleanup()

    def quiet(self, fn, *args, **kw):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            code = fn(*args, **kw)
        return code, buf.getvalue()

    def plan(self, evidence=artifacts.EVIDENCE_CLASSES, before=200):
        code, out = self.quiet(artifacts.cmd_plan, self.root, self.manifest, (), ("scenario142",), evidence, before)
        self.assertEqual(code, 0, out)
        with open(self.manifest) as f:
            return json.load(f)

    def test_plan_lists_only_old_marked_logged_unkept_scenarios(self):
        man = self.plan()
        ev = sorted((i["path"], i["classes"][0], i["bytes"]) for i in man["items"] if i.get("kind") == "evidence")
        self.assertEqual(ev, [("research/m5/scenario150/metal.trace", "metal-trace", 4000),
                              ("research/m5/scenario150/shaders", "shader-dumps", 500),
                              ("research/m5/scenario150/time_profile.xml", "profile-xml", 2000),
                              ("research/m5/scenario150/user-data", "saves", 300)])

    def test_before_is_optional(self):
        man = self.plan(evidence=("metal-trace",), before=None)
        self.assertEqual(sorted(i["path"] for i in man["items"] if i.get("kind") == "evidence"),
                         ["research/m5/scenario150/metal.trace", "research/m5/scenario250/metal.trace"])

    def test_apply_deletes_the_planned_evidence_and_keeps_logs_and_notes(self):
        man = self.plan()
        code, out = self.quiet(artifacts.cmd_apply, self.root, self.manifest, man["id"], self.audit, patterns=IDLE)
        self.assertEqual(code, 0, out)
        for rel in ("research/m5/scenario150/metal.trace", "research/m5/scenario150/time_profile.xml",
                    "research/m5/scenario150/shaders", "research/m5/scenario150/user-data"):
            self.assertFalse(os.path.lexists(os.path.join(self.root, rel)), rel)
        for rel in ("research/m5/scenario150/run/run1.err", "research/m5/scenario150/cp_buckets.txt",
                    "research/m5/scenario150/launcher.txt", "research/m5/scenario150/.retention.json",
                    "research/m5/scenario250/metal.trace", "research/m5/scenario142/metal.trace", "research/m5/scenario151/metal.trace",
                    "research/m5/scenario152/metal.trace", "research/m4/save_c14/user-data"):
            self.assertTrue(os.path.lexists(os.path.join(self.root, rel)), rel)

    def test_log_removed_after_plan_is_skipped(self):
        man = self.plan()
        os.remove(os.path.join(self.root, "research/m5/scenario150/run/run1.err"))
        code, out = self.quiet(artifacts.cmd_apply, self.root, self.manifest, man["id"], self.audit, patterns=IDLE)
        self.assertIn("skipped: no run log in the scenario", out)
        self.assertTrue(os.path.lexists(os.path.join(self.root, "research/m5/scenario150/metal.trace")))

    def test_milestone_filter(self):
        code, out = self.quiet(artifacts.cmd_plan, self.root, self.manifest, (), (), ("saves",), None, "m4")
        with open(self.manifest) as f:
            man = json.load(f)
        self.assertEqual([i["path"] for i in man["items"] if i.get("kind") == "evidence"], [])  # m4 has only a named save
        code, out = self.quiet(artifacts.cmd_plan, self.root, self.manifest, (), (), ("saves",), None, "m5")
        with open(self.manifest) as f:
            man = json.load(f)
        self.assertEqual([i["path"] for i in man["items"] if i.get("kind") == "evidence"], ["research/m5/scenario150/user-data"])

    def test_cli_rejects_unknown_evidence_class(self):
        code, out = self.quiet(artifacts.main, ["plan", "--root", self.root, "--evidence", "logs"])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
