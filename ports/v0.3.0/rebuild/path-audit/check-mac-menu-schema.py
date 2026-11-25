#!/usr/bin/env python3
"""Generate and check the player's schema in its asset-free, CPU-only tool mode."""
import argparse, json, os, subprocess, tempfile
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('engine',type=Path)
args=parser.parse_args()
engine=args.engine.resolve(strict=True)
with tempfile.TemporaryDirectory() as tmp:
    schema=Path(tmp)/'graphics-settings.json'
    env={'HOME':tmp,'TMPDIR':tmp,'PATH':os.defpath,'FM4_COMPARE_OFFSCREEN':'1',
         'SDL_VIDEODRIVER':'offscreen','SDL_AUDIODRIVER':'dummy'}
    cmd=[engine/'fm4_static_recomp','--headless=true','--audio_mute=true',
         f'--game_data_root={tmp}',f'--user_data_root={tmp}',f'--log_file={tmp}/schema.log',
         f'--fm4_write_graphics_schema={schema}']
    subprocess.run(cmd,env=env,cwd=tmp,check=True,capture_output=True,timeout=30)
    data=json.loads(schema.read_bytes())
    rows=[r for b in data['blocks'] for r in b['rows'] if r.get('key')=='resolution_scale']
    expected=[{'label':'720p','value':'1'},{'label':'1080p (Full HD)','value':'5'},{'label':'1440p (QHD)','value':'2'},{'label':'4K (2160p)','value':'3'}]
    aa = [r for b in data['blocks'] for r in b['rows'] if r.get('key')=='swap_post_effect']
    if len(rows)!=1 or rows[0]['choices']!=expected or rows[0].get('default')!='5' or len(aa)!=1 or aa[0].get('default')!='fxaa':
        raise ValueError('Unexpected resolution entries: '+str(rows))
    (engine/'graphics-settings.json').write_bytes(schema.read_bytes())
    print([choice['label'] for choice in expected])
