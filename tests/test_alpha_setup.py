# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The alpha's storage locations and setup steps (scripts/alpha/locations.sh, setup.sh), run by macOS's /bin/bash (3.2, as on the
# player's Mac) in a scratch kit with fake tools; no game bytes, no real extraction.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_alpha_setup.py      (~30 s: a cancelled run waits for setup.sh's 2 s poll)
#
# Fakes (written here): bin/extract-xiso, bin/gdfx and bin/xexsurvey act on a text "ISO" whose first line names the disc
# (disc1, disc2, nopackages, notxbox) and write small known files; df (first on PATH) reports the free KiB and mount point given
# by FAKE_DF ("<path prefix>|<free KiB>|<mount>;..."; longest prefix wins). The success paths need the build's pinned SHA-256s to
# match the fake files: the scratch copy of setup.sh has each pin replaced by the fake file's SHA-256 (every pin must occur
# exactly once, so a changed pin fails the test rather than going untested); the wrong-disc and unsupported cases run the
# unmodified script. Expected sizes are the script's documented constants: Disc 1 8091799701 bytes, Disc 2 3169575934 bytes plus
# 3004952576 installed, plus a 1 GiB margin per volume.
import hashlib
import os
import re
import shlex
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ALPHA = os.environ.get("FM4_ALPHA_DIR", os.path.join(HERE, "..", "scripts", "alpha"))
BASH = "/bin/bash"
GB_KB = 1024 * 1024
PKGDIR = "content/0000000000000000/4d530910/00000002"
PACKAGES = ["4d53091000000001", "4d53091000000002", "4d53091000000003", "4d53091000000004"]
PACKAGE_SIZES = [730218496, 840794112, 579788800, 854151168]   # setup.sh PACKAGE_SIZES

FAKE_XISO = r'''#!/bin/bash
set -euo pipefail
[ "$1 $2" = "-x -d" ] || exit 64
out="$3"; kind="$(head -1 "$4")"
case "${FAKE_XISO:-}" in
  fail) echo "fake extract-xiso: failing" >&2; exit 1 ;;
  sleep) mkdir -p "$out"; echo $$ > "$FAKE_PIDFILE"; exec sleep 30 ;;
esac
mkdir -p "$out"
if [ "$kind" = disc1 ]; then
  for f in default.xex XMediaFacade_default.xex SpeechFacade_default.xex; do echo "fake disc1 $f" > "$out/$f"; done
else
  echo "fake disc2 default.xex" > "$out/default.xex"
  mkdir -p "$out/content/0000000000000000/4d530910/00000002"
  for n in 1 2 3 4; do echo "fake package $n" > "$out/content/0000000000000000/4d530910/00000002/4d5309100000000$n"; done
fi
'''
FAKE_GDFX = r'''#!/bin/bash
set -euo pipefail
kind="$(head -1 "$1")"
case "$kind" in
  disc1) printf 'default.xex\t18\t10\t80\n' > "$2" ;;
  disc2)
    { printf 'content\t20\t800\t10\n'; printf 'content/0000000000000000/4d530910/00000002/4d5309100000000%s\t%x\t%x\t80\n' \
        1 100 730218496 2 200 840794112 3 300 579788800 4 400 854151168; printf 'default.xex\t30\t10\t80\n'; } > "$2" ;;
  nopackages) printf 'default.xex\t30\t10\t80\n' > "$2" ;;
  *) echo "fake gdfx: no GDFX volume" >&2; exit 1 ;;
esac
'''
FAKE_XEXSURVEY = r'''#!/bin/bash
set -euo pipefail
mkdir -p "$2"; echo "image of $(basename "$1")" > "$2/image.bin"
'''
FAKE_DF = r'''#!/bin/bash
# df -Pk <path>, from FAKE_DF; without it, plenty of space on "/".
path="$2"; best=""; free=$((1000 * 1024 * 1024)); mount=/
IFS=';' read -r -a rules <<< "${FAKE_DF:-}"
for r in ${rules[@]+"${rules[@]}"}; do
  prefix="${r%%|*}"; rest="${r#*|}"
  case "$path/" in "$prefix"/*) [ ${#prefix} -le ${#best} ] || { best="$prefix"; free="${rest%%|*}"; mount="${rest#*|}"; } ;; esac
done
echo "Filesystem 1024-blocks Used Available Capacity Mounted on"
echo "fake 1 0 $free 0% $mount"
'''


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


# The build's pins (setup.sh) -> the SHA-256 of the fake file each one checks.
DISC1_PINS = {
    "3618e85af83e972ddcc1b0d0dd808e78e29ab4ab3890a15b31c210a6a3e93e09": sha("fake disc1 default.xex\n"),
    "560bb53fc9a335399e82ba92121410e6781841cbabe3e9dfbf0de43dde7258e9": sha("fake disc1 XMediaFacade_default.xex\n"),
    "b8a5e5137bb18ec42c7e0e2fe5b3e5e760e11e0b0d5ddf0098186995aa7c0f5e": sha("fake disc1 SpeechFacade_default.xex\n"),
    "92462a7d11062b4ce1878d69a34de2bc485027f36a88dd829dac5d33e5960b33": sha("default.xex\t18\t10\t80\n"),
    "776aa2503a6b0a86b449f2f428372882f0b380f532551b166ef2a103f8096d86": sha("image of default.xex\n"),
    "1db8228818573ca43bfa6e0242f803d1ea7ac0a43e0e1a833169dbc24a04e6c2": sha("image of XMediaFacade_default.xex\n"),
    "cdc0014cb84baaae52b5652280dbb50cbde7d7697df9c475905fbc450d3a9e57": sha("image of SpeechFacade_default.xex\n"),
}
DISC2_MAP = ("content\t20\t800\t10\n" + "".join(
    "%s/%s\t%x\t%x\t80\n" % (PKGDIR, p, 100 * (n + 1), s) for n, (p, s) in enumerate(zip(PACKAGES, PACKAGE_SIZES)))
    + "default.xex\t30\t10\t80\n")
