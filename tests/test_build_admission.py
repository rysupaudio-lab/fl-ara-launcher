"""Build-boundary tests: never construct or launch an app with placeholder helpers."""
from pathlib import Path
import hashlib, json, subprocess, sys, tempfile, unittest
ROOT = Path(__file__).resolve().parents[1]

class BuildAdmission(unittest.TestCase):
    def test_missing_helper_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='fl-ara-build-test-') as folder:
            result = subprocess.run([sys.executable, str(ROOT/'tools/build_app.py'), '--output', folder], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Placeholder helpers cannot be packaged', result.stderr)
            self.assertFalse((Path(folder)/'FL ARA Launcher.app').exists())

    def test_placeholder_or_wrong_seal_is_rejected_before_signing(self):
        with tempfile.TemporaryDirectory(prefix='fl-ara-build-test-') as folder:
            base = Path(folder); helper = base/'FLAppBridge.dylib'; helper.write_bytes(b'not a runnable helper')
            manifest = base/'helper.json'
            for ready, seal in [(False, hashlib.sha256(helper.read_bytes()).hexdigest()), (True, '0'*64)]:
                manifest.write_text(json.dumps({'schema':'rysup.fl-ara-helper/1','runtime_ready':ready,'binary':helper.name,'sha256':seal}))
                result = subprocess.run([sys.executable, str(ROOT/'tools/build_app.py'), '--output', str(base/'output'), '--helper', str(helper), '--helper-manifest', str(manifest)], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('not ready or does not match', result.stderr)
                self.assertFalse((base/'output/FL ARA Launcher.app').exists())
                self.assertEqual(helper.read_bytes(), b'not a runnable helper')

if __name__ == '__main__': unittest.main()
