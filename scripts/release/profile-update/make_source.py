#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the 0.3.1 source companion from the public release tree and verified engine sources."""
import argparse
import hashlib
import io
import json
import lzma
import os
from pathlib import Path
import tarfile
import tempfile

STEM = "FM4-Recomp-0.3.1-source"
RECIPE = "scripts/release/profile-update"
ROOT = Path(__file__).resolve().parents[3]
# Every input is a public source file. A workspace scan would also pick up build output.
FILES = """
COPYING
NOTICE
THIRD_PARTY_NOTICES.md
app/setup/ControlsPanel.swift
app/setup/ControlsView.swift
app/setup/Core.swift
app/setup/GraphicsSettings.swift
app/setup/GraphicsView.swift
app/setup/KeyBindings.swift
app/setup/LaunchDecision.swift
app/setup/Main.swift
app/setup/Panel.swift
app/setup/ProfileSetup.swift
app/setup/SetupView.swift
assets/dmg/dmg-background.html
assets/dmg/render.mjs
assets/fm4-recomp-logo.svg
assets/icon/app-icon.html
assets/icon/volume-icon.html
scripts/alpha/locations.sh
scripts/alpha/migrate-career.sh
scripts/alpha/migrate-career.swift
scripts/alpha/migrate-playtest-profile.sh
scripts/alpha/play.sh
scripts/alpha/setup.sh
scripts/alpha/shiftglue-playtest.sh
scripts/preflight.sh
scripts/release/dmg-requirements.txt
scripts/release/make_dmg.py
scripts/release/profile-update/kit-readme.md
scripts/release/profile-update/make_app.sh
scripts/release/profile-update/make_source.py
scripts/release/profile-update/release-inputs.json
scripts/release/profile-update/source-guide.md
tests/app/test_graphics_settings.swift
tests/app/test_keybindings.swift
tests/app/test_launch_decision.swift
tests/app/test_profile_setup.swift
tests/run_app_tests.sh
tests/test_migrate_career.py
tests/test_migrate_playtest_profile.py
tests/test_production_profile.sh
""".split()


def sha256(stream):
    digest = hashlib.sha256()
    for part in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(part)
    return digest.hexdigest()


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("engine_source", type=Path, help="published 0.3.0 source companion")
    cli.add_argument("output_dir", type=Path, help="directory for the archive and checksum")
    args = cli.parse_args()
    sources = {name: ROOT / name for name in FILES}
    if {p.name for p in (ROOT / "app/setup").glob("*.swift")} != {
        p.name for n, p in sources.items() if n.startswith("app/setup/")
    }:
        cli.error("the release source list must cover every Swift input to make_app.sh")
    release = json.loads((ROOT / RECIPE / "release-inputs.json").read_text())
    baseline = release["engine_and_decoder_source"]
    baseline_name = "engine-source/" + baseline["filename"]
    sources[baseline_name] = args.engine_source
    sources["README.md"] = ROOT / RECIPE / "source-guide.md"
    hashes = {}
    for name, path in sorted(sources.items()):
        if path.is_symlink() or not path.is_file():
            cli.error(f"missing or linked source: {name}")
        with path.open("rb") as stream:
            hashes[name] = sha256(stream)
    if hashes[baseline_name] != baseline["sha256"]:
        cli.error("the engine source archive does not match the published 0.3.0 hash")
    sums = "".join(f"{value}  {name}\n" for name, value in sorted(hashes.items())).encode()
    hashes["SHA256SUMS"] = hashlib.sha256(sums).hexdigest()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    target = args.output_dir / (STEM + ".tar.xz")
    checksum = args.output_dir / (target.name + ".sha256")
    if os.path.lexists(target) or os.path.lexists(checksum):
        cli.error("the destination already contains release outputs")

    with tempfile.TemporaryDirectory(dir=args.output_dir) as scratch:
        tmp = Path(scratch) / target.name
        with lzma.open(tmp, "wb", preset=0) as xz:
            with tarfile.open(fileobj=xz, mode="w|", format=tarfile.USTAR_FORMAT) as tar:
                for name in sorted(hashes):
                    item = tarfile.TarInfo(STEM + "/" + name)
                    item.mode = 0o755 if name.endswith(".sh") else 0o644
                    item.size = len(sums) if name == "SHA256SUMS" else sources[name].stat().st_size
                    with (io.BytesIO(sums) if name == "SHA256SUMS" else sources[name].open("rb")) as stream:
                        tar.addfile(item, stream)

        # Read back every byte and reject duplicates and any unplanned entries.
        expected = {STEM + "/" + name: value for name, value in hashes.items()}
        with tarfile.open(tmp, "r|xz") as tar:
            for member in tar:
                if not member.isfile() or member.name not in expected:
                    raise RuntimeError(f"unexpected archive entry: {member.name}")
                with tar.extractfile(member) as stream:
                    if sha256(stream) != expected.pop(member.name):
                        raise RuntimeError(f"source changed during packaging: {member.name}")
        if expected:
            raise RuntimeError(f"archive missing {len(expected)} files")
        with tmp.open("rb") as stream:
            line = f"{sha256(stream)}  {target.name}\n"
        checksum_tmp = Path(scratch) / checksum.name
        checksum_tmp.write_text(line)
        os.link(tmp, target)
        os.link(checksum_tmp, checksum)
        print(line, end="")


if __name__ == "__main__":
    main()
