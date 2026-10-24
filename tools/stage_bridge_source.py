#!/usr/bin/env python3
"""Plan source-only consolidation; applying requires a separately reviewed exact seal.

Default mode reads explicit research directories, without copying them. After the
integration owner freezes those files, save a plan and pass it with --apply.
Changed source bytes or unreviewed include rewrites refuse before any copying.
"""
import argparse, hashlib, json, shutil
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SUFFIXES = {'.h','.hpp','.cpp','.mm'}

def sha(data): return hashlib.sha256(data).hexdigest()
def plan(source, legacy):
    rows=[]
    for folder in ['host','native']:
        for item in sorted((source/folder).iterdir()):
            if item.is_symlink():
                if item.suffix in SUFFIXES: raise ValueError('Source symlinks are not admitted')
                continue
            if not item.is_file() or (item.suffix not in SUFFIXES and item.name != 'build_helper.py'): continue
            data=item.read_bytes(); output=data; transform=None
            if item.name=='FLStandaloneGraphDecoder.h':
                old=b'"../../../../FL Studio ARA/2026-10-23/host/FLNativeModel.h"'
                if output.count(old)!=1: raise ValueError('Expected one reviewed native-model include')
                output=output.replace(old,b'"FLNativeModel.h"'); transform='repo-native-model-include'
            if item.name=='build_helper.py':
                old=b"here.parents[3] / 'FL Studio ARA/2026-10-23/host'"
                if output.count(old)!=1: raise ValueError('Expected one reviewed legacy source fallback')
                output=output.replace(old,b"here.parent / 'legacy'");transform='repo-legacy-default'
            rows.append({'source':str(item.resolve()),'destination':f'{folder}/{item.name}','sha256':sha(data),'output_sha256':sha(output),'transform':transform,'bytes':len(data)})
    for name in ['FLNativeModel.h','FLNativeModel.cpp']:
        item=legacy/name
        if item.is_symlink():raise ValueError('Legacy source symlinks are not admitted')
        data=item.read_bytes();rows.append({'source':str(item.resolve()),'destination':'legacy/'+name,'sha256':sha(data),'output_sha256':sha(data),'transform':None,'bytes':len(data)})
    return {'schema':'rysup.fl-ara-source-consolidation/1','source_root':str(source.resolve()),'legacy_root':str(legacy.resolve()),'destination_root':'native/bridge','sdk_source_included':False,'vendor_binary_or_audio_included':False,'files':rows}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-root',type=Path,required=True);p.add_argument('--legacy-host',type=Path,required=True);p.add_argument('--apply',type=Path,metavar='REVIEWED_PLAN_JSON');a=p.parse_args()
    current=plan(a.source_root,a.legacy_host)
    if a.apply is None:print(json.dumps(current,indent=2));return
    expected=json.loads(a.apply.read_text())
    if current!=expected:raise ValueError('Sources changed or plan differs; no consolidation performed')
    destination=ROOT/'native/bridge'
    if destination.exists():raise ValueError('Durable bridge source already exists; refusing replacement')
    staging=ROOT/'native/bridge.staging'
    if staging.exists():raise ValueError('Source staging directory already exists; preserve for review')
    staging.mkdir()
    try:
        for row in current['files']:
            data=Path(row['source']).read_bytes()
            if sha(data)!=row['sha256']:raise ValueError('Source changed during staging')
            if row['transform']=='repo-native-model-include':data=data.replace(b'"../../../../FL Studio ARA/2026-10-23/host/FLNativeModel.h"',b'"FLNativeModel.h"')
            if row['transform']=='repo-legacy-default':data=data.replace(b"here.parents[3] / 'FL Studio ARA/2026-10-23/host'",b"here.parent / 'legacy'")
            if sha(data)!=row['output_sha256']:raise ValueError('Unreviewed transformation')
            target=staging/row['destination'];target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        # Recheck every original before publishing the staged directory.
        for row in current['files']:
            if sha(Path(row['source']).read_bytes())!=row['sha256']:raise ValueError('Source changed before consolidation completed')
        (staging/'source-provenance.json').write_text(json.dumps(current,indent=2)+'\n')
        staging.rename(destination)
    except BaseException:
        shutil.rmtree(staging);raise
    print(destination)
if __name__=='__main__':main()
