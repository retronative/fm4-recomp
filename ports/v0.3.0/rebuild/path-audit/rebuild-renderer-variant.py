#!/usr/bin/env python3
"""Build and sign a clean GPU plug-in using the materialized SDK overlay and its checked command graph."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

GPU = "librexgpu-fh1.dylib"
def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()
def load(p):
    return json.loads(p.read_text())
def write(p, data):
    p.write_text(json.dumps(data, indent=2) + "\n")
def set_arg(cmd, flag, val):
    cmd[cmd.index(flag)+1]=str(val)
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ["cache","original-sdk-stage","sdk-stage","manifest","old-overlay","previous-clean","out"]:
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--jobs",type=int,default=3)
    a=parser.parse_args()
    if a.out.exists():parser.error("A new destination is required")
    a.out.mkdir()
    original=a.original_sdk_stage.resolve()
    source=a.sdk_stage.resolve()
    metadata=load(a.manifest)
    changes=[]
    for rel,h in metadata['sources'].items():
        if sha(original/rel)!=h:raise ValueError("Original overlay mismatch: "+rel)
        if sha(source/rel)!=h:changes.append(rel)
    previous=load(a.previous_clean/'commands.json')
    inputs=metadata['resolved_dependencies']
    objects=set()
    for rel in changes:
        if not rel.endswith(('.cpp','.h','.inc')) or rel.startswith('tests/'):continue
        for name,names in inputs.items():
            if any(s.endswith('/'+rel) for s in names):
                objects.add(name)
    # Headers added after the original Ninja dependency scan are included by these headers.
    if any(p.endswith('scaled_resolve_banks.h') for p in changes):
        objects.update(k for k,ins in inputs.items() if any(s.endswith('/vulkan/texture_cache.h') for s in ins))
    if not objects:raise ValueError("No affected translation units")
    if any('/rexgpu-fh1.dir/' not in k for k in objects):
        raise ValueError("This variant touches non-GPU objects; relink their owner separately")
    overlay=load(a.old_overlay)
    for e in overlay['roots']:
        external=Path(e['external-contents'])
        if external.is_relative_to(original):e['external-contents']=str(source/external.relative_to(original))
    vfs=a.out/'overlay.json'
    write(vfs,overlay)
    representative=previous['compile'][next(k for k in objects if k.endswith('vulkan/texture_cache.cpp.o'))]
    maps=[v for v in representative if v.startswith('-ffile-prefix-map=')]
    plan={}
    def compile(name):
        cmd=list(previous['compile'].get(name,metadata['commands'][name]))
        cmd=[v for v in cmd if not v.startswith('-ffile-prefix-map=')]
        cmd[1:1]=maps+[f'-ffile-prefix-map={source}=FM4-sdk',f'-ffile-prefix-map={a.out}=FM4-build']
        out=a.out/'objects'/name
        out.parent.mkdir(parents=True,exist_ok=True)
        for flag,val in [('-ivfsoverlay',vfs),('-o',out),('-MT',out),('-MF',out.with_suffix('.d'))]:
            set_arg(cmd,flag,val)
        run=subprocess.run(cmd,cwd=a.cache,capture_output=True,text=True)
        if run.returncode:raise RuntimeError(run.stderr)
        return (name,cmd)
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        plan=dict(pool.map(compile,sorted(objects)))
    package=a.out/'package'
    shutil.copytree(a.previous_clean/'package',package,ignore=shutil.ignore_patterns(GPU),symlinks=True)
    link=previous['link'][GPU][:]
    for k,c in plan.items():
        idx=[i for i,src in enumerate(link) if src==k or src.endswith('/'+k)]
        if len(idx)!=1:raise ValueError(k)
        link[idx[0]]=c[c.index('-o')+1]
    for i,item in enumerate(link):
        if item.endswith('/librexruntime.dylib'):link[i]=str(package/'librexruntime.dylib')
    linked=a.out/'unstripped'/GPU
    linked.parent.mkdir()
    set_arg(link,'-o',linked)
    subprocess.run(link,cwd=a.cache,check=True)
    final=package/GPU
    subprocess.run(['strip','-S','-o',final,linked],check=True)
    subprocess.run(['codesign','--force','--sign','-',final],check=True)
    subprocess.run(['codesign','--verify','--strict',final],check=True)
    if any(v in final.read_bytes() for v in (b'/Users/', b'/private/tmp/', b'/var/folders/')):
        raise ValueError("Private path bytes in clean renderer")
    previous['compile'].update(plan); previous['link'][GPU]=link
    write(a.out/'commands.json',previous)
    write(a.out/'incremental.json',{'changed_sources':{f:sha(source/f) for f in changes},
        'rebuilt_objects':sorted(plan),'engine_sha256':{p.name:sha(p) for p in package.iterdir() if p.name=='fm4_static_recomp' or p.suffix=='.dylib'}})
    print(f'{len(plan)} rebuilt GPU objects, signed renderer {sha(final)}')
if __name__=='__main__':main()