DISC2_PINS = {
    "313d11190fc1505b251a303f368fcda11c0fa7cc5dbefddbdbf7f2b5ce278ad0": sha("fake disc2 default.xex\n"),
    "890a09685065fb010b9d1988f661b3e3a463cad40346482c36077447d0fc5859": sha("fake package 1\n"),
    "7e3508c891a3dce3b0789bf87eadd574bae60c88d9e0c1b6632b2404a99b27c2": sha("fake package 2\n"),
    "12348d67b19dca148192840cdea6258c3eba9d3350ec92b55f380c1130e80e01": sha("fake package 3\n"),
    "c8378235d78803c3dd9fb4e4f8d6c43bf28ec41595eca4bc3913098fb6f57b22": sha("fake package 4\n"),
}


def write_exec(path, text):
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, 0o755)


class Kit:
    """A scratch kit: setup.sh (pins replaced, or not), locations.sh, fake tools, a home, fake ISOs, a fake df."""

    def __init__(self, tmp, pinned=True):
        self.tmp = tmp
        self.kit = os.path.join(tmp, "kit")
        self.home = os.path.join(tmp, "home")
        os.makedirs(os.path.join(self.kit, "bin"))
        os.makedirs(os.path.join(tmp, "fakebin"))
        with open(os.path.join(ALPHA, "setup.sh")) as f:
            script = f.read()
        if pinned:
            for old, new in list(DISC1_PINS.items()) + list(DISC2_PINS.items()):
                assert script.count(old) == 1, "pin %s occurs %d times in setup.sh" % (old, script.count(old))
                script = script.replace(old, new)
            script, n = re.subn(r"DISC2_GDFX_SHA256=\S+", "DISC2_GDFX_SHA256=" + sha(DISC2_MAP), script)
            assert n == 1
        write_exec(os.path.join(self.kit, "setup.sh"), script)
        shutil.copy(os.path.join(ALPHA, "locations.sh"), self.kit)
        write_exec(os.path.join(self.kit, "bin", "extract-xiso"), FAKE_XISO)
        write_exec(os.path.join(self.kit, "bin", "gdfx"), FAKE_GDFX)
        write_exec(os.path.join(self.kit, "bin", "xexsurvey"), FAKE_XEXSURVEY)
        write_exec(os.path.join(tmp, "fakebin", "df"), FAKE_DF)
        self.isos = {}
        for kind in ("disc1", "disc2", "nopackages", "notxbox"):
            self.isos[kind] = os.path.join(tmp, "%s image.iso" % kind)
            with open(self.isos[kind], "w") as f:
                f.write(kind + "\n")

    def env(self, **extra):
        env = {"PATH": os.path.join(self.tmp, "fakebin") + ":/usr/bin:/bin", "HOME": self.tmp, "FM4_RECOMP_HOME": self.home,
               "FAKE_PIDFILE": os.path.join(self.tmp, "extractor.pid"), "LC_ALL": "C"}
        env.update(extra)
        return env

    def setup(self, *args, **extra):
        return subprocess.run([BASH, os.path.join(self.kit, "setup.sh"), *args], env=self.env(**extra),
                              capture_output=True, text=True, timeout=60)

    def conf(self):
        path = os.path.join(self.home, "locations.conf")
        if not os.path.exists(path):
            return None
        with open(path) as f:
            return dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line and not line.startswith("#"))

    def lib(self, script):
        """Runs bash with locations.sh sourced; returns (status, stdout, stderr)."""
        r = subprocess.run([BASH, "-c", 'set -euo pipefail; source "$1"; shift; ' + script, "lib",
                            os.path.join(self.kit, "locations.sh")], env=self.env(), capture_output=True, text=True, timeout=30)
        return r.returncode, r.stdout, r.stderr


class Base(unittest.TestCase):
    pinned = True

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="alpha setup test.")   # a space in every path
        self.k = Kit(self.tmp, self.pinned)
        self.ssd = os.path.join(self.tmp, "Volumes", "Extreme SSD")
        os.makedirs(self.ssd)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def assertResult(self, r, status, name):
        self.assertEqual(r.returncode, status, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip().splitlines()[-1], "result " + name, r.stdout)

    def disc1(self, *args, **extra):
        r = self.k.setup(*args, self.k.isos["disc1"], **extra)
        self.assertResult(r, 0, "ok")
        return r


