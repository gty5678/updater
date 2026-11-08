"""Verify visible Windows console logs alongside captured stdout."""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[1]

@unittest.skipUnless(os.name == 'nt', 'Windows console test')
class ConsoleTests(unittest.TestCase):
    def test_redirected_logs_are_visible(self):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.ReadConsoleOutputCharacterW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR,
                                                       wintypes.DWORD, wintypes.DWORD,
                                                       ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        exe = Path(os.environ.get('UPDATER_EXE', REPO / 'dist/windows-x64/updater.exe'))
        for hidden in (False, True):
            with self.subTest(hidden=hidden), tempfile.TemporaryDirectory(dir=REPO / 'build') as directory:
                root = Path(directory)
                app = root / 'app'
                app.mkdir()
                package = root / 'release.zip'
                with zipfile.ZipFile(package, 'w') as archive:
                    archive.writestr('version.txt', '2')
                info = subprocess.STARTUPINFO()
                info.dwFlags = subprocess.STARTF_USESHOWWINDOW
                info.wShowWindow = 0
                flags = subprocess.CREATE_NEW_CONSOLE if hidden else subprocess.CREATE_NO_WINDOW
                process = subprocess.Popen([str(exe), '--install-dir', str(app), '--local-package',
                                            str(package), '--no-start-main', '--pid', str(os.getpid()),
                                            '--wait-timeout-seconds', '5'],
                                           creationflags=flags, startupinfo=info,
                                           stdin=subprocess.DEVNULL,
                                           env={**os.environ, 'UPDATER_NO_PAUSE': '1'},
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                text = ''
                try:
                    deadline = time.monotonic() + 4
                    while time.monotonic() < deadline:
                        kernel.FreeConsole()
                        if kernel.AttachConsole(process.pid):
                            handle = kernel.CreateFileW('CONOUT$', 0x80000000, 3, None, 3, 0, None)
                            try:
                                buffer = ctypes.create_unicode_buffer(16000)
                                count = wintypes.DWORD()
                                if kernel.ReadConsoleOutputCharacterW(handle, buffer, 16000, 0, ctypes.byref(count)):
                                    text = buffer[:count.value]
                            finally:
                                kernel.CloseHandle(handle)
                                kernel.FreeConsole()
                            if 'Waiting for main PID' in text:
                                break
                        time.sleep(.05)
                finally:
                    output, error = process.communicate(timeout=10)
                self.assertIn('Waiting for main PID', text)
                self.assertIn('Local package mode:', text)
                self.assertIn(b'Waiting for main PID', output)
                self.assertIn(b'Local package mode:', output)

    def test_failure_waits_even_with_closed_stdin(self):
        exe = Path(os.environ.get('UPDATER_EXE', REPO / 'dist/windows-x64/updater.exe'))
        env = dict(os.environ)
        env.pop('UPDATER_NO_PAUSE', None)
        process = subprocess.Popen([str(exe), '--invalid-option'], env=env,
                                   creationflags=subprocess.CREATE_NO_WINDOW,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        try:
            time.sleep(2)
            self.assertIsNone(process.poll(), 'Error window exited before user dismissal')
        finally:
            if process.poll() is None:
                process.terminate()
            output, _ = process.communicate(timeout=10)
        self.assertIn(b'Fatal error:', output)
        self.assertIn('按 Enter 键关闭窗口'.encode(), output)

    def test_existing_lock_waits_and_is_preserved(self):
        exe = Path(os.environ.get('UPDATER_EXE', REPO / 'dist/windows-x64/updater.exe'))
        env = dict(os.environ)
        env.pop('UPDATER_NO_PAUSE', None)
        with tempfile.TemporaryDirectory(dir=REPO / 'build') as directory:
            app = Path(directory)
            lock = app / '.update-lock'
            lock.mkdir()
            process = subprocess.Popen([str(exe), '--install-dir', str(app),
                                        '--local-package', str(app / 'unused.zip'),
                                        '--no-start-main'], env=env,
                                       creationflags=subprocess.CREATE_NO_WINDOW,
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE)
            try:
                time.sleep(2)
                self.assertIsNone(process.poll(), 'Lock error window closed immediately')
                self.assertTrue(lock.is_dir())
            finally:
                if process.poll() is None:
                    process.terminate()
                output, _ = process.communicate(timeout=10)
            self.assertIn('退出码：12'.encode(), output)

if __name__ == '__main__':
    unittest.main(verbosity=2)
