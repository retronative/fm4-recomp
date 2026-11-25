#!/usr/bin/env python3
"""Rebuild the measured SDK window variant from a read-only, matching Ninja cache."""
import argparse
import filecmp
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
HOST_ROOT = HERE.parents[2]
HOST_OBJECT = 'CMakeFiles/fm4_static_recomp.dir/src/main.cpp.o'
DISPLAY_SOURCE = Path('src/ui/display_refresh_mac.mm')
DISPLAY_OBJECT = f'CMakeFiles/fm4_static_recomp.dir/{DISPLAY_SOURCE}.o'
GRAPHICS_OBJECT = 'CMakeFiles/fm4_static_recomp.dir/src/ui/graphics_page.cpp.o'
UI = Path('src/ui/vulkan/vulkan_presenter.cpp')
WINDOW = Path('src/ui/window_sdl.cpp')
CP = Path('src/graphics/vulkan/command_processor.cpp')
COMMON = CP.with_name('fm4_bench_output.inc')
HEADERS = (Path('include/rex/ui/surface_mac.h'), Path('include/rex/ui/vulkan/presenter.h'))
SCOPED_SOURCES = (Path('src/graphics/shared_memory.cpp'), Path('src/graphics/pipeline/texture/cache.cpp'))
SCOPED_HEADERS = (Path('include/rex/graphics/shared_memory.h'), Path('include/rex/graphics/pipeline/texture/cache.h'),
                  Path('include/rex/graphics/pipeline/shader/spirv.h'))
DRAW_RANGES = Path('include/rex/graphics/draw_page_ranges.h')
VULKAN_TEXTURES = Path('src/graphics/vulkan/texture_cache.cpp')
COMPACT_FILES = (Path('include/rex/graphics/vulkan/texture_cache.h'),
                 Path('include/rex/graphics/vulkan/command_processor.h'), VULKAN_TEXTURES,
                 CP.with_name('fh1_native_executor.cpp'), CP.with_name('fm4_native_executor.cpp'),
                 CP.with_name('render_target_cache.cpp'))
BANK_MAP = Path('include/rex/graphics/vulkan/scaled_resolve_banks.h')
FHD_EXECUTOR_HEADER = Path('include/rex/graphics/vulkan/fh1_native_executor.h')
FHD_SOURCES = tuple(CP.with_name(name + '.cpp') for name in ('fh1_camera', 'fh1_inspector', 'scene_post'))
FHD_SHADER_SOURCES = tuple(Path('src/graphics/shaders') / name for name in (
    'fh1_native_edram.hlsli', 'fh1_native_resolve_memory.cs.hlsl',
    'fh1_native_transfer.ps.hlsl', 'fh1_native_transfer_words.cs.hlsl', 'hdr_hud_mask.cs.glsl'))
RUNTIME = 'rexglue-artifacts/librexruntime.dylib'
GPU = 'rexglue-artifacts/librexgpu-fh1.dylib'
LINKS = (RUNTIME, GPU, 'libfm4_static_recomp_XMediaFacade_default.dylib',
         'libfm4_static_recomp_SpeechFacade_default.dylib', 'fm4_static_recomp')
def die(message):
    raise SystemExit(message)
def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()
def captured(argv, cwd):
    return subprocess.check_output(argv, cwd=cwd, text=True)
def native_command(target, cache):
    lines = captured(['ninja','-t','commands','-s',target], cache).splitlines()
    if len(lines) != 1:
        die(f'ambiguous Ninja action for {target}')
    return shlex.split(lines[0])
def set_arg(args, flag, value):
    if args.count(flag) != 1: die(f'ambiguous {flag} in compiler command')
    args[args.index(flag)+1] = str(value)
def deps_for(sdk, cache, extra_sources=()):
    inputs = {str(sdk/p) for p in (*HEADERS,UI,WINDOW,CP,*extra_sources)}
    deps = captured(['ninja','-t','deps'], cache)
    found = {}
    for entry in deps.split('\n\n'):
        lines = entry.splitlines()
        if not lines: continue
        names = set(line.strip() for line in lines[1:])
        matched = names & inputs
        if matched:
            first, sep, rest = lines[0].partition(': #deps')
            if not sep or '(VALID)' not in rest or not (cache/first).is_file():
                die(f'stale/missing dependency entry: {lines[0]}')
            found[first] = sorted(matched)
    union = set().union(*map(set,found.values())) if found else set()
    if inputs != union:
        die(f'incomplete dependency map, missing {inputs-union}')
    return found
