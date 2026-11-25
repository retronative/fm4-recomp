#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Relink the SDK runtime from a completed source build with the three shared decoder libraries."""
import argparse
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True, help='Configured macOS host Ninja build')
parser.add_argument('--libraries', type=Path, required=True, help='Output of build-libraries.py')
parser.add_argument('--output', type=Path, required=True, help='A new file named librexruntime.dylib')
args = parser.parse_args()
out = args.output.resolve()
if out.exists() or out.name != 'librexruntime.dylib':
    parser.error('Choose a new output named librexruntime.dylib')
build = args.build.resolve(strict=True)
target = 'rexglue-artifacts/librexruntime.dylib'
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', '-s', target], text=True)
steps = [shlex.split(step.strip()) for step in commands.split(' && ')]
links = [step for step in steps if '-dynamiclib' in step and '-o' in step and step[step.index('-o') + 1] == target]
if len(links) != 1:
    parser.error('The build must provide exactly one link command for the SDK runtime')
link = links[0]
for archive, shared in [('liblibavcodec.a', 'libavcodec.58.dylib'),
                        ('liblibavutil.a', 'libavutil.56.dylib'),
                        ('libmspack.a', 'libshiftglue_lzx.dylib')]:
    indices = [i for i, value in enumerate(link) if Path(value).name == archive]
    if len(indices) != 1:
        parser.error('Expected one link input for ' + archive)
    link[indices[0]] = str((args.libraries / shared).resolve(strict=True))
link[link.index('-o') + 1] = str(out)
if '-lobjc' not in link:
    link.append('-lobjc')
link.append('-Wl,-rpath,@loader_path')
out.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(link, cwd=build, check=True)
subprocess.run(['codesign', '--force', '--sign', '-', str(out)], check=True)
