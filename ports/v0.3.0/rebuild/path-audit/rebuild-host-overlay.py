#!/usr/bin/env python3
"""Recompile the exact player dependency closure for a set of changed title source files."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
PLAYER = 'fm4_static_recomp'
DISPLAY = 'CMakeFiles/fm4_static_recomp.dir/src/ui/display_refresh_mac.mm.o'
FRAMEWORK = 'Cocoa'
def get(p): return json.loads(p.read_text())
def write(p,obj): p.write_text(json.dumps(obj,indent=2)+'\n')
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def capture(cmd,cache):
    return subprocess.check_output(list(map(str,cmd)),cwd=cache,text=True)
def setarg(c,flag,val):c[c.index(flag)+1]=str(val)
def main():
    pa=argparse.ArgumentParser(description=__doc__)
    for k in ('project','previous-host-source','cache','previous-normal','previous-clean','normal','clean'):
        pa.add_argument('--'+k,type=Path,required=True)
    a=pa.parse_args()
    H=a.project.resolve();base=a.previous_host_source.resolve();cache=a.cache.resolve()
    ref=cache.parents[2]; old=a.previous_normal.resolve(); seed=a.previous_clean.resolve(); N=a.normal.resolve(); C=a.clean.resolve()
    manifest=get(old/'build-result.json')
    original=get(seed/'commands.json');package=old/'package'
    if N.exists() or C.exists():raise ValueError('fresh destinations required')
    N.mkdir(parents=True);C.mkdir(parents=True)
    oldcmd=manifest['commands']
    changed={p.relative_to(H) for p in (H/'src').rglob('*') if p.is_file() and (not (base/p.relative_to(H)).is_file() or sha(p)!=sha(base/p.relative_to(H)))}
    deleted=[p for p in (base/'src').rglob('*') if p.is_file() and not (H/p.relative_to(base)).is_file()]
    if deleted:raise ValueError('source removals require normal full build')
    if not changed:raise ValueError('no changed title sources')
    print('Changed sources: '+', '.join(sorted(map(str,changed))),flush=True)
    if any(p.suffix not in ('.cpp','.mm','.h') for p in changed):
        raise ValueError('Unsupported new source type')
    roots=get(old/'overlay.json')
    snapshot=N/'host-sources'; snapshot.mkdir()
    for rel in sorted(changed):
        src=snapshot/rel;src.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(H/rel,src)
        for virtual in (H/rel,ref/rel):
            items=[i for i in roots['roots'] if i['name']==str(virtual)]
            if not items: roots['roots'].append({'type':'file','name':str(virtual),'external-contents':str(src)})
            else:
                for item in items:item['external-contents']=str(src)
    vfs=N/'overlay.json';write(vfs,roots)
    aliases={str(base/p):str(p) for p in changed}|{str(ref/p):str(p) for p in changed}|{str(H/p):str(p) for p in changed}
    closure={}
    for chunk in capture(['ninja','-t','deps'],cache).split('\n\n'):
        lines=chunk.splitlines()
        if not lines:continue
        key=lines[0].partition(': #deps')[0]
        if not key.startswith('CMakeFiles/fm4_static_recomp.dir/'):continue
        hits=sorted({aliases[line.strip()] for line in lines[1:] if line.strip() in aliases})
        if hits:
            if '(VALID)' not in lines[0]:raise ValueError('stale cache: '+key)
            closure[key]=hits
    for p in changed:
        if p.suffix in ('.cpp','.mm'):
            k='CMakeFiles/fm4_static_recomp.dir/'+str(p)+'.o';closure.setdefault(k,[]).append(str(p))
    # The original cache predates the Objective-C++ unit. Its source includes
    # the graphics declarations directly and is in the previous measured plan.
    if any(str(x)=='src/ui/graphics_page.h' for x in changed):
        closure.setdefault(DISPLAY,[]).append('src/ui/graphics_page.h')
    print('Player dependency closure: '+', '.join(sorted(closure)),flush=True)
    for root,reference,raw in ((N,old,True),(C,seed,False)):
        out=root/'package';out.mkdir()
        for p in (reference/'package').iterdir():
            if p.name==PLAYER:continue
            if p.is_dir():shutil.copytree(p,out/p.name,symlinks=True)
            else:shutil.copy2(p,out/p.name)
        plan={} if raw else original['compile'].copy()
        link=oldcmd[PLAYER][:] if raw else original['link'][PLAYER][:]
        maps=[] if raw else [s for s in original['compile'][DISPLAY] if s.startswith(('-ffile-prefix-map=','-fdebug-compilation-dir='))]
        for k in sorted(closure):
            cmd=oldcmd[k][:] if k in oldcmd else shlex.split(capture(['ninja','-t','commands','-s',k],cache))
            cmd[1:1]=maps
            if '-ivfsoverlay' in cmd:setarg(cmd,'-ivfsoverlay',vfs)
            else:cmd[1:1]=['-ivfsoverlay',str(vfs)]
            ob=root/'objects'/k;ob.parent.mkdir(parents=True,exist_ok=True)
            for flag,val in (('-o',ob),('-MT',ob),('-MF',ob.with_suffix('.d'))):setarg(cmd,flag,val)
            match=[i for i,s in enumerate(link) if s==k or s.endswith('/'+k)]
            if len(match)!=1:raise ValueError('unknown link object '+k)
            link[match[0]]=str(ob);plan[k]=cmd
        with ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(lambda k:subprocess.run(plan[k],cwd=cache,check=True,stdout=subprocess.DEVNULL), sorted(closure)))
        for i,t in enumerate(link):
            if Path(t).is_absolute() and Path(t).parent==reference/'package' and t.endswith('.dylib'):
                link[i]=str(out/Path(t).name)
        if FRAMEWORK not in link:link+=['-framework',FRAMEWORK]
        exe=out/PLAYER;linked=exe if raw else root/'unstripped'/PLAYER
        linked.parent.mkdir(exist_ok=True);setarg(link,'-o',linked)
        subprocess.run(link,cwd=cache,check=True)
        if not raw:
            subprocess.run(['strip','-S','-o',exe,linked],check=True)
            subprocess.run(['codesign','--force','--sign','-',exe],check=True)
            assert not any(s in exe.read_bytes() for s in (b'/Users/',b'/private/tmp/', b'/var/folders/'))
        subprocess.run(['codesign','--verify','--strict',exe],check=True)
        expected={p.name:sha(p) for p in (reference/'package').glob('*.dylib')}
        assert expected=={p.name:sha(p) for p in out.glob('*.dylib')}
        extras=[Path('tests/graphics_page_test.cpp'),Path('CMakeLists.txt'),Path('tools/bench/native_qhd/build_window.py')]
        sources={str(p):sha(H/p) for p in sorted(changed|set(extras))}
        if raw:
            receipt=manifest.copy();receipt['commands']=oldcmd|plan|{PLAYER:link}
            receipt['resolved_dependencies']=manifest['resolved_dependencies']|{k:[str(H/p) for p in files] for k,files in closure.items()}
            receipt['host_sources']=manifest['host_sources']|{str(H/p):s for p,s in sources.items()}
            receipt['hashes']=manifest['hashes']|{PLAYER:sha(exe)}
            receipt['dependencies']=manifest['dependencies']|{PLAYER:capture(['otool','-L',exe],cache).splitlines()[1:]}
            receipt['incremental_from']=str(old/'build-result.json')
            receipt['steps']={k:{'status':0} for k in [*closure,PLAYER]}
            write(root/'build-result.json',receipt)
        else:
            write(root/'commands.json',{'compile':plan,'link':original['link']|{PLAYER:link}})
            write(root/'incremental.json',{'sources':sources,'rebuilt':closure,'player_sha256':sha(exe),'retained_libraries':expected})
        print(root,sha(exe),flush=True)
if __name__=='__main__':main()