class Disc1(Base):
    def test_default_location(self):
        r = self.disc1()
        data = os.path.join(self.k.home, "data")
        self.assertEqual(self.k.conf(), {"version": "1", "data_root": data, "user_data_root": os.path.join(data, "user-data")})
        self.assertTrue(os.path.exists(os.path.join(data, "setup.ok")))
        self.assertTrue(os.path.isdir(os.path.join(data, "user-data")))
        self.assertIn("progress 100 Ready", r.stdout)

    def test_chosen_locations_on_external_drive(self):
        data = os.path.join(self.ssd, "FM4 Recomp", "data")
        user = os.path.join(self.tmp, "saves = mine", "user-data")
        self.disc1("--data", data, "--user-data", user)
        self.assertEqual(self.k.conf()["data_root"], data)
        self.assertEqual(self.k.conf()["user_data_root"], user)
        self.assertTrue(os.path.exists(os.path.join(data, "files", "default.xex")))
        self.assertTrue(os.path.isdir(user))
        self.assertFalse(os.path.exists(os.path.join(self.k.home, "data", "files")))

    def test_relative_location_is_made_absolute(self):
        r = subprocess.run([BASH, os.path.join(self.k.kit, "setup.sh"), "--data", "rel/data/", self.k.isos["disc1"]],
                           env=self.k.env(), cwd=self.ssd, capture_output=True, text=True, timeout=60)
        self.assertResult(r, 0, "ok")
        self.assertEqual(self.k.conf()["data_root"], os.path.join(os.path.realpath(self.ssd), "rel", "data"))   # bash's getcwd

    def test_no_space(self):
        data = os.path.join(self.ssd, "data")
        r = self.k.setup("--data", data, self.k.isos["disc1"], FAKE_DF="%s|%d|/Volumes/Extreme SSD" % (self.ssd, 8 * GB_KB))
        self.assertResult(r, 4, "no-space")      # 8091799701 + 1073741824 = 9165541525 > 8 GiB (8589934592)
        self.assertIn("/Volumes/Extreme SSD", r.stderr)
        self.assertFalse(os.path.exists(os.path.join(data, "files")))
        self.assertIsNone(self.k.conf())

    def test_space_enough_with_margin(self):
        data = os.path.join(self.ssd, "data")
        self.disc1("--data", data, FAKE_DF="%s|%d|/Volumes/Extreme SSD" % (self.ssd, 9 * GB_KB))   # 9663676416 >= 9165541525

    def test_user_data_volume_checked_separately(self):
        data = os.path.join(self.ssd, "data")
        r = self.k.setup("--data", data, self.k.isos["disc1"],
                         FAKE_DF="%s|%d|/Volumes/Extreme SSD;%s|%d|/" % (self.ssd, 20 * GB_KB, self.tmp, 512 * 1024))
        self.assertResult(r, 4, "no-space")      # the user-data folder's volume keeps less than the 1 GiB margin

    def test_extract_failed(self):
        r = self.k.setup(self.k.isos["disc1"], FAKE_XISO="fail")
        self.assertResult(r, 5, "extract-failed")
        self.assertIsNone(self.k.conf())

    def test_cancelled(self):
        p = subprocess.Popen([BASH, os.path.join(self.k.kit, "setup.sh"), self.k.isos["disc1"]], env=self.k.env(FAKE_XISO="sleep"),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        pidfile = self.k.env()["FAKE_PIDFILE"]
        deadline = time.time() + 20
        while not os.path.exists(pidfile) and time.time() < deadline:
            time.sleep(0.1)
        with open(pidfile) as f:
            extractor = int(f.read())
        p.send_signal(signal.SIGTERM)
        out, err = p.communicate(timeout=20)
        self.assertEqual(p.returncode, 6, out + err)
        self.assertEqual(out.strip().splitlines()[-1], "result cancelled")
        time.sleep(0.2)
        with self.assertRaises(ProcessLookupError):
            os.kill(extractor, 0)
        self.assertIsNone(self.k.conf())

    def test_usage(self):
        for args in ([], ["--data"], ["--dest", "/x", self.k.isos["disc1"]], ["a.iso", "b.iso"], ["--bogus", "x", "a.iso"]):
            with self.subTest(args=args):
                self.assertResult(self.k.setup(*args), 2, "usage")

    def test_missing_iso_is_wrong_disc(self):
        self.assertResult(self.k.setup(os.path.join(self.tmp, "none.iso")), 3, "wrong-disc")


class Unpinned(Base):
    """The unmodified setup.sh: the fake files are not the build's (wrong disc); Disc 2 has no pins yet (unsupported)."""
    pinned = False

    def test_disc1_wrong_disc(self):
        r = self.k.setup(self.k.isos["disc1"])
        self.assertResult(r, 3, "wrong-disc")
        self.assertIsNone(self.k.conf())

    def test_disc2_unsupported_or_wrong(self):
        with open(os.path.join(ALPHA, "setup.sh")) as f:
            pending = "DISC2_GDFX_SHA256=PENDING" in f.read()
        data = os.path.join(self.k.home, "data")
        os.makedirs(os.path.join(data, "survey"))
        open(os.path.join(data, "setup.ok"), "w").close()
        r = self.k.setup("--disc2", self.k.isos["disc2"])
        if pending:
            self.assertResult(r, 8, "unsupported")
        else:
            self.assertResult(r, 3, "wrong-disc")
        self.assertFalse(os.path.exists(os.path.join(data, "disc2")))
        self.assertIsNone(self.k.conf())


class Disc2(Base):
    def test_before_disc1(self):
        self.assertResult(self.k.setup("--disc2", self.k.isos["disc2"]), 7, "not-set-up")

    def test_wrong_discs(self):
        self.disc1()
        for kind in ("notxbox", "nopackages", "disc1"):
            with self.subTest(kind=kind):
                r = self.k.setup("--disc2", self.k.isos[kind], FAKE_XISO="fail")
                self.assertResult(r, 3, "wrong-disc")    # refused from the map, before anything is copied
        self.assertNotIn("disc2_root", self.k.conf())

    def test_default_destination(self):
        self.disc1()
        r = self.k.setup("--disc2", self.k.isos["disc2"])
        self.assertResult(r, 0, "ok")
        data = os.path.join(self.k.home, "data")
        conf = self.k.conf()
        self.assertEqual(conf["disc2_root"], os.path.join(data, "disc2"))
        self.assertEqual(conf["disc2_map"], os.path.join(data, "survey", "disc2_gdfx.tsv"))
        self.assertEqual(conf["data_root"], data)                       # Disc 1's record kept
        with open(conf["disc2_map"]) as f:
            self.assertEqual(f.read(), DISC2_MAP)
        self.assertTrue(os.path.exists(os.path.join(conf["disc2_root"], PKGDIR, PACKAGES[3])))   # packages stay packed

    def test_external_destination_and_resume(self):
        self.disc1()
        dest = os.path.join(self.ssd, "FM4 Recomp", "disc2")
        self.assertResult(self.k.setup("--disc2", "--dest", dest, self.k.isos["disc2"]), 0, "ok")
        self.assertEqual(self.k.conf()["disc2_root"], dest)
        r = self.k.setup("--disc2", self.k.isos["disc2"], FAKE_XISO="fail")   # recorded destination, already extracted
        self.assertResult(r, 0, "ok")
        self.assertIn("earlier setup", r.stderr)

    def test_space_adds_up_on_one_volume(self):
        user = os.path.join(self.ssd, "user-data")
        self.disc1("--user-data", user)
        dest = os.path.join(self.ssd, "disc2")
        # 5 GiB free: room for the files (3169575934 + 1 GiB) but not also the installed content (+ 3004952576).
        ssd = "%s|%d|/Volumes/Extreme SSD" % (self.ssd, 5 * GB_KB)
        r = self.k.setup("--disc2", "--dest", dest, self.k.isos["disc2"], FAKE_DF=ssd)
        self.assertResult(r, 4, "no-space")
        self.assertFalse(os.path.exists(dest))
        # The same 5 GiB is enough when the user data is on another volume.
        os.remove(os.path.join(self.k.home, "locations.conf"))
        self.disc1()
        r = self.k.setup("--disc2", "--dest", dest, self.k.isos["disc2"], FAKE_DF=ssd)
        self.assertResult(r, 0, "ok")

    def test_refuses_foreign_folder(self):
        self.disc1()
        dest = os.path.join(self.ssd, "photos")
        os.makedirs(dest)
        open(os.path.join(dest, "keep.jpg"), "w").close()
        self.assertResult(self.k.setup("--disc2", "--dest", dest, self.k.isos["disc2"]), 1, "failed")
        self.assertTrue(os.path.exists(os.path.join(dest, "keep.jpg")))

    def test_corrupt_package_is_wrong_disc(self):
        self.disc1()
        dest = os.path.join(self.ssd, "disc2")
        self.assertResult(self.k.setup("--disc2", "--dest", dest, self.k.isos["disc2"]), 0, "ok")
        with open(os.path.join(dest, PKGDIR, PACKAGES[2]), "a") as f:
            f.write("x")
        self.assertResult(self.k.setup("--disc2", "--dest", dest, self.k.isos["disc2"]), 3, "wrong-disc")


class Library(Base):
    def test_set_get_round_trip(self):
        tricky = os.path.join(self.ssd, "a = b", "c")
        status, out, err = self.k.lib('loc_set "$FM4_RECOMP_HOME" data_root "%s" user_data_root /u; '
                                      'loc_set "$FM4_RECOMP_HOME" user_data_root /v; '
                                      'loc_get "$FM4_RECOMP_HOME" data_root; loc_get "$FM4_RECOMP_HOME" user_data_root' % tricky)
        self.assertEqual(status, 0, err)
        self.assertEqual(out.splitlines(), [tricky, "/v"])
        with open(os.path.join(self.k.home, "locations.conf")) as f:
            text = f.read()
        self.assertTrue(text.startswith("# FM4 Recomp storage locations"))
        self.assertEqual(text.count("user_data_root="), 1)
        self.assertIn("version=1\n", text)
        self.assertEqual([n for n in os.listdir(self.k.home) if ".tmp." in n], [])

    def test_defaults_without_file(self):
        status, out, err = self.k.lib('loc_resolve "$FM4_RECOMP_HOME"; '
                                      'printf "%s|%s|%s|%s\\n" "$LOC_DATA_ROOT" "$LOC_USER_DATA_ROOT" "$LOC_DISC2_ROOT" "$LOC_DISC2_MAP"')
        self.assertEqual(status, 0, err)
        h = self.k.home
        self.assertEqual(out.strip(), "%s/data|%s/data/user-data||" % (h, h))

    def test_refusals(self):
        os.makedirs(self.k.home)
        with open(os.path.join(self.k.home, "locations.conf"), "w") as f:
            f.write("data_root=/a\ndata_root=/b\n")
        self.assertEqual(self.k.lib('loc_get "$FM4_RECOMP_HOME" data_root')[0], 1)          # duplicate key
        self.assertEqual(self.k.lib('loc_set "$FM4_RECOMP_HOME" user_data_root rel/x')[0], 1)  # relative
        self.assertEqual(self.k.lib('loc_set "$FM4_RECOMP_HOME" user_data_root "$(printf "/a\\nb")"')[0], 1)
        self.assertEqual(self.k.lib('loc_set "$FM4_RECOMP_HOME" "Bad-Key" /x')[0], 1)
        with open(os.path.join(self.k.home, "locations.conf"), "w") as f:
            f.write("disc2_root=/d\n")
        self.assertEqual(self.k.lib('loc_resolve "$FM4_RECOMP_HOME"')[0], 1)               # disc2_root without disc2_map

    def test_abs(self):
        status, out, _ = self.k.lib('cd /tmp && loc_abs "x/y//"; loc_abs "/a/b/"; loc_abs /')
        self.assertEqual(status, 0)
        self.assertEqual(out.splitlines(), ["/tmp/x/y", "/a/b", "/"])

    def test_need_per_volume(self):
        a, b = os.path.join(self.ssd, "a"), os.path.join(self.tmp, "b")
        df = "%s|%d|/Volumes/Extreme SSD;%s|%d|/" % (self.ssd, 4 * GB_KB, self.tmp, 100 * GB_KB)
        script = 'export FAKE_DF="%s"; loc_need "%s" %d "%s" %d' % (df, a, 2 << 30, b, 50 << 30)
        self.assertEqual(self.k.lib(script)[0], 0)                                   # 2 + 1 <= 4 GiB; 50 + 1 <= 100
        script = 'export FAKE_DF="%s"; loc_need "%s" %d "%s" %d' % (df, a, 2 << 30, os.path.join(self.ssd, "c"), 2 << 30)
        status, _, err = self.k.lib(script)                                          # 2 + 2 + 1 > 4 GiB on one volume
        self.assertEqual(status, 1)
        self.assertIn("Extreme SSD", err)

    def test_game_args(self):
        self.disc1()
        h = self.k.home
        show = 'loc_game_args "$FM4_RECOMP_HOME"; printf "%s\\n" "${LOC_GAME_ARGS[@]}"'
        status, out, err = self.k.lib(show)
        self.assertEqual(status, 0, err)
        args = out.splitlines()
        self.assertEqual(args[args.index("--user-data") + 1], h + "/data/user-data")
        self.assertEqual(args[args.index("--disc") + 1], h + "/data/files")
        self.assertNotIn("--disc2", args)
        self.assertResult(self.k.setup("--disc2", "--dest", os.path.join(self.ssd, "disc2"), self.k.isos["disc2"]), 0, "ok")
        args = self.k.lib(show)[1].splitlines()
        self.assertEqual(args[args.index("--disc2") + 1], os.path.join(self.ssd, "disc2"))
        self.assertEqual(args[args.index("--disc2-map") + 1], h + "/data/survey/disc2_gdfx.tsv")
        shutil.rmtree(os.path.join(self.ssd, "disc2"))                               # the drive is disconnected
        self.assertEqual(self.k.lib(show)[0], 1)

    def test_game_args_keybindings(self):
        # The in-game Keyboard Controls writes <home>/keybindings.conf; without it the game keeps its defaults (no option).
        self.disc1()
        show = 'loc_game_args "$FM4_RECOMP_HOME"; printf "%s\\n" "${LOC_GAME_ARGS[@]}"'
        status, out, err = self.k.lib(show)
        self.assertEqual(status, 0, err)
        self.assertNotIn("--keybindings", out.splitlines())                         # CONTROL: no file, no option
        conf = os.path.join(self.k.home, "keybindings.conf")
        with open(conf, "w") as f:
            f.write("throttle = J\n")
        status, out, err = self.k.lib(show)
        self.assertEqual(status, 0, err)
        args = out.splitlines()
        self.assertEqual(args.count("--keybindings"), 1)
        self.assertEqual(args[args.index("--keybindings") + 1], conf)               # the path verbatim (a space in it)
        self.assertEqual(args[-2:], ["--keybindings", conf])
        os.remove(conf)
        self.assertNotIn("--keybindings", self.k.lib(show)[1].splitlines())        # removed (Reset to Defaults): gone again

    def test_game_args_user_data(self):
        self.disc1("--user-data", os.path.join(self.ssd, "user-data"))
        shutil.rmtree(os.path.join(self.ssd, "user-data"))                           # never recreated empty on a chosen drive
        status, _, err = self.k.lib('loc_game_args "$FM4_RECOMP_HOME"')
        self.assertEqual(status, 1)
        self.assertIn("is its drive connected", err)


class PlayDefaults(unittest.TestCase):
    """play.sh's Stack v2 settings, evaluated under /bin/bash 3.2 with a clean environment: GPU texture untile is off by default
    (the Quick Race road regression, 2026-11-13 playtest), the other settings keep their values, and an explicit value wins."""

    def settings(self, env):
        text = open(os.path.join(ALPHA, "play.sh")).read()
        start = text.index("export FM4_VARIANT_MEMO=")
        end = text.index("\n", text.index("FM4_EDRAM_RELEASE_REPLACED:-1}"))
        block = text[start:end] + "\nif [ \"$FM4_VERTEX_CACHE_MB\" = 0 ]; then unset FM4_VERTEX_CACHE_MB; fi\n"
        script = "set -euo pipefail\n" + block + "\nenv | grep '^FM4_' | sort\n"
        out = subprocess.run(["/bin/bash", "-c", script], env={"PATH": "/usr/bin:/bin", **env}, capture_output=True, text=True, check=True).stdout
        return dict(line.split("=", 1) for line in out.splitlines())

    def test_gpu_untile_off_by_default(self):
        s = self.settings({})
        self.assertEqual(s["FM4_TEXTURE_GPU_UNTILE"], "0")
        self.assertEqual((s["FM4_GUEST_MEMORY_GPU"], s["FM4_VERTEX_CACHE_MB"], s["FM4_SUBMIT_IF_IDLE"], s["FM4_ASYNC_RETIRE"]), ("1", "256", "8", "1"))
        # The four speed settings of the internal resolutions are on by default; 0 turns one off.
        self.assertEqual([s[k] for k in ("FM4_INTERNAL_SCALE_PITCH", "FM4_RESOLVE_FUSED", "FM4_PRESENT_LAZY", "FM4_EDRAM_RELEASE_REPLACED")], ["1"] * 4)
        self.assertEqual(self.settings({"FM4_RESOLVE_FUSED": "0", "FM4_SUBMIT_IF_IDLE": "32"})["FM4_RESOLVE_FUSED"], "0")  # CONTROL: the environment wins
        self.assertEqual(self.settings({"FM4_SUBMIT_IF_IDLE": "32"})["FM4_SUBMIT_IF_IDLE"], "32")
        self.assertNotEqual(s["FM4_TEXTURE_GPU_UNTILE"], "1")  # CONTROL: the old default

    def cache(self, env):
        text = open(os.path.join(ALPHA, "play.sh")).read()
        start = text.index('FM4_SHADER_CACHE="${FM4_SHADER_CACHE:-')
        end = text.index("\n", text.index("else export FM4_SHADER_CACHE; fi"))
        script = "set -euo pipefail\nDATA=/kit/data\n" + text[start:end] + "\n{ env | grep '^FM4_' || true; } | sort\n"
        out = subprocess.run(["/bin/bash", "-c", script], env={"PATH": "/usr/bin:/bin", **env}, capture_output=True, text=True, check=True).stdout
        return dict(line.split("=", 1) for line in out.splitlines())

    def test_shader_cache_on_by_default_with_opt_out(self):
        self.assertEqual(self.cache({}).get("FM4_SHADER_CACHE"), "/kit/data/shader_cache")  # on, in the data directory
        self.assertNotIn("FM4_SHADER_CACHE", self.cache({"FM4_SHADER_CACHE": "0"}))  # CONTROL: 0 turns it off
        self.assertEqual(self.cache({"FM4_SHADER_CACHE": "/elsewhere"}).get("FM4_SHADER_CACHE"), "/elsewhere")  # another path moves it

    def resolution(self, env, conf=None):
        text = open(os.path.join(ALPHA, "play.sh")).read()
        start = text.index("RES=fhd")
        end = text.index("\n", text.index('export FM4_INTERNAL_RES="$RES"'))
        with tempfile.TemporaryDirectory() as root:
            if conf is not None:
                with open(os.path.join(root, "graphics.conf"), "w", newline="") as f:
                    f.write(conf)
            script = "set -euo pipefail\nROOT=" + shlex.quote(root) + "\n" + text[start:end] + "\necho \"$FM4_INTERNAL_RES\"\n"
            done = subprocess.run(["/bin/bash", "-c", script], env={"PATH": "/usr/bin:/bin", **env}, capture_output=True, text=True, check=True)
        return done.stdout.strip(), done.stderr

    def test_rendering_resolution_from_graphics_conf(self):
        header = "# FM4 Recomp graphics settings, written by FM4 Recomp's Graphics panel. Delete this file for the defaults.\n"
        self.assertEqual(self.resolution({}), ("fhd", ""))                                         # no file: the default
        self.assertEqual(self.resolution({}, header + "internal_resolution = 720p\n"), ("720p", ""))
        self.assertEqual(self.resolution({}, header + "internal_resolution = fhd\n"), ("fhd", ""))   # the app's file (GraphicsSettings.swift)
        self.assertEqual(self.resolution({}, header + "internal_resolution = qhd\n"), ("qhd", ""))
        self.assertEqual(self.resolution({}, "internal_resolution=qhd\r\n"), ("qhd", ""))            # CRLF, no spaces
        self.assertEqual(self.resolution({}, header + "internal_resolution = 4k\n"), ("4k", ""))
        out, err = self.resolution({}, header + "internal_resolution = 8k\n")                      # CONTROL: an unknown value is never exported
        self.assertEqual(out, "fhd")
        self.assertIn("unknown resolution '8k': using fhd", err)
        self.assertEqual(self.resolution({}, header)[0], "fhd")                                   # a file with no setting
        self.assertEqual(self.resolution({"FM4_INTERNAL_RES": "720p"}, "internal_resolution = qhd\n"), ("720p", ""))  # the environment wins
        self.assertEqual(self.resolution({"FM4_INTERNAL_RES": "qhd"}), ("qhd", ""))

    def smoothing(self, env, conf=None):
        text = open(os.path.join(ALPHA, "play.sh")).read()
        start = text.index("AA=on")
        end = text.index("\n", text.index('export FM4_POST_AA="$AA"'))
        with tempfile.TemporaryDirectory() as root:
            if conf is not None:
                with open(os.path.join(root, "graphics.conf"), "w", newline="") as f:
                    f.write(conf)
            script = "set -euo pipefail\nROOT=" + shlex.quote(root) + "\n" + text[start:end] + "\necho \"$FM4_POST_AA\"\n"
            done = subprocess.run(["/bin/bash", "-c", script], env={"PATH": "/usr/bin:/bin", **env}, capture_output=True, text=True, check=True)
        return done.stdout.strip(), done.stderr

    def test_edge_smoothing_from_graphics_conf(self):
        header = "# FM4 Recomp graphics settings, written by FM4 Recomp's Graphics panel. Delete this file for the defaults.\n"
        self.assertEqual(self.smoothing({}), ("on", ""))                                                        # no file: Standard, the default
        self.assertEqual(self.smoothing({}, header + "internal_resolution = qhd\n"), ("on", ""))                # a file that does not name it: the default
        self.assertEqual(self.smoothing({}, header + "internal_resolution = qhd\nanti_aliasing = on\n"), ("on", ""))  # and on (GraphicsSettings.swift)
        self.assertEqual(self.smoothing({}, "anti_aliasing=on\r\n"), ("on", ""))                                # CRLF, no spaces
        self.assertEqual(self.smoothing({}, "anti_aliasing = off\n"), ("off", ""))
        self.assertEqual(self.smoothing({}, header + "internal_resolution = qhd\nanti_aliasing = soft\n"), ("soft", ""))    # the strengths
        self.assertEqual(self.smoothing({}, "anti_aliasing = strong\n"), ("strong", ""))
        out, err = self.smoothing({}, "anti_aliasing = standard\n")                                             # CONTROL: Standard is spelled on
        self.assertEqual(out, "on")
        self.assertIn("unknown anti_aliasing 'standard': using on", err)
        out, err = self.smoothing({}, "anti_aliasing = yes\n")                                                  # CONTROL: an unknown value is never exported
        self.assertEqual(out, "on")
        self.assertIn("unknown anti_aliasing 'yes': using on", err)
        self.assertEqual(self.smoothing({"FM4_POST_AA": "off"}, "anti_aliasing = on\n"), ("off", ""))            # the environment wins
        self.assertEqual(self.smoothing({"FM4_POST_AA": "off"}), ("off", ""))
        # the resolution is read as before from a file that also names the smoothing
        self.assertEqual(self.resolution({}, header + "internal_resolution = fhd\nanti_aliasing = on\n"), ("fhd", ""))

    def hdr(self, env, conf=None):
        text = open(os.path.join(ALPHA, "play.sh")).read()
        start = text.index("HDR=off")
        end = text.index("\n", text.index('export FM4_HDR="$HDR"'))
        with tempfile.TemporaryDirectory() as root:
            if conf is not None:
                with open(os.path.join(root, "graphics.conf"), "w", newline="") as f:
                    f.write(conf)
            script = "set -euo pipefail\nROOT=" + shlex.quote(root) + "\n" + text[start:end] + "\necho \"$FM4_HDR\"\n"
            done = subprocess.run(["/bin/bash", "-c", script], env={"PATH": "/usr/bin:/bin", **env}, capture_output=True, text=True, check=True)
        return done.stdout.strip(), done.stderr

    def test_hdr_from_graphics_conf(self):
        header = "# FM4 Recomp graphics settings, written by FM4 Recomp's Graphics panel. Delete this file for the defaults.\n"
        self.assertEqual(self.hdr({}), ("off", ""))                                                                  # no file: off, the default
        self.assertEqual(self.hdr({}, header + "internal_resolution = qhd\nanti_aliasing = on\n"), ("off", ""))      # a file that does not name it
        self.assertEqual(self.hdr({}, header + "internal_resolution = qhd\nanti_aliasing = on\nhdr = auto\n"), ("auto", ""))
        self.assertEqual(self.hdr({}, "hdr=auto\r\n"), ("auto", ""))                                                     # CRLF, no spaces
        self.assertEqual(self.hdr({}, "hdr = off\n"), ("off", ""))
        out, err = self.hdr({}, "hdr = yes\n")                                                                       # CONTROL: an unknown value is never exported
        self.assertEqual(out, "off")
        self.assertIn("unknown hdr 'yes': using off", err)
        self.assertEqual(self.hdr({"FM4_HDR": "off"}, "hdr = auto\n"), ("off", ""))                                    # the environment wins
        self.assertEqual(self.hdr({"FM4_HDR": "auto"}), ("auto", ""))
        out, err = self.hdr({}, "hdr = on\n")                                                                        # CONTROL: the preference is off or auto
        self.assertEqual(out, "off")
        self.assertIn("unknown hdr 'on': using off", err)
        # the other two settings are read as before from a file that also names hdr
        self.assertEqual(self.resolution({}, header + "internal_resolution = qhd\nanti_aliasing = soft\nhdr = auto\n"), ("qhd", ""))
        self.assertEqual(self.smoothing({}, header + "internal_resolution = qhd\nanti_aliasing = soft\nhdr = auto\n"), ("soft", ""))

    def extended(self, env, conf=None):
        text = open(os.path.join(ALPHA, "play.sh")).read()
        start = text.index('export FM4_EXTENDED_VIEW_OFFERED=')
        end = text.index("\n", text.index('export FM4_EXTENDED_VIEW_HOOK='))
        with tempfile.TemporaryDirectory() as root:
            if conf is not None:
                with open(os.path.join(root, "graphics.conf"), "w", newline="") as f:
                    f.write(conf)
            script = "set -euo pipefail\nROOT=" + shlex.quote(root) + "\n" + text[start:end] + "\necho \"$FM4_EXTENDED_VIEW $FM4_EXTENDED_VIEW_HOOK\"\n"
            done = subprocess.run(["/bin/bash", "-c", script], env={"PATH": "/usr/bin:/bin", **env}, capture_output=True, text=True, check=True)
        return done.stdout.strip(), done.stderr

    def test_extended_view_from_graphics_conf(self):
        header = "# FM4 Recomp graphics settings, written by FM4 Recomp's Graphics panel. Delete this file for the defaults.\n"
        on = {"FM4_EXTENDED_VIEW_OFFERED": "1"}
        # Not offered: off and the hook disarmed whatever the file or the environment says.
        self.assertEqual(self.extended({}), ("off 0", ""))
        self.assertEqual(self.extended({}, "extended_view = on\n"), ("off 0", ""))
        self.assertEqual(self.extended({"FM4_EXTENDED_VIEW": "on", "FM4_EXTENDED_VIEW_HOOK": "1"}, "extended_view = on\n"), ("off 0", ""))
        # Offered (FM4_EXTENDED_VIEW_OFFERED=1, a playtest): the file's value, the hook armed for a live switch.
        self.assertEqual(self.extended(on), ("off 1", ""))
        self.assertEqual(self.extended(on, header + "internal_resolution = qhd\nanti_aliasing = on\nhdr = auto\n"), ("off 1", ""))
        self.assertEqual(self.extended(on, header + "internal_resolution = qhd\nanti_aliasing = on\nhdr = auto\nextended_view = on\n"), ("on 1", ""))
        self.assertEqual(self.extended(on, "extended_view=on\r\n"), ("on 1", ""))                                      # CRLF, no spaces
        out, err = self.extended(on, "extended_view = wide\n")                                                         # CONTROL: an unknown value is never exported
        self.assertEqual(out, "off 1")
        self.assertIn("unknown extended_view 'wide': using off", err)
        self.assertEqual(self.extended({**on, "FM4_EXTENDED_VIEW": "off"}, "extended_view = on\n"), ("off 1", ""))      # the environment wins
        self.assertEqual(self.extended({**on, "FM4_EXTENDED_VIEW_HOOK": "0"}, "extended_view = on\n"), ("on 0", ""))    # the hook disarmed
        # the other settings are read as before from a file that also names it
        self.assertEqual(self.hdr({}, header + "internal_resolution = qhd\nhdr = auto\nextended_view = on\n"), ("auto", ""))
        self.assertEqual(self.resolution({}, header + "internal_resolution = qhd\nhdr = auto\nextended_view = on\n"), ("qhd", ""))

    def occlusion(self, env):
        text = open(os.path.join(ALPHA, "play.sh")).read()
        line = [l for l in text.split("\n") if l.startswith("export FM4_OCCLUSION_QUERIES=")]
        self.assertEqual(len(line), 1)
        done = subprocess.run(["/bin/bash", "-c", "set -euo pipefail\n" + line[0] + "\necho \"$FM4_OCCLUSION_QUERIES\"\n"], env={"PATH": "/usr/bin:/bin", **env},
                              capture_output=True, text=True, check=True)
        return done.stdout.strip()

    def test_occlusion_queries_default(self):
        self.assertEqual(self.occlusion({}), "1")                                # on by default
        self.assertEqual(self.occlusion({"FM4_OCCLUSION_QUERIES": "0"}), "0")    # the environment turns it off

    def audio(self, env):
        text = open(os.path.join(ALPHA, "play.sh")).read()
        start = text.index("AUDIO=audible")
        end = text.index("\n", text.index('AUDIO=mute', start))
        script = "set -euo pipefail\n" + text[start:end] + "\necho $AUDIO\n"
        return subprocess.run(["/bin/bash", "-c", script], env={"PATH": "/usr/bin:/bin", **env}, capture_output=True, text=True, check=True).stdout.strip()

    def test_settings_panel_is_wired(self):
        # Command-comma in the menus opens the Settings panel (Graphics): the game needs the panel's program (it was once dropped).
        self.assertIn('--controls-panel "$HERE/bin/fm4recomp-setup"', open(os.path.join(ALPHA, "play.sh")).read())

    def test_pause_menu_graphics_is_wired(self):
        # The Pause menu's GRAPHICS row (and KEYBOARD CONTROLS) needs the hooks' flag, and the data folder for graphics.conf, in the
        # game's environment; the file it writes is the one read above.
        text = open(os.path.join(ALPHA, "play.sh")).read()
        self.assertIn("FM4_PAUSE_KEYBOARD_CONTROLS=1", text)
        self.assertIn('export FM4_RECOMP_HOME="$ROOT"', text)
        self.assertIn('"$ROOT/graphics.conf"', text)

    def test_audio_audible_unless_muted(self):
        self.assertIn('--audio "$AUDIO"', open(os.path.join(ALPHA, "play.sh")).read())
        self.assertEqual(self.audio({}), "audible")
        self.assertEqual(self.audio({"FM4_RECOMP_AUDIO": "mute"}), "mute")
        self.assertEqual(self.audio({"FM4_RECOMP_AUDIO": "loud"}), "audible")  # CONTROL: only "mute" mutes

    def test_explicit_values_win(self):
        s = self.settings({"FM4_TEXTURE_GPU_UNTILE": "1", "FM4_VERTEX_CACHE_MB": "0"})
        self.assertEqual(s["FM4_TEXTURE_GPU_UNTILE"], "1")
        self.assertNotIn("FM4_VERTEX_CACHE_MB", s)


# A stand-in for the game in play.sh --warm-up: a binary named fm4recomp (play.sh stops it with pkill -x), recording its arguments,
# environment and user data in FAKE_ARGS, then logging (stderr = game.log) the input script's lines for FAKE_MODE=complete (title,
# each @scene list, the route's last event) and waiting for a signal.
FAKE_GAME_C = r"""
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char** argv) {
  FILE* f = fopen(getenv("FAKE_ARGS"), "w");
  const char* user = "";
  for (int i = 1; i < argc; i++) { fprintf(f, "%s\n", argv[i]); if (!strcmp(argv[i], "--user-data") && i + 1 < argc) user = argv[i + 1]; }
  char profile[4096];
  snprintf(profile, sizeof profile, "%s/content/1/00000001/ForzaProfile.xcontent", user);
  fprintf(f, "ENV FM4_INPUT_SCRIPT=%s\nENV FM4_INPUT_LIVE=%s\nPROFILE %s\n", getenv("FM4_INPUT_SCRIPT") ? getenv("FM4_INPUT_SCRIPT") : "-",
          getenv("FM4_INPUT_LIVE") ? getenv("FM4_INPUT_LIVE") : "-", access(profile, R_OK) == 0 ? "yes" : "no");
  fclose(f);
  if (!strcmp(getenv("FAKE_MODE"), "complete")) {
    fprintf(stderr, "[input] title accepted at vblank 1801: the script's @title section (2 events) runs from here\n");
    for (int k = 1; k <= 2; k++) { sleep(1); fprintf(stderr, "[input] @scene race 1 list %d of 2 (2 events) starts at vblank %d\n", k, 1000 * k); }
    fprintf(stderr, "[input] event 9 at scene vblank 500: released\n");
  }
  for (;;) pause();
}
"""

def read(path):
    with open(path) as f:
        return f.read()


ROUTE = "# test route\n0 A\n10\n@title\n0\n@scene race 1\n0 A\n10\n@scene race 1\n0\n500\n"


class WarmUp(Base):
    """play.sh --warm-up in a scratch kit with a fake game: the throwaway profile copy, the arguments (offscreen, muted, the route, no
    keyboard bindings), progress lines, the stop at the route's end (exit 0) and the stop on TERM (Skip: exit 1); the player's user
    data untouched, the throwaway removed."""

    def setUp(self):
        super().setUp()
        self.disc1()
        kit = self.k.kit
        write_exec(os.path.join(kit, "play.sh"), read(os.path.join(ALPHA, "play.sh")))
        for f in ("lib/libvulkan.1.dylib", "lib/libMoltenVK.dylib", "vulkan/icd.d/MoltenVK_icd.json"):
            os.makedirs(os.path.dirname(os.path.join(kit, f)), exist_ok=True)
            open(os.path.join(kit, f), "w").close()
        os.makedirs(os.path.join(kit, "warmup", "profile", "content", "1", "00000001"))
        open(os.path.join(kit, "warmup", "profile", "content", "1", "00000001", "ForzaProfile.xcontent"), "w").close()
        with open(os.path.join(kit, "warmup", "route.txt"), "w") as f:
            f.write(ROUTE)
        src = os.path.join(self.tmp, "fake_game.c")
        with open(src, "w") as f:
            f.write(FAKE_GAME_C)
        subprocess.run(["/usr/bin/cc", "-o", os.path.join(kit, "bin", "fm4recomp"), src], check=True)
        with open(os.path.join(self.k.home, "keybindings.conf"), "w") as f:
            f.write("throttle = J\n")
        self.player = os.path.join(self.k.home, "data", "user-data")
        with open(os.path.join(self.player, "save"), "w") as f:
            f.write("the player's save\n")
        self.args = os.path.join(self.tmp, "args.txt")

    def start(self, mode):
        if subprocess.run(["pgrep", "-x", "fm4recomp"], capture_output=True).returncode == 0:
            self.skipTest("a game named fm4recomp is running on this Mac (play.sh would refuse, and its pkill would reach it)")
        env = self.k.env(FAKE_ARGS=self.args, FAKE_MODE=mode, FM4_INPUT_LIVE="1")
        return subprocess.Popen([BASH, os.path.join(self.k.kit, "play.sh"), "--warm-up", "5"], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, start_new_session=True)

    def checkCommon(self):
        args = read(self.args).splitlines()
        self.assertEqual(args[args.index("--user-data") + 1], os.path.join(self.k.home, "data", "warm-up", "user-data"))
        self.assertEqual(args[args.index("--renderer") + 1], "present")
        self.assertEqual(args[args.index("--audio") + 1], "mute")
        self.assertNotIn("--keybindings", args)                                       # CONTROL: the player has a keybindings.conf
        self.assertNotIn("--controls-panel", args)
        self.assertIn("ENV FM4_INPUT_SCRIPT=" + os.path.join(self.k.kit, "warmup", "route.txt"), args)
        self.assertIn("ENV FM4_INPUT_LIVE=-", args)                                   # CONTROL: set in the caller's environment
        self.assertIn("PROFILE yes", args)                                            # the prepared profile's copy
        self.assertFalse(os.path.exists(os.path.join(self.k.home, "data", "warm-up")))  # the throwaway is removed
        self.assertEqual(read(os.path.join(self.player, "save")), "the player's save\n")
        self.assertEqual(os.listdir(self.player), ["save"])

    def test_complete(self):
        p = self.start("complete")
        out, _ = p.communicate(timeout=120)
        self.assertEqual(p.returncode, 0, out)
        progress = [l for l in out.splitlines() if l.startswith("progress ")]
        self.assertEqual(progress, ["progress 0 Starting the game", "progress 2 Warming up shaders: race 1 of 2",
                                    "progress 52 Warming up shaders: race 2 of 2", "progress 100 Shaders warmed up"], out)
        self.assertIn("play: warm-up complete", out)
        self.checkCommon()

    def test_skip(self):
        p = self.start("skip")
        deadline = time.time() + 60
        while not os.path.exists(self.args) and time.time() < deadline:
            time.sleep(0.2)
        time.sleep(1)
        held = subprocess.run(["pgrep", "-f", "caffeinate -i -w %d" % p.pid], capture_output=True).returncode == 0
        self.assertTrue(held, "no idle-sleep assertion for play.sh's lifetime")  # muted: nothing else keeps the Mac awake
        p.send_signal(signal.SIGTERM)                                                 # the app's Skip: TERM to play.sh only
        out, _ = p.communicate(timeout=60)
        self.assertEqual(p.returncode, 1, out)
        self.assertIn("the warm-up stopped before the route's end", out)
        self.assertNotIn("progress 100", out)
        self.assertNotEqual(subprocess.run(["pgrep", "-x", "fm4recomp"], capture_output=True).returncode, 0)  # the game stopped too
        self.checkCommon()


if __name__ == "__main__":
    unittest.main(verbosity=2)
