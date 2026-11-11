#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""The FM4 Recomp DMG: FM4 Recomp.app, an Applications shortcut and the Finder window of assets/dmg (660 x 400 points, the two
icons at 128 points over their label plates, the volume icon).

  /opt/homebrew/bin/python3 -I scripts/release/make_dmg.py <FM4 Recomp.app> <art dir (render.mjs)> <out.dmg>

Adapted from reFrozen scripts/release/make_dmg.py (GPL-3.0-or-later). The window lives in the volume's .DS_Store,
written with ds_store and mac_alias (pinned by hash in scripts/release/dmg-requirements.txt, from DMG_PYTHON, default
tools/dmg-python; never shipped) on the read-write image mounted at /Volumes/FM4 Recomp, where players' Macs mount it: the
background's alias records the volume, its mount path and the file's ID, so it must not name the build machine
(Finder stores it in the alias). The DMG is then compressed and read back: its contents, the app's
signature, the layout (window bounds, icon view and size, both icon positions, the background) and no build path in the
.DS_Store. FM4 Recomp changes: names, the volume icon, the layout read-back.
"""
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile

APP, ART, DMG = sys.argv[1:4]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DMG_PYTHON = os.environ.get("DMG_PYTHON", os.path.join(ROOT, "tools", "dmg-python"))
VOLUME = "FM4 Recomp"
MOUNT = f"/Volumes/{VOLUME}"
APP_NAME = "FM4 Recomp.app"
WINDOW, TITLE_BAR, ORIGIN = (660, 400), 32, (200, 120)
ICON_SIZE = 128
ICONS = {APP_NAME: (185, 250), "Applications": (475, 250)}
BACKGROUND = ".background/background.tiff"


def run(*cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        raise SystemExit(f"make_dmg: {' '.join(cmd)} failed: {r.stderr.strip()[:500]}")
    return r.stdout


def finder_layout(volume):
    sys.path.insert(0, DMG_PYTHON)
    import ds_store
    import mac_alias
    background = os.path.join(volume, BACKGROUND)
    (x, y), (w, h) = ORIGIN, (WINDOW[0], WINDOW[1] + TITLE_BAR)
    with ds_store.DSStore.open(os.path.join(volume, ".DS_Store"), "w+") as d:
        d["."]["vSrn"] = ("long", 1)
        d["."]["icvl"] = ("type", b"icnv")
        d["."]["vstl"] = ("type", b"icnv")
        d["."]["bwsp"] = {
            "WindowBounds": f"{{{{{x}, {y}}}, {{{w}, {h}}}}}", "ShowToolbar": False, "ShowSidebar": False,
            "ContainerShowSidebar": False, "ShowPathbar": False, "ShowStatusBar": False, "ShowTabView": False,
            "PreviewPaneVisibility": False, "SidebarWidth": 0,
        }
        d["."]["icvp"] = {
            "viewOptionsVersion": 1, "backgroundType": 2,
            "backgroundImageAlias": mac_alias.Alias.for_file(background).to_bytes(),
            "backgroundColorRed": 0.043, "backgroundColorGreen": 0.051, "backgroundColorBlue": 0.063,  # #0b0d10
            "iconSize": float(ICON_SIZE), "textSize": 13.0, "labelOnBottom": True, "arrangeBy": "none",
            "gridSpacing": 100.0, "gridOffsetX": 0.0, "gridOffsetY": 0.0, "showIconPreview": False,
            "showItemInfo": False, "scrollPositionX": 0.0, "scrollPositionY": 0.0,
        }
        d["."]["pBBk"] = mac_alias.Bookmark.for_file(background)
        for name, position in ICONS.items():
            d[name]["Iloc"] = position


def check_layout(volume):
    sys.path.insert(0, DMG_PYTHON)
    import ds_store
    import mac_alias
    with ds_store.DSStore.open(os.path.join(volume, ".DS_Store"), "r") as d:
        bwsp = d["."]["bwsp"]
        icvp = d["."]["icvp"]
        positions = {name: d[name]["Iloc"] for name in ICONS}
    x, y = ORIGIN
    assert bwsp["WindowBounds"] == f"{{{{{x}, {y}}}, {{{WINDOW[0]}, {WINDOW[1] + TITLE_BAR}}}}}", bwsp["WindowBounds"]
    assert not bwsp["ShowToolbar"] and not bwsp["ShowSidebar"] and not bwsp["ShowStatusBar"] and not bwsp["ShowPathbar"]
    assert icvp["backgroundType"] == 2 and icvp["iconSize"] == float(ICON_SIZE) and icvp["arrangeBy"] == "none"
    assert positions == ICONS, positions
    alias = mac_alias.Alias.from_bytes(icvp["backgroundImageAlias"])
    assert alias.volume.name == VOLUME, alias.volume.name
    assert alias.target.cnid == os.stat(os.path.join(volume, BACKGROUND)).st_ino, "the background alias names another file"


def main():
    if os.path.exists(MOUNT):
        raise SystemExit(f"make_dmg: {MOUNT} is in use (the layout must be written where players mount the DMG)")
    with tempfile.TemporaryDirectory() as tmp:
        stage = os.path.join(tmp, "stage")
        os.makedirs(os.path.join(stage, ".background"))
        run("ditto", APP, os.path.join(stage, APP_NAME))
        os.symlink("/Applications", os.path.join(stage, "Applications"))
        shutil.copy(os.path.join(ART, "dmg-background.tiff"), os.path.join(stage, BACKGROUND))
        shutil.copy(os.path.join(ART, "VolumeIcon.icns"), os.path.join(stage, ".VolumeIcon.icns"))
        rw = os.path.join(tmp, "rw.dmg")
        run("hdiutil", "create", "-quiet", "-volname", VOLUME, "-srcfolder", stage, "-fs", "HFS+", "-format", "UDRW", "-ov", rw)
        info = plistlib.loads(run("hdiutil", "attach", "-plist", "-nobrowse", "-noautoopen", rw).encode())
        mnt = next(e["mount-point"] for e in info["system-entities"] if e.get("mount-point"))
        try:
            if mnt != MOUNT:
                raise SystemExit(f"make_dmg: mounted at {mnt}, not {MOUNT}")
            finder_layout(mnt)
            run("SetFile", "-a", "C", mnt)  # the volume uses .VolumeIcon.icns
            run("SetFile", "-a", "V", os.path.join(mnt, ".VolumeIcon.icns"))
            shutil.rmtree(os.path.join(mnt, ".fseventsd"), ignore_errors=True)
        finally:
            run("hdiutil", "detach", "-quiet", mnt)
        if os.path.exists(DMG):
            raise SystemExit(f"make_dmg: {DMG} exists (never overwritten)")
        run("hdiutil", "convert", "-quiet", rw, "-format", "UDZO", "-imagekey", "zlib-level=9", "-o", DMG)

        # Read back where players mount it: contents, signature, layout, no build paths.
        run("hdiutil", "attach", "-quiet", "-readonly", "-nobrowse", "-noautoopen", DMG)
        try:
            top = sorted(n for n in os.listdir(MOUNT) if n not in (".fseventsd", ".Trashes"))
            want = sorted([".DS_Store", ".VolumeIcon.icns", ".background", "Applications", APP_NAME])
            assert top == want, f"DMG holds {top}, not {want}"
            assert os.readlink(os.path.join(MOUNT, "Applications")) == "/Applications"
            run("codesign", "--verify", "--deep", "--strict", os.path.join(MOUNT, APP_NAME))
            raw = open(os.path.join(MOUNT, ".DS_Store"), "rb").read()
            for p in ("/private/", "/var/folders/", "/tmp/", "/Users/"):
                assert p.encode() not in raw and p.encode("utf-16-be") not in raw, f".DS_Store names {p}"
            check_layout(MOUNT)
        finally:
            run("hdiutil", "detach", "-quiet", MOUNT)
    print(f"dmg: {DMG} ({os.path.getsize(DMG) >> 20} MB): app, Applications, Finder layout, volume icon; read back ok")


if __name__ == "__main__":
    main()
