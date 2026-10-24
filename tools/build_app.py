#!/usr/bin/env python3
"""Build native Swift launcher; no vendor binaries or third-party licenses bundled."""
import argparse, hashlib, json, os, plistlib, shutil, subprocess
from pathlib import Path
from bundle_manifest import sanitized_manifest
root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, default=root / 'dist')
p.add_argument('--helper', type=Path, help='Root-qualified native FLAppBridge.dylib')
p.add_argument('--helper-manifest', type=Path, help='Qualification manifest issued by integration owner')
p.add_argument('--compile-only', action='store_true', help='Compile CLI for offline tests; no app or helper is produced')
a = p.parse_args(); out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
(out/'.tmp').mkdir(exist_ok=True)
build_env = dict(os.environ, TMPDIR=str(out/'.tmp') + '/')
if a.compile_only:
    entry = out / 'FL-ARA-Launcher-compilecheck'
    subprocess.run(['xcrun', 'swiftc', '-target', 'arm64-apple-macos15.4', '-O', '-module-cache-path', str(out/'.swift-module-cache'), str(root/'native/Launcher.swift'), '-o', str(entry)], check=True, env=build_env)
    print(entry); raise SystemExit(0)
if a.helper is None or a.helper_manifest is None:
    p.error('Both --helper and --helper-manifest are required. Placeholder helpers cannot be packaged.')
helper = a.helper.resolve(strict=True); manifest = json.loads(a.helper_manifest.read_text())
digest = hashlib.sha256(helper.read_bytes()).hexdigest()
if manifest.get('schema') != 'rysup.fl-ara-helper/1' or manifest.get('runtime_ready') is not True or manifest.get('binary') != 'FLAppBridge.dylib' or manifest.get('sha256') != digest:
    p.error('Helper manifest is not ready or does not match exact helper bytes')
subprocess.run(['/usr/bin/codesign','--verify','--strict',str(helper)],check=True)
app = out / 'FL ARA Launcher.app'
if app.exists(): shutil.rmtree(app)
resources=app/'Contents/Resources'; native=resources/'native'; macos=app/'Contents/MacOS'
native.mkdir(parents=True); macos.mkdir(parents=True)
shutil.copy2(helper,native/'FLAppBridge.dylib')
public_manifest=sanitized_manifest(manifest,native/'FLAppBridge.dylib',root)
(native/'helper.json').write_text(json.dumps(public_manifest,indent=2)+'\n')
(out/'app-build.json').write_text(json.dumps({'source_revision':public_manifest['source_revision'],'compiled_helper_sha256':digest,'public_manifest_sha256':hashlib.sha256((native/'helper.json').read_bytes()).hexdigest()},indent=2)+'\n')
entry=macos/'FL ARA Launcher'
subprocess.run(['xcrun','swiftc','-target','arm64-apple-macos15.4','-O','-module-cache-path',str(out/'.swift-module-cache'),str(root/'native/Launcher.swift'),'-o',str(entry)],check=True,env=build_env)
with (app/'Contents/Info.plist').open('wb') as stream:
    plistlib.dump({'CFBundleIdentifier':'com.rysupaudio.fl-ara-launcher','CFBundleName':'FL ARA Launcher','CFBundleExecutable':entry.name,'CFBundlePackageType':'APPL','CFBundleShortVersionString':'0.1.0','CFBundleVersion':'1','LSMinimumSystemVersion':'15.4','LSArchitecturePriority':['arm64'],'NSHighResolutionCapable':True,'NSHumanReadableCopyright':'Copyright 2026 Rys Up Audio LLC. MIT License. Independent of Image-Line.'},stream)
for name in ['LICENSE','README.md','THIRD_PARTY_NOTICES.md']: shutil.copy2(root/name, resources/name)
shutil.copytree(root/'third_party_licenses', resources/'ThirdPartyLicenses')
print(app)