def link_command(target, cache):
    a = native_command(target, cache)
    if a[:2] != [':','&&'] or '&&' not in a[2:]: die(f'unrecognized linker shell: {target}')
    boundary = a.index('&&', 2)
    cmd, after = a[2:boundary], a[boundary+1:]
    if not cmd[0].endswith('/clang++') or any(v in (';', '|', '&&','||','>','<') for v in cmd):
        die(f'unsupported linker: {target}')
    if target != 'fm4_static_recomp' and after != [':']: die(f'unexpected link suffix: {target}')
    if target == 'fm4_static_recomp':
        # Replaced by explicit staging into the private package; never execute
        # the CMake post-build copies which target the original build tree.
        chunks = [[]]
        for arg in after:
            if arg == '&&': chunks.append([])
            else: chunks[-1].append(arg)
        for part in chunks:
            if part == ['cd', str(cache)]: continue
            if (len(part) >= 4 and Path(part[0]).name == 'cmake' and part[1]=='-E' and
                    part[2] in ('copy_if_different','make_directory')): continue
            die(f'unexpected post-build step {part}')
    return cmd
def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('cache', type=Path, help='reference out/build/mac-arm64-release')
    p.add_argument('sdk', type=Path, help='SDK source used by that cache')
    p.add_argument('measured', type=Path, help='measured overlay root containing package/ and src/')
    p.add_argument('output', type=Path, help='fresh private output directory')
    p.add_argument('--plan',action='store_true',help='dry plan, writes no files and invokes no compiler or game')
    p.add_argument('--scoped-pages',action='store_true',help='build the opt-in per-draw page checks')
    p.add_argument('--resolve-coverage',action='store_true',help='include the opt-in scaled-resolve memory probe')
    p.add_argument('--compact-resolve',action='store_true',help='include the opt-in QHD resolve backing')
    p.add_argument('--native-fhd',action='store_true',help='include the native 1920x1080 raster mode')
    a=p.parse_args()
    if a.resolve_coverage and a.compact_resolve:
        p.error('measure the original address space in a separate coverage run')
    if a.native_fhd and not a.compact_resolve:
        p.error('--native-fhd requires --compact-resolve')
    cache,sdk,measured,out=(v.resolve() for v in (a.cache,a.sdk,a.measured,a.output))
    if any(out == x or out.is_relative_to(x) or x.is_relative_to(out) for x in (sdk,cache,measured)):
        die('output must be separate from cache, source, and measured package')
    if out.exists(): die(f'output already exists: {out}')
    seed = measured/'package'; seal=json.loads((seed/'overlay-build.json').read_text())
    if digest(measured/CP)!=seal['source_sha256']:
        die('measured GPU translation unit hash mismatch')
    if digest(measured/COMMON) != 'd318c97ae54519877bfdd1a966ae37127adf4dd95659e718acf0104bd792cb92':
        die('measured GPU shaders hash mismatch')
    for name, val in seal['hashes'].items():
        if digest(seed/name)!=val: die('measured package mismatch: '+name)
    for k in ('fm4_static_recomp','librexruntime.dylib',LINKS[2],LINKS[3]):
        if digest(cache/k)!=seal['hashes'][k]: die('Ninja cache differs from measured executable set: '+k)
    stage = out/'sdk-overlay'; package=out/'package'; objects=out/'objects'
    args_by_target={}
    extra_sources = SCOPED_SOURCES if a.scoped_pages else ()
    if a.resolve_coverage:
        extra_sources += (VULKAN_TEXTURES,)
    if a.compact_resolve:
        # VulkanTextureCache gains fields, including an inline size accessor.
        # Recompile all translation units that depend on its definition.
        extra_sources += COMPACT_FILES
    view_sources = (Path('src/ui/presenter.cpp'), Path('src/graphics/pipeline/shader/spirv_translator.cpp'))
    view_header = Path('include/rex/graphics/pipeline/shader/spirv_translator.h')
    if a.native_fhd:
        extra_sources += (*FHD_SOURCES, FHD_EXECUTOR_HEADER, view_header)
    # The added shader variant consumes a spare modification bit. Struct size and all existing offsets stay the same.
    deps=deps_for(sdk, cache, extra_sources + view_sources)
    main_compile = native_command(HOST_OBJECT, cache)
    cached_host = Path(main_compile[main_compile.index('-c')+1]).parent.parent
    # The cached game and these sources must have the same disc and function
    # partitioning. Rebuild each translated unit changed by a new menu hook.
    generated = Path('.local/generated/1.0/default')
    host_units = {HOST_OBJECT: [Path('src/main.cpp'), Path('src/fm4_app.h')],
                  'CMakeFiles/fm4_static_recomp.dir/src/game_hooks.cpp.o':
                      [Path('src/game_hooks.cpp'), Path('src/game_hooks.h')]}
    host_units[GRAPHICS_OBJECT] = [Path('src/ui/graphics_page.cpp')]
    host_units['CMakeFiles/fm4_static_recomp.dir/src/config/host_config.cpp.o'] = [Path('src/config/host_config.cpp')]
    host_units[DISPLAY_OBJECT] = [DISPLAY_SOURCE]
    for pattern in ('*.h', '*.cpp'):
        names = {p.name for p in (HOST_ROOT/generated).glob(pattern)}
        if not names or names != {p.name for p in (cached_host/generated).glob(pattern)}:
            die('regenerate the 1.0 translation with the matching partition before building')
        for name in sorted(names):
            rel = generated/name
            if filecmp.cmp(HOST_ROOT/rel, cached_host/rel, shallow=False):
                continue
            if not name.startswith('fm4_static_recomp_recomp.') or not name.endswith('.cpp'):
                die(f'generated ABI or partition changed: {name}; use a full build')
            host_units[f'CMakeFiles/fm4_static_recomp_recomp.dir/{rel}.o'] = [rel]
    for obj, paths in host_units.items():
        deps[obj] = [str(HOST_ROOT/path) for path in paths]
    stage_files=(*HEADERS,UI,WINDOW,CP,COMMON,*view_sources,view_header)
    if a.native_fhd:
        stage_files += (*FHD_SOURCES, FHD_EXECUTOR_HEADER, *FHD_SHADER_SOURCES)
    if a.resolve_coverage:
        stage_files += (VULKAN_TEXTURES,)
    if a.compact_resolve:
        stage_files += (*COMPACT_FILES, BANK_MAP, Path('tests/unit/graphics/fm4_scaled_resolve_test.cpp'))
    if a.scoped_pages:
        # These SDK header additions have no layout or vtable changes. Their new
        # methods are used only by the three GPU translation units being rebuilt.
        stage_files+=(*SCOPED_SOURCES,*SCOPED_HEADERS,DRAW_RANGES)
    overrides={t:objects/t for t in deps}
    vfs = out/'overlay.json'
    host_overlay = []
    for target in sorted(deps):
        # Older read-only caches predate the Mac refresh helper. Clang selects
        # Objective-C++ from its .mm extension using the host graphics flags.
        cmd=native_command(GRAPHICS_OBJECT if target == DISPLAY_OBJECT else target,cache)
        if target == DISPLAY_OBJECT:
            set_arg(cmd, '-c', HOST_ROOT/DISPLAY_SOURCE)
        if (not cmd[0].endswith('/clang++') or any(v.startswith('@') for v in cmd)
                or any('pch' in v for v in cmd) or any(v in (';', '&&','||','|','>') for v in cmd)):
            die(f'unsupported compile command: {target}')
        src=Path(cmd[cmd.index('-c')+1])
        if target in host_units:
            # Keep one canonical spelling for host headers included both by the
            # generated game code and by the app (they use pragma once).
            paths = host_units[target]
            if src != (HOST_ROOT if target == DISPLAY_OBJECT else cached_host)/paths[0]:
                die(f'wrong cached host source: {src}')
            for path in paths:
                host_overlay.append({'type':'file', 'name':str(cached_host/path),
                                     'external-contents':str(HOST_ROOT/path)})
        elif not src.is_relative_to(sdk):
            die(f'unexpected outside source TU {src}')
        cmd[1:1]=['-ivfsoverlay',str(vfs)]
        for f,v in (('-MT',overrides[target]),('-MF',overrides[target].with_suffix('.d')),('-o',overrides[target])):
            set_arg(cmd,f,v)
        args_by_target[target]=cmd
    linked=set()
    for target in LINKS:
        cmd=link_command(target,cache)
        if target == 'fm4_static_recomp':
            if DISPLAY_OBJECT not in cmd:
                cmd.insert(cmd.index(GRAPHICS_OBJECT) + 1, DISPLAY_OBJECT)
            if 'CoreGraphics' not in cmd:
                cmd.extend(['-framework', 'CoreGraphics'])
        if target == RUNTIME and '-lobjc' not in cmd:
            cmd.append('-lobjc')
        set_arg(cmd,'-o',package/Path(target).name)
        for i,arg in enumerate(cmd):
            if arg in overrides:
                cmd[i]=str(overrides[arg]); linked.add(arg)
            elif arg in (RUNTIME,GPU):
                cmd[i]=str(package/Path(arg).name)
            elif arg.startswith('-Wl,-rpath,') and str(cache) in arg:
                cmd[i]='-Wl,-rpath,@loader_path'
        args_by_target[target]=cmd
    if linked!=set(deps): die(f'recompiled objects not present in link targets: {set(deps)-linked}')
    print(f'{len(deps)} transitive object recompiles, {len(LINKS)} relinks; final package {package}',flush=True)
    if a.plan:
        for k in args_by_target: print(k)
        return
    out.mkdir(parents=True)
    package.mkdir()
    for rel in stage_files:
        if rel in (DRAW_RANGES, BANK_MAP): continue  # created by the SDK patch
        path=stage/rel;path.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile((measured if rel in (CP,COMMON) else sdk)/rel,path)
    patch_paths=[HERE/'window-hdr.patch',HERE/'window-standard.patch',HERE/'window-input.patch']
    # Anchor git-apply inside this private fragment tree even if the output
    # directory itself lives inside the port's checkout.
    subprocess.run(['git','init','--quiet',str(stage)],check=True)
    patches=[str(path) for path in patch_paths]
    subprocess.run(['git','-C',str(stage),'apply','--check',*patches],check=True)
    subprocess.run(['git','-C',str(stage),'apply',*patches],check=True)
    fast_aa = HERE/'standard-fast.patch'
    subprocess.run(['git','-C',str(stage),'apply',str(fast_aa)],check=True)
    patch_paths.append(fast_aa)
    if a.scoped_pages:
        scoped = HERE/'scoped-pages.patch'
        subprocess.run(['git','-C',str(stage),'apply',str(scoped)],check=True)
        patch_paths.append(scoped)
    if a.resolve_coverage:
        coverage = HERE/'resolve-coverage.patch'
        subprocess.run(['git','-C',str(stage),'apply',str(coverage)],check=True)
        patch_paths.append(coverage)
    if a.compact_resolve:
        compact = HERE/'compact-resolve.patch'
        subprocess.run(['git','-C',str(stage),'apply',str(compact)],check=True)
        patch_paths.append(compact)
    view = HERE / 'extended-view.patch'
    subprocess.run(['git','-C',str(stage),'apply',str(view)], check=True)
    patch_paths.append(view)
    if a.native_fhd:
        for name in ('native-fhd.patch', 'native-fhd-shaders.patch'):
            patch = HERE / name
            subprocess.run(['git', '-C', str(stage), 'apply', str(patch)], check=True)
            patch_paths.append(patch)
        compiler = HERE / 'compile_native_fhd.py'
        shader_cmd = [sys.executable, str(compiler), '--sdk', str(sdk), '--overlay', str(stage)]
        subprocess.run(shader_cmd, check=True)
        stage_files += tuple(Path(line) for line in captured(shader_cmd + ['--list'], stage).splitlines())
        patch_paths.append(compiler)
    stage_files = tuple(dict.fromkeys(stage_files))
    vfs.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[
        {'type':'file','name':str(sdk/rel),'external-contents':str(stage/rel)} for rel in stage_files]+host_overlay},indent=2))
    # Preserve relative local loader manifest, driver, and the shader/upscaler
    # resources of the verified reference bundle, excluding old binaries/objects.
    for name in ('vulkan','upscalers','graphics-settings.json'):
        subprocess.run(['/bin/cp','-Rc',str(seed/name),str(package/name)],check=True)
    for item in overrides.values(): item.parent.mkdir(parents=True,exist_ok=True)
    proof={'patches':{p.name:digest(p) for p in patch_paths},'resolved_dependencies':deps,
           'sources':{str(rel):digest(stage/rel) for rel in stage_files},'commands':args_by_target,'steps':{}}
    proof['host_sources'] = {path:digest(Path(path)) for obj in host_units for path in deps[obj]}
    (out/'build-plan.json').write_text(json.dumps(proof,indent=2))
    with (out/'build.log').open('w') as log:
        for label,cmd in args_by_target.items():
            print(label,flush=True)
            started=time.time()
            run=subprocess.run(cmd,cwd=cache,stdout=log,stderr=subprocess.STDOUT)
            proof['steps'][label]={'status':run.returncode,'seconds':round(time.time()-started,2)}
            if run.returncode:
                die(f'{label} failed ({run.returncode}); see {out}/build.log')
    dependencies={}
    for t in LINKS:
        art=package/Path(t).name
        ld=captured(['/usr/bin/otool','-L',str(art)],package)
        dependencies[art.name]=ld.splitlines()[1:]
        if not art.is_file() or str(cache) in ld: die('unexpected build-only load command in '+str(art))
        rpaths=captured(['/usr/bin/otool','-l',str(art)],package)
        if re.search(r'^\s*path '+re.escape(str(cache)),rpaths,re.M):
            die('absolute cached lib search path in '+str(art))
        subprocess.run(['/usr/bin/codesign','--verify',str(art)],check=True)
    proof['hashes']={Path(t).name:digest(package/Path(t).name) for t in LINKS}
    proof['dependencies']=dependencies
    (out/'build-result.json').write_text(json.dumps(proof,indent=2))
    print('Complete:',package)
if __name__=='__main__':main()
