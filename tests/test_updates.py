"""End-to-end updater checks using disposable applications and a loopback server."""
import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import threading
import unittest
import zipfile


REPO = Path(__file__).resolve().parents[1]
DEFAULT_UPDATER = REPO / 'dist/windows-x64/updater.exe'
UPDATER = Path(os.environ.get('UPDATER_EXE', DEFAULT_UPDATER)).resolve()


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class UpdateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not UPDATER.is_file():
            raise RuntimeError(f'Build updater first or set UPDATER_EXE: {UPDATER}')

    def setUp(self):
        test_root = REPO / 'build' / 'integration-tests'
        test_root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='run-', dir=test_root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'demo app'
        self.app.mkdir()
        self.write(self.app, {'version.txt': '1.0.0', 'obsolete.txt': 'remove me',
                              'data/user.txt': 'user data', 'settings.json': 'user settings'})
        self.before = self.snapshot()

    @staticmethod
    def write(root, files):
        for name, content in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding='utf-8')

    def snapshot(self):
        return {p.relative_to(self.app).as_posix(): p.read_bytes()
                for p in self.app.rglob('*') if p.is_file()}

    def package(self, files=None):
        path = self.root / 'release.zip'
        if files is None:
            files = {'version.txt': '2.0.0', 'assets/new.txt': 'new asset'}
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name, content in files.items():
                archive.writestr(name, content)
        return path

    def run_update(self, *args, code=0, clean=True):
        result = subprocess.run([str(UPDATER), '--install-dir', str(self.app),
                                 '--no-start-main', *map(str, args)],
                                capture_output=True, timeout=20)
        output = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        self.assertEqual(result.returncode, code, output)
        if clean:
            for name in ('.update-lock', '.updater', '.updater_backup'):
                self.assertFalse((self.app / name).exists(), f'{name} remains\n{output}')
        return output

    def serve(self):
        handler = functools.partial(QuietHandler, directory=str(self.root))
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        def stop():
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
        self.addCleanup(stop)
        return f'http://127.0.0.1:{server.server_port}'

    def manifest(self, version='2.0.0', bad_hash=False):
        package = self.package()
        base = self.serve()
        system = {'Windows': 'windows', 'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]
        arch = 'arm64' if platform.machine().lower() in ('arm64', 'aarch64') else 'x64'
        digest = '1' * 64 if bad_hash else hashlib.sha256(package.read_bytes()).hexdigest()
        (self.root / 'latest.json').write_text(json.dumps({
            'latestVersion': version,
            'packages': {f'{system}-{arch}': {'url': base + '/release.zip', 'sha256': digest}}
        }), encoding='utf-8')
        return ('--latest-json-url', base + '/latest.json', '--current-version', '1.0.0',
                '--download-retries', '1')

    def test_local_full_replacement(self):
        self.run_update('--local-package', self.package())
        self.assertEqual(self.snapshot(), {'version.txt': b'2.0.0', 'assets/new.txt': b'new asset'})

    def test_keep_user_data_over_package_defaults(self):
        package = self.package({'version.txt': '2.0.0', 'data/user.txt': 'default',
                                'settings.json': 'default'})
        self.run_update('--local-package', package, '--keep', 'data', '--keep', 'settings.json')
        self.assertEqual(self.snapshot(), {'version.txt': b'2.0.0',
                                          'data/user.txt': b'user data',
                                          'settings.json': b'user settings'})

    def test_auto_wrapper_directory(self):
        self.run_update('--local-package', self.package({'release/version.txt': '2.0.0'}))
        self.assertEqual(self.snapshot(), {'version.txt': b'2.0.0'})

    def test_explicit_root_preserves_single_directory(self):
        self.run_update('--local-package', self.package({'assets/new.txt': 'asset'}),
                        '--payload-root', '.')
        self.assertEqual(self.snapshot(), {'assets/new.txt': b'asset'})

    def test_online_update_with_sha256(self):
        self.run_update(*self.manifest())
        self.assertEqual(self.snapshot(), {'version.txt': b'2.0.0', 'assets/new.txt': b'new asset'})

    def test_same_version_skips_update(self):
        output = self.run_update(*self.manifest(version='1.0.0'))
        self.assertIn('No update needed', output)
        self.assertEqual(self.snapshot(), self.before)

    def test_hash_mismatch_preserves_installation(self):
        self.run_update(*self.manifest(bad_hash=True), code=3)
        self.assertEqual(self.snapshot(), self.before)

    def test_missing_local_package(self):
        self.run_update('--local-package', self.root / 'missing.zip', code=11)
        self.assertEqual(self.snapshot(), self.before)

    def test_corrupt_archive_preserves_installation(self):
        package = self.root / 'broken.zip'
        package.write_bytes(b'not a zip')
        output = self.run_update('--local-package', package, code=1)
        self.assertIn('Update failed:', output)
        self.assertEqual(self.snapshot(), self.before)

    def test_archive_traversal_rejected(self):
        output = self.run_update('--local-package', self.package({'../escaped.txt': 'escape'}), code=1)
        self.assertIn('Update failed:', output)
        self.assertFalse((self.root / 'escaped.txt').exists())
        self.assertEqual(self.snapshot(), self.before)

    def test_launch_failure_rolls_back(self):
        package = self.package({'version.txt': '2.0.0', 'broken.exe': 'invalid executable'})
        self.run_update('--local-package', package, '--start-main', '--main-exe', 'broken.exe', code=20)
        self.assertEqual(self.snapshot(), self.before)

    def test_existing_lock_is_respected(self):
        lock = self.app / '.update-lock'
        lock.mkdir()
        output = self.run_update('--local-package', self.package(), code=1, clean=False)
        self.assertIn('Installation is locked:', output)
        self.assertTrue(lock.is_dir())
        self.assertFalse((self.app / '.updater').exists())
        self.assertEqual(self.snapshot(), self.before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
