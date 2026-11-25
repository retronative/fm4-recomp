#!/usr/bin/env python3
"""Rebuild the two changed title images from a measured plan and a known signed kit."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

GPU = 'librexgpu-fh1.dylib'
PLAYER = 'fm4_static_recomp'
CLEAN_NAMES = (GPU, PLAYER)

def run(args, cwd):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr + result.stdout)
    return result.stdout

def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def json_in(path):
    return json.loads(path.read_text())

def record(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('normal', 'source', 'sdk', 'cache', 'reference', 'release-receipt', 'inventory', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=3)
    parser.add_argument('--reuse-compiled', action='store_true', help='Resume only an interrupted link of the same materialized source and object set')
    a = parser.parse_args()
    out = a.output.resolve()
    out.mkdir(exist_ok=a.reuse_compiled)
    package = out/'package'
    package.mkdir(exist_ok=a.reuse_compiled)
    clean_reference = a.reference.resolve(strict=True)
    previous = json_in(a.release_receipt)
    inventory = json_in(a.inventory)
    for name, info in previous.items():
        if name in CLEAN_NAMES:
            continue
        if digest(clean_reference/name) != info['candidate_sha256']:
            raise ValueError('Input differs from signed package: '+name)
        shutil.copy2(clean_reference/name, package/name)
    normal = a.normal.resolve(strict=True)
    commands = json_in(normal/'build-plan.json')['commands']
    mapped = {c[c.index('-o')+1]: k for k,c in commands.items() if k.endswith('.o')}
    maps = [(normal, 'FM4-build'), (a.cache.resolve().parents[2], 'FM4-Recomp'),(a.source.resolve(),'FM4-Recomp'),(a.sdk.resolve(),'ShiftGlue')]
    flags = [f'-ffile-prefix-map={k}={v}' for k,v in maps] + ['-fdebug-compilation-dir=.']
    compiles = {}
    links = {}
    for target, command in commands.items():
        owner = Path(target).name
        if owner not in CLEAN_NAMES:
            continue
        affected = {ob['object'] for group in inventory[owner]['affected_inputs'].values() for ob in group}
        link = command[:]
        for i, token in enumerate(link):
            if token.endswith('.o'):
                key = mapped.get(token, token)
                if key not in affected and key not in commands:
                    continue
                if Path(key).is_absolute() or '..' in Path(key).parts:
                    raise ValueError('Invalid build object: '+key)
                obj = out/'objects'/key
                if key not in compiles:
                    c = (commands[key][:] if key in commands else shlex.split(run(['ninja','-t','commands','-s',key],a.cache)))
                    if len([s for s in c if s=='-c'])!=1: raise ValueError(key)
                    obj.parent.mkdir(parents=True,exist_ok=True)
                    c[1:1]=flags
                    for flag, value in [('-o',obj),('-MT',obj),('-MF',obj.with_suffix('.d'))]:
                        c[c.index(flag)+1]=str(value)
                    compiles[key]=c
                link[i]=str(obj)
            elif Path(token).is_absolute() and token.endswith(('/librexruntime.dylib', '/librexgpu-fh1.dylib')):
                link[i]=str(package/Path(token).name)
        linked = out/'unstripped'/owner
        linked.parent.mkdir(exist_ok=True)
        link[link.index('-o')+1]=str(linked)
        links[owner]=link
    if set(links) != set(CLEAN_NAMES): raise ValueError('Incomplete link plan')
    if a.reuse_compiled:
        old = json_in(out/'commands.json')
        if old['compile']!=compiles or any(not Path(cmd[cmd.index('-o')+1]).is_file() for cmd in compiles.values()):
            raise ValueError('Cannot reuse incomplete or different compilation inputs')
    record(out/'commands.json', {'compile':compiles,'link':links})
    print(f'{len(compiles)} object files to compile including all modified host and renderer inputs',flush=True)
    def compile_one(item):
        label, cmd = item
        try: run(cmd, a.cache)
        except RuntimeError as e: raise RuntimeError(label+'\n'+str(e)) from e
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for idx,_ in enumerate(pool.map(compile_one, () if a.reuse_compiled else compiles.items()), 1):
            if idx%40==0: print(f'{idx} objects compiled',flush=True)
    for name in CLEAN_NAMES:
        link=links[name]
        run(link,a.cache)
        final=package/name
        run(['strip','-S','-o',final,link[link.index('-o')+1]],a.cache)
        run(['codesign','--force','--sign','-',final],a.cache)
        run(['codesign','--verify','--strict',final],a.cache)
        if any(p in final.read_bytes() for p in [b'/Users/',b'/private/tmp/',b'/var/folders/']):
            raise ValueError('Unexpected private build path in '+name)
        print(name,digest(final),flush=True)
    record(out/'hashes.json',{p.name:digest(p) for p in package.iterdir()})
if __name__=='__main__':
    main()
