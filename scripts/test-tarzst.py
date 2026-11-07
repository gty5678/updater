#!/usr/bin/env python3
"""Test TAR link preservation and rejection with the real updater (requires zstandard)."""
import io
import pathlib
import subprocess
import sys
import tarfile
import tempfile

import zstandard

exe = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='updater-tarzst-') as temporary:
    root = pathlib.Path(temporary)
    cases = [
        ('relative', [('Framework/Current', 'A')], True),
        ('pax-long-link', [('alias', 'Framework/' + 'a' * 120)], True),
        ('absolute', [('alias', '/tmp')], False),
        ('escape', [('alias', '../outside')], False),
        ('write-through', [('Framework', 'real')], False),
        ('hardlink', [('alias', 'Framework/A/program')], False),
    ]
    for name, links, success in cases:
        app = root / name
        app.mkdir()
        (app / 'old.txt').write_text('old')
        (app / 'data').mkdir()
        (app / 'data/user.txt').write_text('user data')
        content = io.BytesIO()
        with tarfile.open(fileobj=content, mode='w', format=tarfile.PAX_FORMAT) as tar:
            # Deliberately put links before their targets to test deferred creation.
            for path, target in links:
                info = tarfile.TarInfo(path)
                info.type = tarfile.LNKTYPE if name == 'hardlink' else tarfile.SYMTYPE
                info.linkname = target
                tar.addfile(info)
            for path in ('Framework/A/program', 'Framework/' + 'a' * 120):
                info = tarfile.TarInfo(path)
                info.mode = 0o755
                info.size = 7
                tar.addfile(info, io.BytesIO(b'program'))
        archive = root / (name + '.tar.zst')
        archive.write_bytes(zstandard.ZstdCompressor().compress(content.getvalue()))
        result = subprocess.run([str(exe), '--install-dir', str(app), '--local-package', str(archive),
                                 '--payload-root', '.', '--keep', 'data', '--no-start-main'],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == success, (name, result.stdout, result.stderr)
        assert (app / 'data/user.txt').read_text() == 'user data'
        if success:
            link, target = links[0]
            assert (app / link).is_symlink()
            assert (app / link).readlink() == pathlib.Path(target)
            assert (app / 'Framework/A/program').stat().st_mode & 0o111
            assert (app / 'old.txt').read_text() == 'old'
        else:
            assert (app / 'old.txt').read_text() == 'old'
        print(name, 'OK')

# Both standard and portable layouts replace only the .app; sibling data stays.
with tempfile.TemporaryDirectory(prefix='updater-bundle-') as temporary:
    root = pathlib.Path(temporary)
    content = io.BytesIO()
    with tarfile.open(fileobj=content, mode='w') as tar:
        for path, data in [('Demo.app/Contents/Info.plist', b'plist'),
                           ('Demo.app/Contents/MacOS/Demo', b'program')]:
            info = tarfile.TarInfo(path)
            info.size = len(data)
            info.mode = 0o755
            tar.addfile(info, io.BytesIO(data))
    archive = root / 'bundle.tar.zst'
    archive.write_bytes(zstandard.ZstdCompressor().compress(content.getvalue()))
    for layout in ('Applications', 'portable'):
        parent = root / layout
        app = parent / 'Demo.app'
        (app / 'Contents').mkdir(parents=True)
        (app / 'Contents/obsolete').write_text('obsolete')
        (parent / 'data').mkdir()
        (parent / 'data/user.txt').write_text('user data')
        (parent / 'portable.ini').write_text('portable config')
        result = subprocess.run([str(exe), '--install-dir', str(app), '--local-package', str(archive),
                                 '--payload-root', 'Demo.app', '--no-start-main'], capture_output=True, text=True)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert not (app / 'Contents/obsolete').exists()
        assert (app / 'Contents/MacOS/Demo').read_bytes() == b'program'
        assert (parent / 'data/user.txt').read_text() == 'user data'
        assert (parent / 'portable.ini').read_text() == 'portable config'
        assert {p.name for p in app.iterdir()} == {'Contents'}
        print(layout, 'bundle replacement OK')
    app = root / 'rollback/Demo.app'
    (app / 'Contents').mkdir(parents=True)
    (app / 'Contents/old.txt').write_text('original')
    result = subprocess.run([str(exe), '--install-dir', str(app), '--local-package', str(archive),
                             '--payload-root', 'Demo.app', '--main-exe', 'Contents/MacOS/Demo'],
                            capture_output=True, text=True)
    assert result.returncode == 20, (result.returncode, result.stdout, result.stderr)
    assert (app / 'Contents/old.txt').read_text() == 'original'
    assert not (app / 'Contents/MacOS/Demo').exists()
    print('bundle rollback OK')
