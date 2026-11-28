#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""scripts/alpha/migrate-from-0.3.swift on synthetic user-data folders, one per case (adapted from 0.3.1's
tests/test_migrate_career.py, f7d26cd). Expected bytes come from the two layouts, not from the program:
- this engine's record (runtime/kernel/content.h): 0x134 bytes, device +0 and type +4 (big-endian), display name UTF-16BE
  from +8 (128 units), file name from +0x108 (42 bytes);
- 0.3.x's header (ShiftGlue content_manager.h XCONTENT_AGGREGATE_DATA): the same 0x134 bytes, xuid +0x138, title id +0x140,
  0x148 bytes in all.

    /opt/homebrew/bin/python3 -B -I tests/test_migrate_from_03.py    (compiles the tool once with xcrun swiftc; ~1 min)
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ACCOUNT, TITLE, TYPE = "B13EBABEBABEBABE", "4D530910", "00000001"
OLD_TIME, NEW_TIME = 1_900_000_000, 2_000_000_000


def record(name, display, padding=0x00):
    """This engine's 0x134-byte record, field by field; `padding` fills every byte the fields leave (the guest's garbage)."""
    out = bytearray([padding] * 0x134)
    out[0:8] = bytes([0, 0, 0, 1, 0, 0, 0, 1])
    text = display.encode("utf-16-be") + b"\0\0"
    out[8:8 + len(text)] = text
    out[0x108:0x108 + len(name) + 1] = name.encode("ascii") + b"\0"
    return bytes(out)


def header(name, display, size=0x148, title=TITLE, xuid=0):
    """0.3.x's header: the zero-padded record, then xuid at 0x138 and title id at 0x140."""
    out = bytearray(size)
    out[0:0x134] = record(name, display)
    out[0x138:0x140] = xuid.to_bytes(8, "big")
    out[0x140:0x144] = bytes.fromhex(title)
    return bytes(out)


