#!/usr/bin/env python3
"""Plan source-only consolidation; applying requires a separately reviewed exact seal.

Default mode reads explicit research directories, without copying them. After the
integration owner freezes those files, save a plan and pass it with --apply.
Changed source bytes or unreviewed include rewrites refuse before any copying.
"""
import argparse, hashlib, json, shutil
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SUFFIXES = {'.h','.hpp','.cpp','.mm','.py'}

def sha(data): return hashlib.sha256(data).hexdigest()
def plan(source, legacy, freeze_receipt=None):
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
                if output.count(old)==1:
                    output=output.replace(old,b'"FLNativeModel.h"'); transform='repo-native-model-include'
                elif output.count(b'"../legacy/FLNativeModel.h"')!=1 and output.count(b'"FLNativeModel.h"')!=1:
                    raise ValueError('Expected one reviewed native-model include')
            if item.name=='build_helper.py':
                old=b"here.parents[3] / 'FL Studio ARA/2026-10-23/host'"
                if output.count(old)==1:
                    output=output.replace(old,b"here.parent / 'legacy'");transform='repo-legacy-default'
                elif b"here.parent/'legacy'" not in output and b"here.parent / 'legacy'" not in output:
                    raise ValueError('Expected one reviewed legacy source fallback')
            if item.name=='check_standalone_host_contracts.py':
                old=b"WORKSPACE = Path('/Users/jordanrys/Documents/Rys Up Audio')\nARA = WORKSPACE/'Development Resources/Dependencies/ARA_SDK'\nVST = WORKSPACE/'Plugins/Source/RysUpMultiBand/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK'\nOUT = SOURCE/'contract-checks'"
                new=b"ARA = Path(os.environ.get('ARA_SDK_ROOT', '/missing/ARA_SDK_ROOT')).resolve()\nVST = Path(os.environ.get('VST3_SDK_ROOT', '/missing/VST3_SDK_ROOT')).resolve()\nOUT = Path(os.environ.get('FL_CONTRACT_OUTPUT', '/missing/FL_CONTRACT_OUTPUT')).resolve()"
                if output.count(old)!=1:raise ValueError('Expected reviewed contract-check path block')
                output=output.replace(old,new).replace(b'#!/opt/homebrew/bin/python3.13',b'#!/usr/bin/env python3',1)
                output=output.replace(b'    OUT.mkdir(exist_ok=True)',b"    if not all(os.environ.get(k) for k in ['ARA_SDK_ROOT','VST3_SDK_ROOT','FL_CONTRACT_OUTPUT']):\n        raise SystemExit('Set external ARA_SDK_ROOT, VST3_SDK_ROOT and FL_CONTRACT_OUTPUT before running checks')\n    OUT.mkdir(parents=True,exist_ok=True)")
                transform='repo-contract-env'
            rows.append({'source':str(item.resolve()),'destination':f'{folder}/{item.name}','sha256':sha(data),'output_sha256':sha(output),'transform':transform,'bytes':len(data)})
    for name in ['FLNativeModel.h','FLNativeModel.cpp']:
        item=legacy/name
        if item.is_symlink():raise ValueError('Legacy source symlinks are not admitted')
        data=item.read_bytes();rows.append({'source':str(item.resolve()),'destination':'legacy/'+name,'sha256':sha(data),'output_sha256':sha(data),'transform':None,'bytes':len(data)})
    if freeze_receipt:
        frozen=json.loads(freeze_receipt.read_text())
        if frozen.get('schema')!='rysup.fl-ara-source-freeze/1':raise ValueError('Unsupported freeze receipt')
        expected={str(Path(r['frozen']).resolve()):r['sha256'] for r in frozen['inputs']}
        actual={r['source']:r['sha256'] for r in rows}
        if actual!=expected:raise ValueError('Frozen input set or source seals differ from owner freeze')
    return {'upstream_source_freeze_sha256':sha(freeze_receipt.read_bytes()) if freeze_receipt else None,'schema':'rysup.fl-ara-source-consolidation/1','source_root':str(source.resolve()),'legacy_root':str(legacy.resolve()),'destination_root':'native/bridge','sdk_source_included':False,'vendor_binary_or_audio_included':False,'files':rows}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-root',type=Path,required=True);p.add_argument('--legacy-host',type=Path,required=True);p.add_argument('--apply',type=Path,metavar='REVIEWED_PLAN_JSON');p.add_argument('--freeze-receipt',type=Path);a=p.parse_args()
    current=plan(a.source_root,a.legacy_host,a.freeze_receipt)
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
            if row['transform']=='repo-contract-env':
                # Recompute the reviewed transformation through the same planner.
                item=Path(row['source']); original=data
                data=data.replace(b"WORKSPACE = Path('/Users/jordanrys/Documents/Rys Up Audio')\nARA = WORKSPACE/'Development Resources/Dependencies/ARA_SDK'\nVST = WORKSPACE/'Plugins/Source/RysUpMultiBand/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK'\nOUT = SOURCE/'contract-checks'",b"ARA = Path(os.environ.get('ARA_SDK_ROOT', '/missing/ARA_SDK_ROOT')).resolve()\nVST = Path(os.environ.get('VST3_SDK_ROOT', '/missing/VST3_SDK_ROOT')).resolve()\nOUT = Path(os.environ.get('FL_CONTRACT_OUTPUT', '/missing/FL_CONTRACT_OUTPUT')).resolve()").replace(b'#!/opt/homebrew/bin/python3.13',b'#!/usr/bin/env python3',1)
                data=data.replace(b'    OUT.mkdir(exist_ok=True)',b"    if not all(os.environ.get(k) for k in ['ARA_SDK_ROOT','VST3_SDK_ROOT','FL_CONTRACT_OUTPUT']):\n        raise SystemExit('Set external ARA_SDK_ROOT, VST3_SDK_ROOT and FL_CONTRACT_OUTPUT before running checks')\n    OUT.mkdir(parents=True,exist_ok=True)")
            if sha(data)!=row['output_sha256']:raise ValueError('Unreviewed transformation')
            target=staging/row['destination'];target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        # Recheck every original before publishing the staged directory.
        for row in current['files']:
            if sha(Path(row['source']).read_bytes())!=row['sha256']:raise ValueError('Source changed before consolidation completed')
        public={'schema':current['schema'],'upstream_source_freeze_sha256':current['upstream_source_freeze_sha256'],'sdk_source_included':False,'vendor_binary_or_audio_included':False,'files':[{k:v for k,v in row.items() if k!='source'} for row in current['files']]}
        (staging/'source-provenance.json').write_text(json.dumps(public,indent=2)+'\n')
        staging.rename(destination)
    except BaseException:
        shutil.rmtree(staging);raise
    print(destination)
if __name__=='__main__':main()
