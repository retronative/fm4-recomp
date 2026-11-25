#!/usr/bin/env python3
"""Recompile only the graphics page, replace the player binary, preserve signed engine dependencies."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

TARGET = "CMakeFiles/fm4_static_recomp.dir/src/ui/graphics_page.cpp.o"
EXE = "fm4_static_recomp"
SRC = Path("src/ui/graphics_page.cpp")

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def write(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")

def run(argv, cwd):
    subprocess.run(argv, cwd=cwd, check=True)

def flag(args, key, value):
    args[args.index(key) + 1] = str(value)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["cache", "project", "previous-normal", "previous-clean", "normal-out", "clean-out"]:
        parser.add_argument("--"+name, type=Path, required=True)
    a=parser.parse_args()
    cache,project,old,prior = (p.resolve(strict=True) for p in (a.cache,a.project,a.previous_normal,a.previous_clean))
    new,clean = a.normal_out.resolve(),a.clean_out.resolve()
    if new.exists() or clean.exists() or new==clean:
        parser.error("Both outputs must be new folders")
    prev = json.loads((old/"build-result.json").read_text())
    original_source = project/SRC
    changed = {p for p,h in prev["host_sources"].items() if digest(Path(p))!=h}
    if changed != {str(original_source)}:
        raise ValueError(f"Unexpected host-source changes: {changed}")
    for p,h in prev["sources"].items():
        if digest(old/"sdk-overlay"/p)!=h:
            raise ValueError(f"Previous staged SDK source changed: {p}")
    for n,h in prev["hashes"].items():
        if digest(old/"package"/n)!=h:
            raise ValueError(f"Previous stage has changed: {n}")
    new.mkdir()
    clean.mkdir()
    (new/"source").mkdir()
    snapshot=new/"source"/SRC.name
    shutil.copy2(original_source,snapshot)
    overlay = json.loads((old/"overlay.json").read_text())
    named = [e for e in overlay["roots"] if Path(e["external-contents"]) == original_source]
    if len(named)!=1:
        raise ValueError("Unexpected host source overlay")
    named[0]["external-contents"] = str(snapshot)
    write(new/"overlay.json",overlay)
    clean_plan = json.loads((prior/"commands.json").read_text())
    output_products={}
    for root,comp,link,seed in [
            (new,prev["commands"][TARGET],prev["commands"][EXE],old),
            (clean,clean_plan["compile"][TARGET],clean_plan["link"][EXE],prior)]:
        comp = list(comp)
        link = list(link)
        obj = root/"objects"/TARGET
        pkg = root/"package"
        obj.parent.mkdir(parents=True)
        pkg.mkdir()
        for item in (seed/"package").iterdir():
            if item.name == EXE:
                continue
            if item.is_dir():
                shutil.copytree(item,pkg/item.name,symlinks=True)
            else:
                shutil.copy2(item,pkg/item.name)
        oldobj = comp[comp.index("-o")+1]
        flag(comp,"-ivfsoverlay",new/"overlay.json")
        for opt,path in [("-o",obj),("-MF",obj.with_suffix(".d")),("-MT",obj)]:
            flag(comp,opt,path)
        if root==clean:
            comp.insert(1, f"-ffile-prefix-map={new}=FM4-build")
        if link.count(oldobj)!=1:
            raise ValueError("Unexpected object in executable link")
        link[link.index(oldobj)] = str(obj)
        for i,v in enumerate(link):
            if Path(v).is_absolute() and v.endswith(".dylib") and Path(v).parent==seed/"package":
                link[i]=str(pkg/Path(v).name)
        exe=pkg/EXE
        linkout=exe
        if root==clean:
            linkout=root/"unstripped"/EXE
            linkout.parent.mkdir()
        flag(link,"-o",linkout)
        started=time.monotonic()
        run(comp,cache)
        ct=round(time.monotonic()-started,3)
        started=time.monotonic()
        run(link,cache)
        if root==clean:
            run(["strip","-S","-o",str(exe),str(linkout)],cache)
            run(["codesign","--force","--sign","-",str(exe)],cache)
            for name in [k for k in (seed/"package").iterdir() if k.suffix==".dylib"]:
                if digest(pkg/name.name)!=digest(name):
                    raise ValueError(f"Unchanged dependency was altered: {name.name}")
            if any(x in exe.read_bytes() for x in (b"/Users/",b"/private/tmp/",b"/var/folders/")):
                raise ValueError("Private build path remains in executable")
        run(["codesign","--verify","--strict",str(exe)],cache)
        output_products[root] = [comp,link,ct,round(time.monotonic()-started,3)]
    comp,link,ct,lt = output_products[new]
    result=copy.deepcopy(prev)
    result["commands"][TARGET]=comp
    result["commands"][EXE]=link
    result["host_sources"][str(original_source)]=digest(snapshot)
    result["hashes"][EXE]=digest(new/"package"/EXE)
    result["dependencies"][EXE]=subprocess.check_output(["otool","-L",str(new/"package"/EXE)],text=True).splitlines()[1:]
    result["steps"]={TARGET:{"status":0,"seconds":ct},EXE:{"status":0,"seconds":lt}}
    result["incremental_from"]=str(old/"build-result.json")
    write(new/"build-result.json",result)
    comp,link,ct,lt=output_products[clean]
    clean_plan["compile"][TARGET]=comp
    clean_plan["link"][EXE]=link
    write(clean/"commands.json",clean_plan)
    write(clean/"incremental.json",{"source":str(SRC),"content_sha256":digest(snapshot),
          "compile_seconds":ct,"link_and_sign_seconds":lt,"new_player_sha256":digest(clean/"package"/EXE),
          "retained_libraries":{p.name:digest(p) for p in sorted((clean/"package").glob("*.dylib"))}})
    print("Rebuilt the graphics page and player. Other code images remain byte-identical.")

if __name__=="__main__":
    main()
