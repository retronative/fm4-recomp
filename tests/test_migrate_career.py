#!/usr/bin/env python3
"""Save compatibility regression tests. Each case starts with an isolated profile."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ACCOUNT = "B13EBABEBABEBABE"
TITLE = "4D530910"
TYPE = "00000001"

def metadata(name, display):
    record = bytearray([0xaa] * 308)
    record[:8] = bytes.fromhex("0000000100000001")
    text = display.encode("utf-16-be") + bytes(2)
    record[8:8+len(text)] = text
    filename = name.encode("ascii") + bytes(1)
    record[264:264+len(filename)] = filename
    return bytes(record)

def native_header(name, display):
    header = bytearray(328)
    header[:8] = bytes.fromhex("0000000100000001")
    text = display.encode("utf-16-be")
    header[8:8+len(text)] = text
    header[264:264+len(name)] = name.encode("ascii")
    header[320:324] = bytes.fromhex(TITLE)
    return bytes(header)

class Migration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.runner = Path(cls.build.name) / "kit/migrate-career.sh"
        cls.runner.parent.mkdir()
        cls.runner.write_bytes((ROOT/"scripts/alpha/migrate-career.sh").read_bytes())
        cls.runner.chmod(0o755)
        bindir=cls.runner.parent/"bin"
        bindir.mkdir()
        subprocess.run(["xcrun", "swiftc", "-module-cache-path", str(Path(cls.build.name)/"modules"),
                        str(ROOT/"scripts/alpha/migrate-career.swift"), "-o", str(bindir/"fm4_migrate_career")], check=True)
    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.old = self.root/"content/1"/TYPE
        self.old.mkdir(parents=True)
        self.title = self.root/ACCOUNT/TITLE
        self.saves = self.title/TYPE
        self.headers = self.title/"Headers"/TYPE
    def add(self, name, display):
        (self.old/name/"nested").mkdir(parents=True)
        (self.old/name/"nested/test").write_bytes(name.encode()*8)
        (self.old/name/".guest").write_bytes(b"guest data")
        (self.old/name/".DS_Store").write_bytes(b"host metadata")
        (self.old/(name+".xcontent")).write_bytes(metadata(name,display))
    def migrate(self, ok=True):
        result=subprocess.run([str(self.runner),str(self.root)],capture_output=True,text=True)
        self.assertEqual(result.returncode == 0, ok, result.stdout+result.stderr)
    def test_imports_all_saved_packages_and_keeps_old(self):
        for name,display in [("ForzaProfile","User"),("photo","夜景"),("marketplace","Marketplace Data")]:
            self.add(name,display)
        dlc=self.old.parent/"00000002";dlc.mkdir();(dlc/"installed-disc-content").write_bytes(b"disc")
        self.migrate()
        for name,display in [("ForzaProfile","User"),("photo","夜景"),("marketplace","Marketplace Data")]:
            self.assertEqual((self.headers/(name+".header")).read_bytes(),native_header(name,display))
            self.assertEqual((self.saves/name/".guest").read_bytes(),b"guest data")
            self.assertFalse((self.saves/name/".DS_Store").exists())
            self.assertEqual((self.saves/name/"nested/test").read_bytes(),(self.old/name/"nested/test").read_bytes())
            self.assertEqual((self.old/(name+".xcontent")).read_bytes(), metadata(name,display))
        self.assertFalse((self.title/"00000002").exists())
        self.assertFalse((self.root/"shiftglue").exists())
    def test_existing_destination_wins_and_retry_does_not_restore_deleted_photo(self):
        self.add("ForzaProfile","User");self.add("photo","Photos")
        (self.saves/"ForzaProfile").mkdir(parents=True)
        (self.saves/"ForzaProfile/native").write_bytes(b"newer")
        self.headers.mkdir(parents=True)
        (self.headers/"ForzaProfile.header").write_bytes(b"newer metadata")
        self.migrate()
        self.assertEqual((self.saves/"ForzaProfile/native").read_bytes(),b"newer")
        self.assertEqual((self.headers/"ForzaProfile.header").read_bytes(),b"newer metadata")
        self.assertFalse((self.saves/"ForzaProfile/nested").exists())
        self.assertTrue((self.headers/"photo.header").is_file())
        import shutil
        shutil.rmtree(self.saves/"photo");(self.headers/"photo.header").unlink()
        self.migrate()
        self.assertFalse((self.saves/"photo").exists())
    def test_rejects_name_mismatch_and_source_symlinks(self):
        self.add("ForzaProfile","User")
        (self.old/"ForzaProfile.xcontent").write_bytes(metadata("../else","User"))
        self.migrate(False)
        self.assertFalse((self.saves/"ForzaProfile").exists())
        (self.old/"ForzaProfile.xcontent").write_bytes(metadata("ForzaProfile","User"))
        (self.old/"ForzaProfile/file-link").symlink_to("/etc/hosts")
        self.migrate(False)
        self.assertFalse((self.headers/"ForzaProfile.header").exists())
    def test_resumes_after_payload_rename_before_header(self):
        self.add("photo","Photos")
        self.headers.mkdir(parents=True)
        payload=self.saves/"photo";payload.mkdir(parents=True)
        (payload/"original").write_bytes(b"staged")
        pending=self.root/".fm4-legacy-saves-v1/pending-photo";pending.mkdir(parents=True)
        (pending/"inode").write_text(str(payload.stat().st_ino))
        (pending/"header").write_bytes(native_header("photo","Photos"))
        self.migrate()
        self.assertEqual((payload/"original").read_bytes(),b"staged")
        self.assertEqual((self.headers/"photo.header").read_bytes(),native_header("photo","Photos"))
        self.assertFalse(pending.exists())
    def test_incomplete_unowned_destination_remains_unmodified(self):
        self.add("photo","Photos")
        (self.saves/"photo").mkdir(parents=True)
        (self.saves/"photo/original").write_bytes(b"native")
        self.migrate(False)
        self.assertEqual((self.saves/"photo/original").read_bytes(),b"native")
        self.assertFalse((self.headers/"photo.header").exists())
    def test_native_symlink_stays_untouched_and_empty_home_is_valid(self):
        self.migrate()
        self.add("ForzaProfile","User")
        elsewhere=self.root/"elsewhere";elsewhere.mkdir()
        (self.root/ACCOUNT).symlink_to(elsewhere,target_is_directory=True)
        self.migrate(False)
        self.assertEqual(list(elsewhere.iterdir()),[])
if __name__=="__main__":
    unittest.main()
