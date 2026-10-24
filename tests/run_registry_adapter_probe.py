#!/usr/bin/env python3
"""Run actual-vendor callback routing against synthetic registry data only.

The child cannot read the real registry and cannot write outside its UUID run.
This does not execute FL, plugins, audio, or any vendor licensing validator.
"""
import argparse,hashlib,json,os,subprocess,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--binary',type=Path,required=True);p.add_argument('--bridge',type=Path,default=Path('/Applications/FL Studio 2026.app/Contents/Resources/FL/Shared/libwinbridge_x64.dylib'));a=p.parse_args()
assert hashlib.sha256(a.bridge.read_bytes()).hexdigest()=='433f8dc07e2fab6b1786c7963f7fabe01d86bce75582019e4de0c82dfff5c8f2'
run=a.output_root.resolve()/('registry-adapter-'+str(uuid.uuid4()));run.mkdir(mode=0o700,parents=True);home=run/'home';prefs=home/'Library/Preferences/Image-Line';prefs.mkdir(parents=True);tmp=run/'tmp';tmp.mkdir()
private='''<?xml version="1.0"?><XMLReg><Key Name="Key1"><Key Name="HKEY_CURRENT_USER"><Key Name="Software"><Key Name="Image-Line"><Key Name="Shared"><Key Name="Paths"><Value Name="Private" Type="2">PRIVATE_ONLY</Value></Key></Key></Key></Key></Key></Key></XMLReg>'''
auth='''<?xml version="1.0"?><XMLReg><Key Name="Key1"><Key Name="HKEY_CURRENT_USER"><Key Name="Software"><Key Name="Image-Line"><Key Name="Registrations"><Key Name="SyntheticOnly"><Value Name="Ascii" Type="2">abcXYZ</Value><Value Name="Name😀" Type="2">A😀Z</Value><Value Name="Binary" Type="3">00017f80feff</Value><Value Name="Number" Type="1">305419896</Value><Value Name="OddHex" Type="3">0a1</Value><Value Name="Empty" Type="2"></Value><Value Name="MalformedHex" Type="3">GG</Value><Value Name="Unsupported" Type="5">1</Value><Value Name="textualvalue:Literal" Type="2">SYNTHETIC_AUTH_CANARY_ONLY</Value><Value Name="Spare" Type="2">No auth validator is run</Value></Key></Key></Key></Key></Key></Key></XMLReg>'''
(prefs/'reg.xml').write_text(private);source=run/'synthetic-original.xml';source.write_text(auth);source.chmod(0o600);before=hashlib.sha256(source.read_bytes()).hexdigest()
def literal(path):return json.dumps(str(Path(path).resolve()))
real_registry=Path.home()/'Library/Preferences/Image-Line/reg.xml'
profile='(version 1)(allow default)(deny file-write*)(allow file-write* (subpath '+literal(run)+'))(deny file-read* (literal '+literal(real_registry)+'))'
(run/'child.sb').write_text(profile)
env={k:v for k,v in os.environ.items() if not k.startswith(('DYLD_','RYSUP_'))};env.update(HOME=str(home),CFFIXED_USER_HOME=str(home),TMPDIR=str(tmp)+'/')
command=['/usr/bin/sandbox-exec','-f',str(run/'child.sb'),str(a.binary.resolve()),str(a.bridge.resolve()),str(source)]
r=subprocess.run(command,env=env,capture_output=True,text=True,timeout=60);(run/'stdout.txt').write_text(r.stdout);(run/'stderr.txt').write_text(r.stderr)
private_after=(prefs/'reg.xml').read_bytes();preserved=hashlib.sha256(source.read_bytes()).hexdigest()==before
receipt={'schema':'rysup.registry-read-through-synthetic-probe/1','run':str(run),'child_exit':r.returncode,'source_fixture_preserved':preserved,'source_canary_absent_from_private_registry':b'SYNTHETIC_AUTH_CANARY_ONLY' not in private_after,'denied_write_canary_absent':b'MUST_NOT_PERSIST' not in private_after,'ordinary_private_write_persisted':b'PRIVATE_WRITE_ALLOWED' in private_after,'real_registry_read_blocked_by_profile':True,'vendor_bridge_sha256':hashlib.sha256(a.bridge.read_bytes()).hexdigest(),'probe_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest(),'adapter_source_sha256':hashlib.sha256((ROOT/'native/bridge-support/RegistrationReadThrough.mm').read_bytes()).hexdigest(),'command':command,'stdout':r.stdout,'stderr':r.stderr,'daW_or_plugin_or_audio_started':False}
(run/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
assert r.returncode==0 and preserved and receipt['source_canary_absent_from_private_registry'] and receipt['denied_write_canary_absent'] and receipt['ordinary_private_write_persisted']
