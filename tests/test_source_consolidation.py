from pathlib import Path
import importlib.util,json,sys,tempfile,unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('stage',ROOT/'tools/stage_bridge_source.py');stage=importlib.util.module_from_spec(spec);spec.loader.exec_module(stage)
class SourceFreeze(unittest.TestCase):
 def fixture(self,base):
  source=base/'research';legacy=base/'legacy';(source/'host').mkdir(parents=True);(source/'native').mkdir();legacy.mkdir()
  (source/'host/FLStandaloneGraphDecoder.h').write_text('#include "../../../../FL Studio ARA/2026-10-23/host/FLNativeModel.h"\n')
  (source/'native/build_helper.py').write_text("old = here.parents[3] / 'FL Studio ARA/2026-10-23/host'\n")
  for n in ['FLNativeModel.h','FLNativeModel.cpp']:(legacy/n).write_text('original source\n')
  (source/'native/ignore.dylib').write_bytes(b'not eligible')
  return source,legacy
 def test_plan_does_not_copy_and_excludes_binaries(self):
  with tempfile.TemporaryDirectory() as folder:
   base=Path(folder);source,legacy=self.fixture(base);plan=stage.plan(source,legacy)
   self.assertEqual(len(plan['files']),4);self.assertFalse((ROOT/'native/bridge').exists())
   self.assertTrue(all(not row['destination'].endswith('.dylib') for row in plan['files']))
   self.assertEqual(sum(row['transform'] is not None for row in plan['files']),2)
 def test_changed_frozen_source_refuses_before_writes(self):
  with tempfile.TemporaryDirectory() as folder:
   base=Path(folder);source,legacy=self.fixture(base);plan=base/'plan.json';plan.write_text(json.dumps(stage.plan(source,legacy)))
   (legacy/'FLNativeModel.cpp').write_text('changed after review\n')
   with patch.object(stage,'ROOT',base/'package'),patch.object(sys,'argv',['stage','--source-root',str(source),'--legacy-host',str(legacy),'--apply',str(plan)]):
    with self.assertRaisesRegex(ValueError,'Sources changed'):stage.main()
   self.assertFalse((base/'package/native/bridge.staging').exists())
 def test_directory_source_symlink_is_rejected(self):
  with tempfile.TemporaryDirectory() as folder:
   base=Path(folder);source,legacy=self.fixture(base);(source/'host/link.h').symlink_to(legacy/'FLNativeModel.h')
   with self.assertRaisesRegex(ValueError,'symlinks'):stage.plan(source,legacy)
if __name__=='__main__':unittest.main()
