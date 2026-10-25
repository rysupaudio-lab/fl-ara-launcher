#!/usr/bin/env python3
"""Compile/run isolated own-file glue controls; no native admission or plugin load."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ara-sdk',required=True);p.add_argument('--vst3-sdk',required=True);p.add_argument('--output',required=True)
    a=p.parse_args();n=Path(__file__).resolve().parent;h=n.parent/'host';l=n.parent/'legacy';ara=Path(a.ara_sdk).resolve();vst=Path(a.vst3_sdk).resolve();out=Path(a.output).resolve()
    out.mkdir(parents=True,exist_ok=True);(out/'tmp').mkdir(exist_ok=True)
    sources=[n/'FLAppBridgeSaveGuardIntegrationTests.mm',n/'FLNativeSaveGuard.mm',n/'FLVST3NativeHooks.cpp',n/'FLStandaloneSourceOverlay.cpp',n/'FLNativeProcessObserver.cpp',n/'FLNativeSlotPowerObserver.cpp',n/'FLHeadlessNativeExport.mm',n/'FLNativeMixBoundary.cpp',n/'RegistrationReadThrough.mm',n/'FLTestOutputSink.cpp',n/'FLUnitDeviceGate.cpp',h/'FLStandaloneGraphDecoder.cpp',h/'FLStandaloneStateEnvelope.cpp',h/'GenericARAHost.cpp',h/'VST3ARAEntry.cpp',h/'OwnedVST3PlaybackCompanion.cpp',h/'StandaloneARACoordinator.cpp',l/'FLNativeModel.cpp',ara/'ARA_Library/Dispatch/ARAHostDispatch.cpp',ara/'ARA_Library/Debug/ARADebug.c',vst/'pluginterfaces/base/funknown.cpp',vst/'pluginterfaces/base/coreiids.cpp']
    inputs=sources+[n/'FLAppBridge.mm']+sorted(n.glob('*.h'))+sorted(h.glob('*.h'))+[l/'FLNativeModel.h']
    seals={str(x):digest(x) for x in inputs};commands=[];objects=[]
    env=dict(os.environ,TMPDIR=str(out/'tmp'),CLANG_MODULE_CACHE_PATH=str(out/'tmp'/'modules'))
    common=['-arch','arm64','-O1','-g0','-mmacosx-version-min=12.0','-fsanitize=undefined','-fno-sanitize-recover=all','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-unused-parameter','-DARA_SUPPORT_VERSION_1=0','-DRYSUP_FL_SAVE_GUARD_TESTING=1','-DFL_APP_DECODER_SHA256="'+digest(h/'FLStandaloneGraphDecoder.cpp')+'"',*[ '-I'+str(x) for x in (n,h,l,ara,vst)]]
    def run(cmd):
        commands.append(cmd);result=subprocess.run(cmd,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        if result.returncode:
            (out/'failure.txt').write_text(result.stdout);raise RuntimeError(result.stdout)
        return result.stdout
    for i,s in enumerate(sources):
        obj=out/(str(i)+'-'+s.stem+'.o');language=['-std=c11','-x','c'] if s.suffix=='.c' else ['-std=c++17']
        if s.suffix=='.mm':language+=['-fobjc-arc']
        run(['clang++',*common,*language,'-c',str(s),'-o',str(obj)]);objects.append(obj)
    binary=out/'FLAppBridgeSaveGuardIntegrationTests'
    run(['clang++','-arch','arm64','-fsanitize=undefined','-mmacosx-version-min=12.0',*map(str,objects),*sum((['-framework',x] for x in ('CoreFoundation','Foundation','AppKit','AudioToolbox','CoreAudio','Security')),[]),'-o',str(binary)])
    result=json.loads(run([str(binary),str(out)]).strip())
    if any(digest(Path(x))!=seal for x,seal in seals.items()):raise RuntimeError('source changed during controls')
    receipt={'schema':'rysup.fl-native-save-glue-control-build/1','result':result,'commands':commands,'inputs':seals,'binary_sha256':digest(binary),'runtime_ready':False}
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(result))
if __name__=='__main__':main()
