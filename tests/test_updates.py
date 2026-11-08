"""End-to-end updater checks using disposable applications and a loopback server."""
import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import tempfile
import threading
import time
import unittest
import zipfile


REPO = Path(__file__).resolve().parents[1]
DEFAULT_UPDATER = REPO / 'dist/windows-x64/updater.exe'
UPDATER = Path(os.environ.get('UPDATER_EXE', DEFAULT_UPDATER)).resolve()
TEST_ENV = {**os.environ, 'UPDATER_NO_PAUSE': '1'}


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
                                capture_output=True, timeout=20, env=TEST_ENV)
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

    def test_local_overlay_preserves_unpacked_files(self):
        self.run_update('--local-package', self.package())
        self.assertEqual(self.snapshot(), {**self.before, 'version.txt': b'2.0.0',
                                          'assets/new.txt': b'new asset'})

    def test_keep_user_data_over_package_defaults(self):
        package = self.package({'version.txt': '2.0.0', 'data/user.txt': 'default',
                                'settings.json': 'default'})
        self.run_update('--local-package', package, '--keep', 'data', '--keep', 'settings.json')
        self.assertEqual(self.snapshot(), {**self.before, 'version.txt': b'2.0.0',
                                          'data/user.txt': b'user data',
                                          'settings.json': b'user settings'})

    def test_auto_wrapper_directory(self):
        self.run_update('--local-package', self.package({'release/version.txt': '2.0.0'}))
        self.assertEqual(self.snapshot(), {**self.before, 'version.txt': b'2.0.0'})

    def test_explicit_root_preserves_single_directory(self):
        self.run_update('--local-package', self.package({'assets/new.txt': 'asset'}),
                        '--payload-root', '.')
        self.assertEqual(self.snapshot(), {**self.before, 'assets/new.txt': b'asset'})

    def test_online_update_with_sha256(self):
        self.run_update(*self.manifest())
        self.assertEqual(self.snapshot(), {**self.before, 'version.txt': b'2.0.0',
                                          'assets/new.txt': b'new asset'})

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

    def test_duplicate_does_not_interrupt_running_update(self):
        requested = threading.Event()
        release = threading.Event()
        package = self.package().read_bytes()

        class SlowHandler(QuietHandler):
            def do_GET(self):
                requested.set()
                release.wait(timeout=10)
                self.send_response(200)
                self.send_header('Content-Length', str(len(package)))
                self.end_headers()
                self.wfile.write(package)

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), SlowHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        manifest = self.root / 'slow.json'
        system = {'Windows': 'windows', 'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]
        arch = 'arm64' if platform.machine().lower() in ('arm64', 'aarch64') else 'x64'
        manifest.write_text(json.dumps({
            'latestVersion': '2.0.0',
            'packages': {f'{system}-{arch}': {
                'url': f'http://127.0.0.1:{server.server_port}/release.zip',
                'sha256': hashlib.sha256(package).hexdigest()}},
        }))
        # Serve the manifest normally; only the package download is blocked.
        base = self.serve()
        first = subprocess.Popen([str(UPDATER), '--install-dir', str(self.app),
            '--no-start-main', '--latest-json-url', base + '/slow.json',
            '--current-version', '1.0.0'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=TEST_ENV)
        try:
            self.assertTrue(requested.wait(timeout=5), 'First updater never began download')
            self.run_update('--local-package', self.root / 'release.zip', code=12, clean=False)
            self.assertIsNone(first.poll(), 'Duplicate stopped the original updater')
            self.assertTrue((self.app / '.update-lock').is_dir())
            release.set()
            output, _ = first.communicate(timeout=15)
            self.assertEqual(first.returncode, 0, output.decode(errors='replace'))
            self.assertEqual((self.app / 'version.txt').read_text(), '2.0.0')
            self.assertFalse((self.app / '.update-lock').exists())
        finally:
            release.set()
            if first.poll() is None:
                first.kill()
            first.communicate(timeout=5)
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_existing_lock_is_respected(self):
        lock = self.app / '.update-lock'
        lock.mkdir()
        output = self.run_update('--local-package', self.package(), code=12, clean=False)
        self.assertIn('安装目录已锁定', output)
        self.assertNotIn('Fatal error:', output)
        self.assertTrue(lock.is_dir())
        self.assertFalse((self.app / '.updater').exists())
        self.assertEqual(self.snapshot(), self.before)

    def check_resume(self, mode, restart=False):
        payload = self.package({'version.txt': '2.0.0', 'padding.bin': os.urandom(500000)}).read_bytes()
        half = len(payload) // 2
        requests = []
        system = {'Windows': 'windows', 'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]
        arch = 'arm64' if platform.machine().lower() in ('arm64', 'aarch64') else 'x64'

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                if self.path == '/latest':
                    body = json.dumps({'latestVersion': '2.0.0', 'packages': {
                        f'{system}-{arch}': {'url': base + '/package',
                            'sha256': hashlib.sha256(payload).hexdigest()}}}).encode()
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                requests.append((self.headers.get('Range'), self.headers.get('If-Range')))
                first = len(requests) == 1
                start = int(self.headers['Range'].split('=')[1].split('-')[0]) if self.headers.get('Range') else 0
                if mode == 'ignore' or (mode == 'changed' and not first):
                    start = 0
                status = 206 if start else 200
                if mode == 'complete' and start == len(payload):
                    self.send_response(416)
                    self.send_header('Content-Range', f'bytes */{len(payload)}')
                    self.end_headers()
                    return
                if mode == 'invalid' and not first:
                    status = 206
                self.send_response(status)
                self.send_header('ETag', '"v2"' if mode == 'changed' and not first else '"v1"')
                if status == 206:
                    range_start = start + 1 if mode == 'invalid' else start
                    self.send_header('Content-Range', f'bytes {range_start}-{len(payload)-1}/{len(payload)}')
                self.send_header('Content-Length', str(len(payload) - start))
                self.end_headers()
                try:
                    self.wfile.write(payload[:half] if first else payload[start:])
                    self.wfile.flush()
                    if first:
                        time.sleep(2 if mode == 'timeout' else 0.1)
                        try:
                            self.connection.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass  # The timeout test's client may already have closed its socket.
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        base = f'http://127.0.0.1:{server.server_port}'
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        args = ('--latest-json-url', base + '/latest', '--current-version', '1.0.0',
                '--payload-root', '.', '--package-timeout-seconds', '1', '--download-retries')
        try:
            if restart or mode in ('invalid', 'complete'):
                failed_output = self.run_update(*args, '1', code=1)
                self.assertEqual(self.snapshot(), self.before)
                if mode == 'complete':
                    cache_path = failed_output.split('Package download cache (retained on interruption): ')[1].splitlines()[0]
                    Path(cache_path).write_bytes(payload)
            if mode == 'invalid':
                self.run_update(*args, '1', code=1)
                self.assertEqual(self.snapshot(), self.before)
                return
            output = self.run_update(*args, '1' if restart else '2')
            offset = len(payload) if mode == 'complete' else half
            self.assertEqual(requests, [(None, None), (f'bytes={offset}-', '"v1"')])
            self.assertIn(f'Resuming package download from byte {offset}', output)
            self.assertEqual((self.app / 'version.txt').read_bytes(), b'2.0.0')
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_download_resumes_within_retries(self):
        self.check_resume('range')

    def test_download_resumes_after_restarting_updater(self):
        self.check_resume('range', restart=True)

    def test_server_ignoring_range_restarts_without_appending(self):
        self.check_resume('ignore')

    def test_changed_etag_restarts_without_appending(self):
        self.check_resume('changed')

    def test_invalid_content_range_never_installs(self):
        self.check_resume('invalid')

    def test_package_timeout_resumes_instead_of_restarting(self):
        self.check_resume('timeout')

    def test_complete_cached_package_handles_http_416(self):
        self.check_resume('complete', restart=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
