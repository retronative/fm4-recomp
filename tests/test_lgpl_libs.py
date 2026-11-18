#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# scripts/release/check_lgpl_libs.sh on stand-in kits built here with xcrun clang (stand-in programs and libraries with the
# real file names and one real function each): the layout scripts/release/bundle_kit.sh writes passes, and each planted
# fault fails with its own message (the LZX decoder or FFmpeg linked into a program again, a library missing, a library
# that is not the one it is named as, a library not named @rpath/<file>, a program that does not load a library).
# After ReCarbon tests/lgpl_dylibs_test.py (GPL-3.0-or-later). macOS only; everything in a scratch folder.
#   /opt/homebrew/bin/python3 -B -I tests/test_lgpl_libs.py
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECK = os.path.join(ROOT, 'scripts', 'release', 'check_lgpl_libs.sh')
LIBS = {'libavcodec.61.dylib': 'avcodec_send_packet', 'libavutil.59.dylib': 'av_frame_alloc', 'libfm4_lzx.dylib': 'lzxd_decompress'}
failures = 0


def clang(source, out, *args):
    with open(out + '.c', 'w') as f:
        f.write(source)
    subprocess.run(['xcrun', 'clang', '-arch', 'arm64', out + '.c', '-o', out, *args], check=True, capture_output=True)
    os.remove(out + '.c')


def make(kit, libs=LIBS, inside=(), fm4_links=tuple(LIBS), survey_links=('libfm4_lzx.dylib',), install_name='@rpath/{}'):
    """A stand-in kit: `libs` (file -> the function it defines) in lib/; bin/fm4recomp and bin/xexsurvey load `fm4_links` and
    `survey_links` from there; the functions in `inside` are compiled into bin/fm4recomp instead."""
    os.makedirs(os.path.join(kit, 'bin'))
    os.makedirs(os.path.join(kit, 'lib'))
    for name, function in libs.items():
        clang(f'int {function}(void) {{ return 1; }}\n', os.path.join(kit, 'lib', name), '-dynamiclib',
              '-install_name', install_name.format(name))
    for program, links in (('fm4recomp', fm4_links), ('xexsurvey', survey_links)):
        used = [LIBS[n] for n in links]
        own = list(inside) if program == 'fm4recomp' else []
        source = ''.join(f'int {f}(void);\n' for f in used) + ''.join(f'int {f}(void) {{ return 2; }}\n' for f in own)
        source += 'int main(void) { return 0' + ''.join(f' + {f}()' for f in used + own) + '; }\n'
        clang(source, os.path.join(kit, 'bin', program), *[os.path.join(kit, 'lib', n) for n in links])


def expect(tmp, name, message, **faults):
    """The check passes on the kit (message None) or fails naming `message`."""
    global failures
    kit = os.path.join(tmp, name)
    make(kit, **faults)
    run = subprocess.run(['/bin/bash', CHECK, kit], capture_output=True, text=True)
    ok = run.returncode == 0 if message is None else run.returncode == 1 and message in run.stderr
    if not ok:
        failures += 1
        print(f'FAIL: {name}: exit {run.returncode}, expected {message!r}\n{run.stderr}')


with tempfile.TemporaryDirectory() as tmp:
    expect(tmp, 'good', None)
    # Negative controls: one fault each.
    expect(tmp, 'lzx_inside', 'fm4recomp contains LGPL library code (_lzxd_decompress', inside=('lzxd_decompress',),
           fm4_links=('libavcodec.61.dylib', 'libavutil.59.dylib'))
    expect(tmp, 'ffmpeg_inside', 'fm4recomp contains LGPL library code (_av', inside=('avcodec_send_packet', 'av_frame_alloc'),
           fm4_links=('libfm4_lzx.dylib',))
    expect(tmp, 'ff_inside', 'fm4recomp contains LGPL library code (_ff_xma_decode', inside=('ff_xma_decode',))
    expect(tmp, 'missing', 'lib/libavutil.59.dylib is missing',
           libs={k: v for k, v in LIBS.items() if k != 'libavutil.59.dylib'}, fm4_links=('libavcodec.61.dylib', 'libfm4_lzx.dylib'))
    expect(tmp, 'wrong_library', 'lib/libfm4_lzx.dylib does not define lzxd_decompress',
           libs={**LIBS, 'libfm4_lzx.dylib': 'something_else'}, fm4_links=('libavcodec.61.dylib', 'libavutil.59.dylib'), survey_links=())
    expect(tmp, 'absolute_name', 'lib/libavcodec.61.dylib is not named @rpath/libavcodec.61.dylib', install_name='/opt/somewhere/{}')
    expect(tmp, 'not_loaded', 'bin/fm4recomp does not load @rpath/libavutil.59.dylib', fm4_links=('libavcodec.61.dylib', 'libfm4_lzx.dylib'))
    expect(tmp, 'survey_not_loaded', 'bin/xexsurvey does not load @rpath/libfm4_lzx.dylib', survey_links=())
print('test_lgpl_libs: ' + ('PASS (1 kit accepted, 8 planted faults refused)' if failures == 0 else f'{failures} FAILED'))
sys.exit(1 if failures else 0)
