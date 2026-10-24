from pathlib import Path
import importlib.util,json,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('manifest',ROOT/'tools/bundle_manifest.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class PublicManifest(unittest.TestCase):
 def test_machine_paths_commands_and_stale_aliases_are_dropped(self):
  with tempfile.TemporaryDirectory() as folder:
   root=Path(folder);helper=root/'FLAppBridge.dylib';helper.write_bytes(b'signed-helper-bytes')
   raw={'schema':'rysup.fl-ara-helper/1','binary':helper.name,'runtime_ready':True,'sha256':'old','helper_sha256':'stale','helper_path':'/Users/private/secret','commands':['/Volumes/private/compiler'],'source_revision':'a'*40,'normal_app_integration_qualified':False,'third_party_qualified':False,'inputs':[{'path':'/Users/private/code'}]}
   value=m.sanitized_manifest(raw,helper,root);text=json.dumps(value)
   self.assertNotIn('/Users',text);self.assertNotIn('/Volumes',text);self.assertNotIn('commands',value);self.assertNotIn('helper_sha256',value);self.assertNotIn('helper_path',value)
   self.assertFalse(value['normal_app_integration_qualified']);self.assertFalse(value['third_party_qualified']);self.assertFalse(value['runtime_qualified']);self.assertNotEqual(value['sha256'],'old')
if __name__=='__main__':unittest.main()
