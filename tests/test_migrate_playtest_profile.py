#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import concurrent.futures
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/alpha/migrate-playtest-profile.sh"
CAREER = Path("B13EBABEBABEBABE/4D530910/00000001/ForzaProfile/ForzaProfile")
HEADER = Path("B13EBABEBABEBABE/4D530910/Headers/00000001/ForzaProfile.header")


class Migration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name).resolve()
        self.old = base / "FM4 Recomp ShiftGlue Playtest"
        self.layout = self.old / "shiftglue"
        self.new = base / "FM4 Recomp"
        self.user = base / "External Disk" / "FM4 saves"
        self.new.mkdir()
        self.user.mkdir(parents=True)

    def put(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)

    def migrate(self, *args):
        return subprocess.run(["/bin/bash", str(SCRIPT), *(args or (self.old, self.new, self.user))],
                              capture_output=True, text=True)

    def seed(self):
        self.put(self.layout / "user" / CAREER, b"old progress")
        self.put(self.layout / "user" / HEADER, b"old header")
        self.put(self.layout / "user/achievements/profile.json", b"old awards")
        self.put(self.layout / "config/graphics.toml", b"old graphics")
        self.put(self.layout / "cache/transient", b"cached")
        self.put(self.old / "locations.conf", b"old path")

    def test_import_and_repeat(self):
        self.seed()
        result = self.migrate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.user / CAREER).read_bytes(), b"old progress")
        self.assertEqual((self.user / HEADER).read_bytes(), b"old header")
        self.assertEqual((self.user / "achievements/profile.json").read_bytes(), b"old awards")
        self.assertEqual((self.new / "config/graphics.toml").read_bytes(), b"old graphics")
        self.assertFalse((self.new / "locations.conf").exists())
        self.assertFalse((self.new / "cache").exists())
        self.assertEqual((self.layout / "user" / CAREER).read_bytes(), b"old progress")
        (self.user / CAREER).write_bytes(b"new progress")
        (self.new / "config/graphics.toml").unlink()
        self.assertEqual(self.migrate().returncode, 0)
        self.assertEqual((self.user / CAREER).read_bytes(), b"new progress")
        self.assertFalse((self.new / "config/graphics.toml").exists())

    def test_existing_career_and_setting_win(self):
        self.seed()
        self.put(self.user / CAREER, b"my existing career")
        self.put(self.user / HEADER, b"my active header")
        self.put(self.new / "config/graphics.toml", b"my settings")
        result = self.migrate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(self.layout / "user"), result.stderr)
        self.assertEqual((self.user / CAREER).read_bytes(), b"my existing career")
        self.assertEqual((self.user / HEADER).read_bytes(), b"my active header")
        self.assertFalse((self.user / "achievements/profile.json").exists())
        self.assertEqual((self.new / "config/graphics.toml").read_bytes(), b"my settings")
        self.assertEqual((self.layout / "user" / HEADER).read_bytes(), b"old header")

    def test_missing_career_inside_an_existing_account(self):
        self.seed()
        other = self.user / CAREER.parents[1] / "photo/unique"
        self.put(other, b"only here")
        result = self.migrate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.user / CAREER).read_bytes(), b"old progress")
        self.assertEqual((self.user / HEADER).read_bytes(), b"old header")
        self.assertEqual(other.read_bytes(), b"only here")

    def test_partial_career_requires_matching_counterpart(self):
        self.seed()
        self.put(self.user / CAREER, b"another user")
        self.assertNotEqual(self.migrate().returncode, 0)
        self.assertFalse((self.user / HEADER).exists())
        self.put(self.user / CAREER, b"old progress")
        result = self.migrate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.user / HEADER).read_bytes(), b"old header")

    def test_missing_source_and_unavailable_destination(self):
        self.assertEqual(self.migrate().returncode, 0)
        self.seed()
        self.user.rmdir()
        self.assertNotEqual(self.migrate().returncode, 0)
        self.assertFalse(self.user.exists())
        self.assertFalse((self.new / "config/graphics.toml").exists())

    def test_unsafe_path_leaves_no_partial_account(self):
        self.seed()
        (self.layout / "user" / HEADER).unlink()
        (self.layout / "user" / HEADER).symlink_to(self.new)
        self.assertNotEqual(self.migrate().returncode, 0)
        self.assertFalse((self.user / CAREER).exists())
        (self.layout / "user" / HEADER).unlink()
        self.put(self.layout / "user" / HEADER, b"old header")
        self.assertNotEqual(self.migrate(self.old, self.new, self.layout / "user").returncode, 0)
        alias = self.new / "saves"
        alias.symlink_to(self.user)
        self.assertNotEqual(self.migrate(self.old, self.new, alias).returncode, 0)

    def test_parallel_launch(self):
        self.seed()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: self.migrate(), range(2)))
        for result in results:
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.user / CAREER).read_bytes(), b"old progress")
        self.assertEqual((self.user / HEADER).read_bytes(), b"old header")


if __name__ == "__main__":
    unittest.main()
