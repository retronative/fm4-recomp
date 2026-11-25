#!/usr/bin/env python3
"""Plan or run an isolated, targeted public build from the measured combined manifest."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

def run(command, cwd):
    subprocess.run(command, cwd=cwd, check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["cache", "sdk", "project", "build-result", "inventory", "reference-package", "output"]:
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--reuse-stage", type=Path, help="Completed clean build for a previous candidate")
    parser.add_argument("--prior-build-result", type=Path, help="Original manifest for the previous clean build")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--expect-rebuilds", type=int, help="Fail if the reuse plan differs from the measured scope")
    args = parser.parse_args()
    cache = args.cache.resolve(strict=True)
    build = args.build_result.resolve(strict=True).parent
    reference = args.reference_package.resolve(strict=True)
    out = args.output.resolve()
    if bool(args.reuse_stage) != bool(args.prior_build_result):
        raise ValueError("Supply both previous build locations together")
    if out.exists() or any(p == out or p in out.parents for p in (cache, build, reference)):
        raise ValueError("Choose a new staging directory outside the build/cache/reference")
    out.mkdir(parents=True)
    package = out / "package"
    raw = out / "unstripped"
    package.mkdir()
    raw.mkdir()
    commands = json.loads(args.build_result.read_text())["commands"]
    inventory = json.loads(args.inventory.read_text())
    host = cache.parents[2]  # <host>/out/build/<preset>
    maps = {
        str(host): "FM4-Recomp", str(args.sdk.resolve(strict=True)): "ShiftGlue",
        str(args.project.resolve(strict=True)): "FM4-Recomp", str(build): "FM4-build",
    }
    flags = [f"-ffile-prefix-map={source}={label}" for source, label in maps.items()]
    flags.append("-fdebug-compilation-dir=.")
    compile_commands = {}
    link_commands = {}
    replacement_objects = {}
    libs = {"liblibavcodec.a": "libavcodec.58.dylib", "liblibavutil.a": "libavutil.56.dylib",
            "libmspack.a": "libshiftglue_lzx.dylib"}
    outputs = {Path(name).name for name in inventory}
    for target, command in commands.items():
        name = Path(target).name
        if name not in outputs:
            continue
        entry = inventory[name]
        if entry["unassigned_strings"] or any(key not in ("guest", "host/SDK") for key in entry["affected_inputs"]):
            raise ValueError(f"Unaccounted path or new archive in {name}")
        for group in entry["affected_inputs"].values():
            for ob in group:
                t = ob["object"]
                if Path(t).is_absolute() or ".." in Path(t).parts:
                    raise ValueError(f"Unsafe relative target {t}")
                if t not in compile_commands:
                    args0 = list(commands[t]) if t in commands else shlex.split(subprocess.check_output(
                        ["ninja", "-t", "commands", "-s", t], cwd=cache, text=True))
                    if not Path(args0[0]).name.startswith("clang") or "-c" not in args0 or any(s in (";", "&&", "|", ">") for s in args0):
                        raise ValueError(f"Unexpected compiler recipe {t}")
                    old = args0[args0.index("-o") + 1]
                    obj = out / "objects" / t
                    obj.parent.mkdir(parents=True, exist_ok=True)
                    for key, value in [("-o", obj), ("-MT", obj), ("-MF", obj.with_suffix(".d"))]:
                        args0[args0.index(key) + 1] = str(value)
                    args0[1:1] = flags
                    replacement_objects[old] = str(obj)
                    compile_commands[t] = args0
        link = list(command)
        for i, item in enumerate(link):
            if item in replacement_objects:
                link[i] = replacement_objects[item]
            elif Path(item).is_absolute() and Path(item).name in outputs:
                link[i] = str(package / Path(item).name)
            elif Path(item).name in libs:
                link[i] = str(package / libs[Path(item).name])
        link[link.index("-o")+1] = str(raw / name)
        if name == "librexruntime.dylib":
            link.append("-Wl,-rpath,@loader_path")
        link_commands[name] = link
    if set(link_commands) != outputs:
        raise ValueError("Unrecognized measured build")
    (out / "commands.json").write_text(json.dumps({"compile": compile_commands,"link":link_commands},indent=2) + "\n")
    print(f"Prepared {len(compile_commands)} compile and {len(link_commands)} link commands.")
    if not args.execute:
        return
    for lib in libs.values():
        shutil.copy2(reference / lib, package / lib)
    rebuild = dict(compile_commands)
    reused = []
    if args.reuse_stage:
        prior = args.reuse_stage.resolve(strict=True)
        prior_build = args.prior_build_result.resolve(strict=True).parent
        prior_commands = json.loads(args.prior_build_result.read_text())["commands"]
        prior_plan = json.loads((prior / "commands.json").read_text())["compile"]
        # Comparing original object bytes incorporates all compiled header/VFS
        # differences, including changes outside the top-level source file.
        def raw_path(src_commands, target):
            command = src_commands.get(target)
            return cache / (command[command.index("-o")+1] if command else target)
        def normalized(command, stage, measured):
            return [s.replace(str(stage), "<stage>").replace(str(measured), "<measured>") for s in command]
        for t, command in compile_commands.items():
            source_obj = prior / "objects" / t
            if not source_obj.is_file() or t not in prior_plan:
                continue
            if normalized(command, out, build) != normalized(prior_plan[t], prior, prior_build):
                continue
            before = hashlib.sha256(raw_path(prior_commands, t).read_bytes()).digest()
            after = hashlib.sha256(raw_path(commands, t).read_bytes()).digest()
            if before == after:
                os.link(source_obj, out / "objects" / t)
                reused.append(t)
                del rebuild[t]
    (out / "reuse.json").write_text(json.dumps({"reused": reused, "recompiled": list(rebuild)}, indent=2) + "\n")
    if args.expect_rebuilds is not None and len(rebuild) != args.expect_rebuilds:
        raise ValueError(f"Unexpected rebuild count {len(rebuild)}; see reuse.json")
    print(f"{len(reused)} previously verified objects reused; {len(rebuild)} objects to compile.", flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        list(pool.map(lambda command: run(command, cache), rebuild.values()))
    for name, cmd in link_commands.items():
        run(cmd, cache)
        path = package / name
        run(["strip", "-S", "-o", str(path), str(raw / name)], cache)
        data = path.read_bytes()
        if any(prefix in data for prefix in (b"/Users/", b"/private/tmp/", b"/var/folders/")):
            raise ValueError(f"Private build-path text remains in {name}")
        for opts in (["-gUj"], ["-uj"]):
            old = set(subprocess.check_output(["nm", *opts, str(reference / name)]).splitlines())
            new = set(subprocess.check_output(["nm", *opts, str(path)]).splitlines())
            if old != new:
                raise ValueError(f"ABI change in {name}: {len(old-new)} missing, {len(new-old)} added")
        run(["codesign", "--force", "--sign", "-", str(path)], cache)
        run(["codesign", "--verify", "--strict", str(path)], cache)

if __name__ == "__main__":
    main()
