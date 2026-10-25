#!/usr/bin/env python3
"""Compile the normal-FL owned-copy ARA helper; never load a DAW or plugin.

SDK and output paths are explicit/environment-configurable so the source tree
can be staged on the M1/Razer without source-path edits or local disk fallbacks.
Compile capability is deliberately distinct from root-owned runtime qualification.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ara-sdk', default=os.environ.get('ARA_SDK_ROOT'))
    parser.add_argument('--vst3-sdk', default=os.environ.get('VST3_SDK_ROOT'))
    parser.add_argument('--native-model-root', '--legacy-host', default=os.environ.get('FL_NATIVE_MODEL_ROOT'))
    parser.add_argument('--output', default=os.environ.get('FL_APP_HELPER_OUTPUT'))
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'clang++'))
    parser.add_argument('--syntax-only', action='store_true')
    args = parser.parse_args()
    if not args.ara_sdk or not args.vst3_sdk or not args.output:
        parser.error('--ara-sdk, --vst3-sdk and --output are required (or their documented environment variables)')
    here = Path(__file__).resolve().parent
    host = here.parent / 'host'
    old = Path(args.native_model_root).resolve() if args.native_model_root else here.parent/'legacy'
    if not (old/'FLNativeModel.h').is_file():
        parser.error('--legacy-host must point to the new fl/legacy reader; no historical sealed-source fallback is allowed')
    ara, vst = Path(args.ara_sdk).resolve(), Path(args.vst3_sdk).resolve()
    required = [ara/'ARA_API/ARAInterface.h', vst/'pluginterfaces/base/funknown.h', old/'FLNativeModel.cpp', host/'StandaloneARACoordinator.cpp']
    for path in required:
        if not path.is_file() or path.stat().st_size < 64:
            parser.error(f'missing/nonmaterialized source or SDK: {path}')
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    # A failed rebuild cannot leave a previously qualified/stale manifest usable.
    (output/'helper.json').write_text(json.dumps({'schema':'rysup.fl-ara-helper/1',
        'runtime_ready':False,'binary':'FLAppBridge.dylib','sha256':None,
        'compile_passed':False,'status':'building'}, indent=2)+'\n')
    sources = [here/'FLAppBridge.mm', here/'FLNativeApplication.mm', here/'FLVST3NativeHooks.cpp', here/'FLStandaloneSourceOverlay.cpp', here/'FLNativeProcessObserver.cpp', here/'FLNativeMixBoundary.cpp', here/'RegistrationReadThrough.mm', here/'FLNativeSaveGuard.mm', here/'FLTestOutputSink.cpp', here/'FLUnitDeviceGate.cpp',
               host/'FLStandaloneGraphDecoder.cpp', host/'FLStandaloneStateEnvelope.cpp', host/'GenericARAHost.cpp', host/'VST3ARAEntry.cpp',
               host/'OwnedVST3PlaybackCompanion.cpp', host/'StandaloneARACoordinator.cpp', old/'FLNativeModel.cpp',
               ara/'ARA_Library/Dispatch/ARAHostDispatch.cpp', ara/'ARA_Library/Debug/ARADebug.c',
               vst/'pluginterfaces/base/funknown.cpp', vst/'pluginterfaces/base/coreiids.cpp']
    # Input seals are compiled in; no unsealed decoder or production-code load.
    decoder_sha = sha(host/'FLStandaloneGraphDecoder.cpp')
    common = ['-arch', 'arm64', '-O1', '-g0', '-fvisibility=hidden', '-mmacosx-version-min=12.0',
              '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-Wno-unused-parameter',
              '-DARA_SUPPORT_VERSION_1=0', '-DFL_APP_DECODER_SHA256="'+decoder_sha+'"',
              '-I'+str(here), '-I'+str(host), '-I'+str(old), '-I'+str(ara), '-I'+str(vst)]
    sealed_inputs = sources + sorted(here.glob('*.h')) + sorted(host.glob('*.h')) + [old/'FLNativeModel.h']
    input_seals = {str(path):sha(path) for path in sealed_inputs}
    commands, objects = [], []
    for index, source in enumerate(sources):
        if not source.is_file():
            parser.error(f'missing source: {source}')
        language = ['-std=c++17'] if source.suffix != '.c' else ['-std=c11', '-x', 'c']
        if source.suffix == '.mm':
            language += ['-fobjc-arc']
        if source.name == 'FLNativeApplication.mm':
            language += ['-fptrauth-intrinsics']
        obj = output / f'{index:02d}-{source.stem}.o'
        cmd = [args.compiler, *common, *language, '-fsyntax-only' if args.syntax_only else '-c', str(source)]
        if not args.syntax_only:
            cmd += ['-o', str(obj)]
        commands.append(cmd)
        result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            (output/'build-failure.txt').write_text(result.stdout)
            print(result.stdout, file=sys.stderr)
            return result.returncode
        objects.append(obj)
    binary = output/'FLAppBridge.dylib'
    candidate = output/'FLAppBridge.building.dylib'
    if not args.syntax_only:
        cmd = [args.compiler, '-arch', 'arm64', '-dynamiclib', '-mmacosx-version-min=12.0', '-Wl,-undefined,error', '-Wl,-install_name,@rpath/FLAppBridge.dylib',
               *map(str, objects), '-framework', 'CoreFoundation', '-framework', 'Foundation', '-framework', 'AppKit',
               '-framework', 'AudioToolbox', '-framework', 'CoreAudio', '-framework', 'Security', '-o', str(candidate)]
        commands.append(cmd)
        subprocess.run(cmd, check=True)
        # Owner-local helper only. Signing this helper does not alter vendor code.
        subprocess.run(['codesign', '--force', '--sign', '-', str(candidate)], check=True)
    if any(sha(Path(path)) != seal for path, seal in input_seals.items()):
        raise RuntimeError('Source/header inputs changed during compilation; manifest remains unqualified')
    if not args.syntax_only:
        candidate.replace(binary)
    receipt = {'schema':'rysup.fl-ara-helper/1', 'runtime_ready':False,
               'normal_app_integration_qualified':False, 'third_party_qualified':False,
               'compile_passed':True, 'syntax_only':args.syntax_only,
               'architectures':['arm64'], 'supported_fl_version':'26.1.7.5421',
               'binary':binary.name, 'sha256':sha(binary) if not args.syntax_only else None,
               'helper_path':str(binary) if not args.syntax_only else None,
               'helper_sha256':sha(binary) if not args.syntax_only else None,
               'decoder_sha256':decoder_sha,
               'inputs':[{'path':str(s),'sha256':input_seals[str(s)],'bytes':s.stat().st_size} for s in sealed_inputs],
               'commands':commands,
               'capabilities':{'actual_native_component_observation':False,
                   'generic_same_factory_ara_binding':False,'native_ram_source_decoder':False,
                   'ordinary_state_envelope':False,'native_source_cache_overlay':False,
                   'third_party_audio_null_tests':False,'cold_native_project_restore_tests':False,
                   'normal_playlist_edit_tests':False},
               'qualification_note':'Compile evidence only. Root must qualify exact binary, plugin and actual normal app runtime before runtime_ready can become true.'}
    (output/'helper.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps({'compile_passed':True, 'runtime_ready':False, 'manifest':str(output/'helper.json')}))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
