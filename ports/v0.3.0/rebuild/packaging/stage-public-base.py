#!/usr/bin/env python3
"""Stage the engine with support files from the published v0.2.0 DMG in an empty output directory."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

RELEASE="https://github.com/retronative/fm4-recomp/releases/tag/v0.2.0"
BASE_SHA="c05a16c64f81ec43de5202a07495a62101ca4a26557c4307366c407537180731"
LAUNCHER="bdec53892484889a73fba7c0fc428626f237b2ff"

def run(*cmd, **kw):
    return subprocess.run(cmd,check=True,**kw)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ["dmg","companion","launcher","host","sdk","engine","clean-engine","output"]:
        parser.add_argument("--"+key,type=Path,required=True)
    parser.add_argument("--version",default="0.3.0")
    parser.add_argument("--build-number",required=True)
    a=parser.parse_args()
    if a.output.exists():
        parser.error("Select an empty destination that does not already exist.")
    with a.dmg.open('rb') as f:
        if hashlib.file_digest(f,'sha256').hexdigest()!=BASE_SHA:
            parser.error("Base DMG differs from the checksum at "+RELEASE)
    print("Public v0.2.0 base checksum verified.",flush=True)
    src=a.companion.resolve(strict=True)
    kit=src/'rebuild/packaging'
    host_source=a.launcher.resolve(strict=True)
    if subprocess.check_output(['git','-C',host_source,'rev-parse','v0.2.0^{}'],text=True).strip()!=LAUNCHER:
        parser.error("The public macOS launcher checkout does not contain the expected v0.2.0 tag.")
    with tempfile.TemporaryDirectory(prefix='fm4-public-base-') as work:
        temp=Path(work);mount=temp/'volume';mount.mkdir()
        launcher=temp/'launcher';launcher.mkdir()
        tar=temp/'launcher.tar'
        run('git','-C',host_source,'archive','-o',tar,LAUNCHER,'app/setup','scripts/alpha')
        with tarfile.open(tar) as files:
            files.extractall(launcher,filter='data')
        run('git','apply','--check',src/'project-changes/mac-launcher.patch',cwd=launcher)
        run('git','apply',src/'project-changes/mac-launcher.patch',cwd=launcher)
        run('/usr/bin/hdiutil','attach','-quiet','-nobrowse','-noautoopen','-readonly','-mountpoint',mount,a.dmg.resolve())
        try:
            base=mount/'FM4 Recomp.app'
            run('codesign','--verify','--deep','--strict',base)
            run(sys.executable,kit/'stage_release.py','--project',launcher,'--base-app',base,
                '--engine',a.engine.resolve(), '--host',a.host.resolve(),'--sdk',a.sdk.resolve(),
                '--engine-binaries',a.clean_engine.resolve(),'--libraries',a.clean_engine.resolve(),
                '--notice',kit/'THIRD_PARTY_NOTICES.md','--licenses',src/'licenses',
                '--version',a.version,'--build-number',a.build_number,'--output',a.output.absolute())
        finally:
            run('/usr/bin/hdiutil','detach','-quiet',mount)
    print("Staged verified public-base app:",a.output.absolute()/'FM4 Recomp.app')

if __name__=='__main__':
    main()
