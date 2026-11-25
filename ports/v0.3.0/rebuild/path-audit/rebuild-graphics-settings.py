#!/usr/bin/env python3
"""Build the Mac settings fix on the exact previous normal and clean release command graphs."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

SOURCE = ["src/ui/graphics_page.cpp", "src/config/host_config.cpp", "src/ui/display_refresh_mac.mm"]
OTHER = ["CMakeLists.txt", "tests/graphics_page_test.cpp", "tools/bench/native_qhd/build_window.py"]
PLAYER = "fm4_static_recomp"
def obj(rel):
    return "CMakeFiles/fm4_static_recomp.dir/" + rel + ".o"
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path):
    return json.loads(path.read_text())
def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")
def put(cmd, flag, value):
    cmd[cmd.index(flag) + 1] = str(value)
def main():
    p = argparse.ArgumentParser(description=__doc__)
    for arg in ["project", "cache", "previous-normal", "previous-clean", "normal", "clean"]:
        p.add_argument("--" + arg, type=Path, required=True)
    p.add_argument("--menu-only", action="store_true", help="Reuse the prior display/config objects")
    a = p.parse_args()
    project, cache, old, prior, normal, clean = (getattr(a, key).resolve() for key in
        ("project", "cache", "previous_normal", "previous_clean", "normal", "clean"))
    compiled = SOURCE[:1] if a.menu_only else SOURCE
    extra = ["tests/graphics_page_test.cpp", "tools/bench/native_qhd/build_window.py"] if a.menu_only else OTHER
    if normal.exists() or clean.exists():
        p.error("outputs must be new folders")
    manifest = read(old / "build-result.json")
    for name, h in manifest["hashes"].items():
        assert digest(old / "package" / name) == h
    for name, h in manifest["host_sources"].items():
        if Path(name) not in [project / p for p in compiled + extra]:
            assert digest(Path(name)) == h, name
    for root, seed in [(normal, old), (clean, prior)]:
        root.mkdir()
        package = root / "package"
        shutil.copytree(seed/"package", package, ignore=shutil.ignore_patterns(PLAYER), symlinks=True)
        source = root / "source"
        for rel in compiled + extra:
            dest = source / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(project / rel, dest)
        commands = manifest["commands"].copy() if root == normal else read(prior/"commands.json")["compile"].copy()
        link = list((manifest["commands"] if root == normal else read(prior/"commands.json")["link"])[PLAYER])
        old_ui = commands[obj(SOURCE[0])][commands[obj(SOURCE[0])].index("-o")+1]
        base = list(commands[obj(SOURCE[0])])
        for rel in compiled:
            out = root / "objects" / obj(rel)
            out.parent.mkdir(parents=True, exist_ok=True)
            c = list(base)
            if root == clean:
                c.insert(1, f"-ffile-prefix-map={root}=FM4-build")
            for flag, path in [("-o",out),("-MT", out),("-MF",out.with_suffix(".d")),("-c",source/rel)]:
                put(c, flag, path)
            subprocess.run(c, cwd=cache, check=True)
            commands[obj(rel)] = c
            if rel == SOURCE[2]:
                link.insert(link.index(str(root/"objects"/obj(SOURCE[0])))+1, str(out))
            else:
                link[link.index(old_ui if rel == SOURCE[0] else obj(rel))] = str(out)
        if "CoreGraphics" not in link:
            link.extend(["-framework","CoreGraphics"])
        for i, arg in enumerate(link):
            if arg.endswith(".dylib") and Path(arg).parent == seed/"package":
                link[i] = str(package/Path(arg).name)
        out = package/PLAYER
        linked = out if root == normal else root/"unstripped"/PLAYER
        linked.parent.mkdir(exist_ok=True)
        put(link,"-o",linked)
        subprocess.run(link,cwd=cache,check=True)
        if root == clean:
            subprocess.run(["strip","-S","-o",str(out),str(linked)],check=True)
            subprocess.run(["codesign","--force","--sign","-",str(out)],check=True)
            assert not any(marker in out.read_bytes() for marker in [b"/Users/",b"/private/tmp/",b"/var/folders/"])
        subprocess.run(["codesign","--verify","--strict",str(out)],check=True)
        for lib in package.glob("*.dylib"):
            assert digest(lib) == digest(seed/"package"/lib.name)
        hashes={rel:digest(source/rel) for rel in compiled+extra}
        if root == normal:
            result = dict(manifest)
            result["commands"] = commands | {PLAYER: link}
            result["host_sources"] = manifest["host_sources"] | {str(project/p):h for p,h in hashes.items()}
            result["hashes"] = manifest["hashes"] | {PLAYER:digest(out)}
            result["dependencies"] = manifest["dependencies"] | {PLAYER:subprocess.check_output(["otool","-L",str(out)],text=True).splitlines()[1:]}
            result["steps"] = {key:{"status":0} for key in [*(obj(p) for p in compiled),PLAYER]}
            result["incremental_from"] = str(old/"build-result.json")
            write(root/"build-result.json",result)
            shutil.copy2(old/"overlay.json", root/"overlay.json")
        else:
            plan=read(prior/"commands.json")
            plan["compile"]=commands
            plan["link"][PLAYER]=link
            write(clean/"commands.json",plan)
            write(clean/"incremental.json",{"sources":hashes,"new_player_sha256":digest(out),"retained_libraries":{p.name:digest(p) for p in package.glob("*.dylib")}})
if __name__=="__main__":
    main()