class Migration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        kit = Path(cls.build.name) / "kit"
        (kit / "bin").mkdir(parents=True)
        cls.runner = kit / "migrate-from-0.3.sh"
        shutil.copy(ROOT / "scripts/alpha/migrate-from-0.3.sh", cls.runner)
        cls.runner.chmod(0o755)
        subprocess.run(["/usr/bin/xcrun", "swiftc", "-swift-version", "5", "-module-name", "FM4MigrateFrom03",
                        "-module-cache-path", str(Path(cls.build.name) / "modules"),
                        str(ROOT / "scripts/alpha/migrate-from-0.3.swift"), "-o", str(kit / "bin/fm4_migrate_from_03")],
                       check=True, timeout=300)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "user-data"
        self.ours = self.root / "content/1" / TYPE
        self.theirs = self.root / ACCOUNT / TITLE / TYPE
        self.headers = self.root / ACCOUNT / TITLE / "Headers" / TYPE
        self.state = self.root / ".fm4-from-0.3-saves-v1"
        self.ours.mkdir(parents=True)
        self.theirs.mkdir(parents=True)
        self.headers.mkdir(parents=True)

    def ours_package(self, name, files, display="User", padding=0xAA, when=OLD_TIME):
        self.write_tree(self.ours / name, files, when)
        (self.ours / (name + ".xcontent")).write_bytes(record(name, display, padding))

    def theirs_package(self, name, files, display="User", when=NEW_TIME, head=None):
        self.write_tree(self.theirs / name, files, when)
        (self.headers / (name + ".header")).write_bytes(head if head is not None else header(name, display))

    @staticmethod
    def write_tree(base, files, when):
        base.mkdir(parents=True)
        for rel, data in files.items():
            path = base / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            os.utime(path, (when, when))

    def migrate(self, ok=True):
        done = subprocess.run([str(self.runner), str(self.root)], capture_output=True, text=True, timeout=60)
        self.assertEqual(done.returncode == 0, ok, done.stdout + done.stderr)
        return done

    def backups(self):
        return sorted(p for p in self.root.iterdir() if p.name.startswith("saves-replaced-"))

    def snapshot(self):
        return {str(p.relative_to(self.root)): (p.read_bytes() if p.is_file() else None)
                for p in sorted(self.root.rglob("*")) if not str(p.relative_to(self.root)).startswith(".fm4-from-0.3")}

    def test_newer_03_copy_replaces_ours_and_ours_is_kept_in_a_backup(self):
        self.ours_package("ForzaProfile", {"ForzaProfile": b"career 0.2", "Thumbnails/Thumbnail_1.xdc": b"thumb"})
        self.theirs_package("ForzaProfile", {"ForzaProfile": b"career 0.3", "Thumbnails/Thumbnail_1.xdc": b"thumb"})
        self.migrate()
        self.assertEqual((self.ours / "ForzaProfile/ForzaProfile").read_bytes(), b"career 0.3")
        self.assertEqual((self.ours / "ForzaProfile/Thumbnails/Thumbnail_1.xdc").read_bytes(), b"thumb")
        # 0x134 bytes: 00000001 00000001, "User" in UTF-16BE and its terminator, zeros, "ForzaProfile\0" at 0x108, zeros.
        expected = (bytes.fromhex("0000000100000001") + "User".encode("utf-16-be") + bytes(0x108 - 16)
                    + b"ForzaProfile" + bytes(0x134 - 0x108 - 12))
        self.assertEqual(len(expected), 0x134)
        self.assertEqual((self.ours / "ForzaProfile.xcontent").read_bytes(), expected)
        [backup] = self.backups()
        self.assertEqual((backup / "ForzaProfile/ForzaProfile").read_bytes(), b"career 0.2")
        self.assertEqual((backup / "ForzaProfile.xcontent").read_bytes(), record("ForzaProfile", "User", 0xAA))
        self.assertEqual((self.theirs / "ForzaProfile/ForzaProfile").read_bytes(), b"career 0.3")  # the 0.3 copy stays
        self.assertTrue((self.headers / "ForzaProfile.header").is_file())
        self.assertEqual((self.state / "done").read_bytes(), b"1\n")
        self.assertEqual(sorted(p.name for p in self.state.iterdir()), ["done"])

    def test_identical_package_is_left_alone_and_no_backup_is_made(self):
        # A real "marketplace": the same (empty) folder, our record with guest padding, the header zero-padded.
        self.ours_package("marketplace", {}, display="Marketplace Data")
        self.theirs_package("marketplace", {}, display="Marketplace Data")
        before = (self.ours / "marketplace.xcontent").read_bytes()
        self.migrate()
        self.assertEqual((self.ours / "marketplace.xcontent").read_bytes(), before)
        self.assertEqual(self.backups(), [])
        self.assertTrue((self.state / "done").is_file())

    def test_package_only_in_03_is_added_and_one_only_here_is_kept(self):
        self.theirs_package("photo", {"img": b"03"}, display="夜景")
        self.ours_package("legacy", {"data": b"kept"})
        self.migrate()
        self.assertEqual((self.ours / "photo/img").read_bytes(), b"03")
        self.assertEqual((self.ours / "photo.xcontent").read_bytes(), record("photo", "夜景"))
        self.assertEqual((self.ours / "legacy/data").read_bytes(), b"kept")
        self.assertEqual(self.backups(), [])

    def test_host_metadata_is_not_copied_and_an_xuid_in_the_header_is_dropped(self):
        self.theirs_package("photo", {"img": b"03", ".DS_Store": b"finder", ".guest": b"guest"},
                            head=header("photo", "Photos", xuid=0xB13EBABEBABEBABE))
        self.migrate()
        self.assertFalse((self.ours / "photo/.DS_Store").exists())
        self.assertEqual((self.ours / "photo/.guest").read_bytes(), b"guest")
        self.assertEqual((self.ours / "photo.xcontent").read_bytes(), record("photo", "Photos"))

    def test_our_copy_saved_after_the_03_copy_is_refused_and_nothing_changes(self):
        self.ours_package("ForzaProfile", {"ForzaProfile": b"played here later"}, when=NEW_TIME + 60)
        self.theirs_package("ForzaProfile", {"ForzaProfile": b"career 0.3"})
        before = self.snapshot()
        done = self.migrate(ok=False)
        self.assertIn("was saved after the 0.3 copy", done.stderr)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.state / "done").exists())

    def test_unexpected_headers_and_folders_are_refused_and_nothing_changes(self):
        cases = {
            "size": header("photo", "Photos", size=0x14C),                        # a license mask appended
            "title": header("photo", "Photos", title="415607E6"),
            "name": header("other", "Photos"),
            "device": bytes.fromhex("0000000200000001") + header("photo", "Photos")[8:],
        }
        for label, head in cases.items():
            with self.subTest(label):
                shutil.rmtree(self.theirs / "photo", ignore_errors=True)
                self.theirs_package("photo", {"img": b"03"}, head=head)
                before = self.snapshot()
                self.migrate(ok=False)
                self.assertEqual(self.snapshot(), before)
                self.assertFalse((self.state / "done").exists())

    def test_folder_without_header_symlink_and_colon_name_are_refused(self):
        self.write_tree(self.theirs / "noheader", {"a": b"x"}, NEW_TIME)
        self.migrate(ok=False)
        shutil.rmtree(self.theirs / "noheader")
        self.theirs_package("photo", {"img": b"03"})
        (self.theirs / "photo/link").symlink_to("/etc/hosts")
        self.migrate(ok=False)
        (self.theirs / "photo/link").unlink()
        (self.theirs / "photo/2026-11-14 1751").write_bytes(b"colon-free")
        (self.theirs / "photo/a:b").write_bytes(b"colon")
        self.migrate(ok=False)
        self.assertFalse((self.ours / "photo").exists())
        (self.theirs / "photo/a:b").unlink()
        self.migrate()
        self.assertEqual((self.ours / "photo/2026-11-14 1751").read_bytes(), b"colon-free")

    def test_incomplete_package_here_is_refused(self):
        self.write_tree(self.ours / "photo", {"img": b"here"}, OLD_TIME)  # no .xcontent record
        self.theirs_package("photo", {"img": b"03"})
        self.migrate(ok=False)
        self.assertEqual((self.ours / "photo/img").read_bytes(), b"here")

    def test_header_without_folder_is_ignored(self):
        (self.headers / "gone.header").write_bytes(header("gone", "Gone"))
        done = self.migrate()
        self.assertIn("saves: gone: a 0.3 header without its folder", done.stderr)
        self.assertFalse((self.ours / "gone").exists())

    def test_the_refusal_is_the_last_line_after_earlier_notes(self):
        # play.sh merges the streams and shows the last line: a note printed before the refusal must not end up after it.
        (self.headers / "gone.header").write_bytes(header("gone", "Gone"))   # a note first
        self.write_tree(self.theirs / "noheader", {"a": b"x"}, NEW_TIME)        # then a refusal
        done = subprocess.run(["/bin/bash", "-c", '"$0" "$1" 2>&1', str(self.runner), str(self.root)],
                              capture_output=True, text=True, timeout=60)
        self.assertNotEqual(done.returncode, 0)
        lines = done.stdout.strip().splitlines()
        self.assertIn("gone: a 0.3 header without its folder", done.stdout)
        self.assertTrue(lines[-1].startswith("saves: ") and "has no header" in lines[-1], done.stdout)

    def test_done_marker_makes_it_run_once(self):
        self.theirs_package("photo", {"img": b"03"})
        self.migrate()
        shutil.rmtree(self.ours / "photo")
        (self.ours / "photo.xcontent").unlink()
        self.migrate()
        self.assertFalse((self.ours / "photo").exists())  # a deletion here stays deleted

    def test_without_a_03_account_nothing_is_written(self):
        shutil.rmtree(self.root / ACCOUNT)
        self.migrate()
        self.assertFalse(self.state.exists())

    def test_resumes_a_replacement_interrupted_after_the_old_record_moved(self):
        self.ours_package("ForzaProfile", {"ForzaProfile": b"career 0.2"})
        self.theirs_package("ForzaProfile", {"ForzaProfile": b"career 0.3"})
        backup = self.root / "saves-replaced-2026-11-26_120000"
        backup.mkdir()
        old_record = (self.ours / "ForzaProfile.xcontent").read_bytes()
        (self.ours / "ForzaProfile.xcontent").rename(backup / "ForzaProfile.xcontent")
        pending = self.state / "pending-ForzaProfile"
        self.write_tree(pending / "payload", {"ForzaProfile": b"career 0.3"}, NEW_TIME)
        (pending / "record").write_bytes(record("ForzaProfile", "User"))
        (pending / "inode").write_text(str((pending / "payload").stat().st_ino))
        (pending / "action").write_text("replace")
        (pending / "backup").write_text(backup.name)
        self.migrate()
        self.assertEqual((self.ours / "ForzaProfile/ForzaProfile").read_bytes(), b"career 0.3")
        self.assertEqual((self.ours / "ForzaProfile.xcontent").read_bytes(), record("ForzaProfile", "User"))
        self.assertEqual((backup / "ForzaProfile/ForzaProfile").read_bytes(), b"career 0.2")
        self.assertEqual((backup / "ForzaProfile.xcontent").read_bytes(), old_record)
        self.assertEqual(self.backups(), [backup])
        self.assertFalse(pending.exists())

    def test_play_runs_it_under_the_lock_before_the_game(self):
        text = (ROOT / "scripts/alpha/play.sh").read_text()
        lock = text.index('echo $$ > "$LOCK/pid"')
        call = text.index('"$HERE/migrate-from-0.3.sh" "$LOC_USER_DATA_ROOT"')
        game = text.index('"$DEADLINE" "$HERE/bin/fm4recomp"')
        self.assertLess(lock, call)
        self.assertLess(call, game)
        self.assertIn("pgrep -x 'fm4recomp|fm4_static_recomp'", text)


if __name__ == "__main__":
    unittest.main()
