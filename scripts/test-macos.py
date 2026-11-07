#!/usr/bin/env python3
"""Exercise a packaged updater against disposable local applications."""
import pathlib, platform, sys, tempfile, zipfile, subprocess, time
arch = 'arm64' if platform.machine() == 'arm64' else 'x64'
repo = pathlib.Path(__file__).resolve().parents[1]
exe = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else repo / f'dist/macos-{arch}/updater'
with tempfile.TemporaryDirectory(prefix='updater macOS 测试 ') as tmp:
    root = pathlib.Path(tmp)
    app = root / '应用目录'
    app.mkdir()
    (app / 'old.txt').write_text('old')
    (app / 'portable.ini').write_text('[Portable]\nDataDirectory=../data\n')
    (app / 'nested').mkdir()
    (app / 'nested/custom.txt').write_text('custom')
    (app / 'data').mkdir()
    (app / 'data' / 'keep.txt').write_text('keep')
    package = root / '更新包.zip'
    with zipfile.ZipFile(package, 'w') as z:
        info = zipfile.ZipInfo('Demo.app/Contents/MacOS/Demo')
        info.create_system = 3
        info.external_attr = 0o100755 << 16
        z.writestr(info, '#!/bin/sh\nprintf "%s" "$1" > started.txt\n')
        z.writestr('version.txt', '2.0')
        z.writestr('nested/program.txt', 'new')
        z.writestr('portable.ini', 'must not replace user config')
        link = zipfile.ZipInfo('nested/current')
        link.create_system = 3
        link.external_attr = 0o120777 << 16
        z.writestr(link, 'program.txt')
    args = [str(exe), '--install-dir', str(app), '--local-package', str(package), '--payload-root', '.', '--keep', 'data,portable.ini']
    subprocess.run(args + ['--main-exe', 'Demo.app/Contents/MacOS/Demo', '--main-args', '"启动成功 空格"'], check=True)
    for _ in range(100):
        if (app / 'started.txt').exists(): break
        time.sleep(.05)
    assert (app / 'started.txt').read_text() == '启动成功 空格'
    assert (app / 'old.txt').read_text() == 'old'
    assert 'DataDirectory' in (app / 'portable.ini').read_text()
    assert (app / 'nested/custom.txt').read_text() == 'custom'
    assert (app / 'nested/current').is_symlink()
    assert (app / 'nested/current').read_text() == 'new'
    assert (app / 'data/keep.txt').read_text() == 'keep'
    assert (app / 'version.txt').read_text() == '2.0'
    # A non-executable entry must fail to launch and restore the previous app.
    with zipfile.ZipFile(package, 'w') as z:
        z.writestr('broken', 'not executable')
    result = subprocess.run(args + ['--main-exe', 'broken'])
    assert result.returncode == 20, result.returncode
    assert (app / 'version.txt').read_text() == '2.0'
    assert (app / 'data/keep.txt').read_text() == 'keep'
    assert not (app / 'broken').exists()
print('PASS: install, executable mode, app launch, Unicode/spaces, keep data, rollback')

# Exercise the native network backend through the real updater CLI.
import hashlib
import http.server
import io
import json
import threading

payload = io.BytesIO()
with zipfile.ZipFile(payload, 'w') as z:
    z.writestr('version.txt', '3.0')
package_bytes = payload.getvalue()
counts = {}

class UpdateServer(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        counts[self.path] = counts.get(self.path, 0) + 1
        assert self.headers.get('User-Agent') == 'Updater/1.0'
        if self.path == '/latest':
            body = json.dumps({'latestVersion': '3.0', 'packages': {
                f'macos-{arch}': {'url': base + '/redirect',
                                  'sha256': hashlib.sha256(package_bytes).hexdigest()}}}).encode()
        elif self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', base + '/payload')
            self.end_headers()
            return
        elif self.path == '/payload':
            if counts[self.path] == 1:
                self.send_response(503)
                self.end_headers()
                return
            body = package_bytes
        elif self.path == '/loop':
            self.send_response(302)
            self.send_header('Location', base + '/loop')
            self.end_headers()
            return
        elif self.path == '/slow':
            time.sleep(2)
            body = b'{}'
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), UpdateServer)
base = f'http://127.0.0.1:{server.server_port}'
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='updater-network-') as tmp:
        app = pathlib.Path(tmp) / 'app'
        app.mkdir()
        args = [str(exe), '--install-dir', str(app), '--current-version', '1.0',
                '--no-start-main', '--payload-root', '.']
        subprocess.run(args + ['--latest-json-url', base + '/latest'], check=True, timeout=15)
        assert (app / 'version.txt').read_text() == '3.0'
        assert counts['/payload'] == 2
        for route, message in [('/missing', 'HTTP 404'), ('/loop', 'redirect'), ('/slow', 'NSURLErrorDomain -1001')]:
            result = subprocess.run(args + ['--latest-json-url', base + route,
                '--download-retries', '1', '--download-timeout-seconds', '1'],
                capture_output=True, text=True, timeout=10)
            assert result.returncode != 0, route
            assert message in result.stdout, result.stdout
        assert (app / 'version.txt').read_text() == '3.0'
finally:
    server.shutdown()
    server.server_close()
print('PASS: native HTTP download, SHA256, progress, redirect, retry, HTTP errors, redirect limit, timeout')
