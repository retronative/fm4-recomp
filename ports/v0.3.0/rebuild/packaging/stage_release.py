#!/usr/bin/env python3
"""Stage and verify a local performance RC app from the six original kit inputs and the release libraries."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HOST = "fm4_static_recomp"
RUNTIME = "librexruntime.dylib"
GPU = "librexgpu-fh1.dylib"
FACADES = [f"lib{HOST}_{name}Facade_default.dylib" for name in ("Speech", "XMedia")]
CODEC, UTIL, LZX = "libavcodec.58.dylib", "libavutil.56.dylib", "libshiftglue_lzx.dylib"
DECODERS = (CODEC, UTIL, LZX)
ENGINE_FILES = (HOST, RUNTIME, GPU, *FACADES)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_packaged_files(app, engine):
    allowed_bin = {"extract-xiso", "fm4_validation_control", "gdfx", "xexsurvey"}
    allowed_lib = {"libMoltenVK.dylib", "libVkLayer_khronos_validation.dylib", "libfm4_lzx.dylib", "libvulkan.1.dylib"}
    kit = engine.parents[1]
    for folder, names in ((kit / "bin", allowed_bin), (kit / "lib", allowed_lib),
                          (kit / "engines", {"shiftglue"}),
                          (engine, {*ENGINE_FILES, *DECODERS, "graphics-settings.json", "licenses", "upscalers", "vulkan"})):
        require({p.name for p in folder.iterdir()} == names, f"Unexpected contents of {folder.name}")
    forbidden = {".xex", ".iso", ".xma", ".fxobj", ".sav", ".log", ".trace"}
    local_home = str(Path.home()).encode() + b"/"
    for path in app.rglob("*"):
        require(not path.is_symlink(), f"Unexpected symlink: {path.name}")
        if not path.is_file():
            continue
        require(path.suffix.lower() not in forbidden and path.name not in {
            ".DS_Store", "GPU_STOP", "locations.conf", "graphics.conf", "image.bin", "default.xexp"},
            f"Personal or game data entered the kit: {path.name}")
        data = path.read_bytes()
        require(not any(marker in data for marker in (b"/.codex/", local_home, b"/private/tmp/fm4-")),
                f"Developer path in {path.relative_to(app)}")
        if data.startswith(b"\xcf\xfa\xed\xfe") or data.startswith(b"\xca\xfe\xba\xbe"):
            require(b"/Users/" not in data and b"/private/tmp/" not in data,
                    f"Private path in executable {path.name}")
            subprocess.run(["codesign", "--verify", "--strict", path], check=True)


def verify_links(kit, engine):
    expected = {
        engine / HOST: {GPU, RUNTIME}, engine / GPU: {RUNTIME}, engine / RUNTIME: set(DECODERS),
        engine / CODEC: {UTIL}, engine / UTIL: set(), engine / LZX: set(),
        kit / "bin/xexsurvey": {"libfm4_lzx.dylib"}, kit / "lib/libfm4_lzx.dylib": set(),
    }
    expected.update({engine / f: {RUNTIME} for f in FACADES})
    for path, required in expected.items():
        lines = subprocess.check_output(["otool", "-L", path], text=True).splitlines()[1:]
        names = {line.lstrip().split(" (compatibility version", 1)[0] for line in lines}
        own = "@rpath/" + path.name
        if path.suffix == ".dylib":
            install_id = subprocess.check_output(["otool", "-D", path], text=True).splitlines()[1]
            require(install_id == own, f"Unexpected install name for {path.name}")
        names.discard(own)
        deps = {name for name in names if not name.startswith(("/System/Library/", "/usr/lib/"))}
        require(deps == {"@rpath/" + name for name in required},
                f"Unexpected dependencies in {path.name}: {sorted(deps)}")
    for lib, marker in ((engine / CODEC, "_avcodec_send_packet"), (engine / UTIL, "_av_frame_alloc"),
                        (engine / LZX, "_lzxd_decompress"), (kit / "lib/libfm4_lzx.dylib", "_lzxd_decompress")):
        symbols = subprocess.check_output(["nm", "-gU", lib], text=True).split()
        require(marker in symbols, f"Missing decoder export in {lib.name}")
    for program in (engine / RUNTIME, kit / "bin/xexsurvey"):
        symbols = subprocess.check_output(["nm", "-gU", program], text=True).splitlines()
        embedded = any(re.fullmatch(r"_(?:av_|avcodec_|avutil_|ff_|lzxd_).*", s.split()[-1])
                       for s in symbols if s.split())
        require(not embedded, f"Embedded decoder symbols in {program.name}")
    runtime_commands = subprocess.check_output(["otool", "-l", engine / RUNTIME], text=True)
    require("path @loader_path (" in runtime_commands, "Runtime cannot find its local replacement libraries")


def regenerate_schema(engine, temp):
    # Schema mode returns before graphics initialization and needs no game assets.
    schema = temp / "graphics-settings.json"
    env = {"PATH": os.defpath, "HOME": str(temp), "TMPDIR": str(temp), "FM4_COMPARE_OFFSCREEN": "1",
           "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy"}
    subprocess.run([engine / HOST, "--headless=true", "--audio_mute=true", f"--game_data_root={temp}",
                    f"--user_data_root={temp}", f"--log_file={temp / 'schema.log'}",
                    f"--fm4_write_graphics_schema={schema}"], cwd=temp, env=env, check=True, timeout=30)
    data = json.loads(schema.read_text())
    rows = {row.get("key"): row for block in data["blocks"] for row in block["rows"]}
    require(data["schema_version"] == 1 and "fm4_extended_view" in rows and "resolution_scale" in rows,
            "Candidate schema is missing graphics settings")
    options = [(entry["value"], entry["label"]) for entry in rows["resolution_scale"]["choices"]]
    require(options == [("1", "720p"), ("5", "1080p (Full HD)"), ("2", "1440p (QHD)"), ("3", "4K (2160p)")],
            f"Resolution labels or saved values differ: {options}")
    shutil.copyfile(schema, engine / "graphics-settings.json")


def stage(args):
    dest = args.output.absolute()
    require(not dest.exists(), "Choose a new output directory")
    require(re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", args.version), "Expected a numeric macOS version")
    require(re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", args.build_number), "Expected numeric build components")
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=dest.parent, prefix=".fm4-release-") as work:
        temp = Path(work)
        stage_dir = temp / "staged"
        subprocess.run(["/bin/bash", HERE / "stage_playtest.sh", args.project.resolve(), args.base_app.resolve(),
                        args.engine.resolve(), args.host.resolve(), args.sdk.resolve(), stage_dir], check=True)
        (stage_dir / "Play QHD Performance.command").rename(stage_dir / "Play FM4 Recomp.command")
        app = stage_dir / "FM4 Recomp.app"
        resources = app / "Contents/Resources"
        kit = resources / "kit"
        engine = kit / "engines/shiftglue"
        for name in ENGINE_FILES:
            shutil.copy2(args.engine_binaries / name, engine / name)
        for name in DECODERS:
            shutil.copy2(args.libraries / name, engine / name)
        verify_links(kit, engine)
        regenerate_schema(engine, temp)
        shutil.copyfile(args.readme, kit / "README.md")
        for folder in (resources / "licenses", kit / "licenses"):
            shutil.copyfile(args.notice, folder / "THIRD_PARTY_NOTICES.md")
            dest_licenses = folder / "shiftglue"
            if dest_licenses.exists():
                shutil.rmtree(dest_licenses)
            shutil.copytree(args.licenses, dest_licenses)
            notice = (folder / "THIRD_PARTY_NOTICES.md").read_text()
            for name in (*DECODERS, "libfm4_lzx.dylib"):
                require(name in notice, f"Missing decoder attribution: {name}")
            for link in re.findall(r'\]\(([^)]+)\)', notice):
                if "://" not in link:
                    require((folder / link).is_file(), f"Missing licence reference: {link}")
        plist_file = app / "Contents/Info.plist"
        with plist_file.open("rb") as stream:
            info = plistlib.load(stream)
        info.update(CFBundleShortVersionString=args.version, CFBundleVersion=args.build_number)
        with plist_file.open("wb") as stream:
            plistlib.dump(info, stream, sort_keys=False)
        # Engine images and helpers arrive signed. Seal the app around those verified nested files.
        subprocess.run(["codesign", "--force", "--sign", "-", app], check=True)
        check_packaged_files(app, engine)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", app], check=True)
        subprocess.run([app / "Contents/MacOS/FM4Recomp", "--setup", "prerequisites"],
                       env={**os.environ, "HOME": str(temp)}, check=True)
        require(not dest.exists(), "Output directory appeared during staging")
        stage_dir.rename(dest)
    print(dest / "FM4 Recomp.app", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for opt, help_text in (
        ("project", "Prepared FM4 macOS wrapper source"),
        ("base-app", "Existing verified macOS app supplying setup and validation tools"),
        ("engine", "Full engine build including Vulkan and FidelityFX support"),
        ("host", "FM4 Static Recomp source checkout"), ("sdk", "ShiftGlue SDK checkout"),
        ("engine-binaries", "Final clean game binary, runtime, GPU library and two facades"),
        ("libraries", "Three replaceable SDK decoder libraries"),
        ("notice", "Matching Mac THIRD_PARTY_NOTICES.md"), ("licenses", "ShiftGlue attribution directory"),
        ("output", "New dedicated output directory")):
        parser.add_argument("--" + opt, type=Path, required=True, help=help_text)
    parser.add_argument("--readme", type=Path, default=HERE / "release-readme.md")
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-number", required=True)
    stage(parser.parse_args())
