#!/usr/bin/env python3
"""Check the metadata Gear Lever reads and the packaged SDL window's identity."""
import configparser
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

NAME = "Demon's Souls (KyTy)"
ID = 'demons-souls-kyty'


def main():
    root = Path(sys.argv[1]).resolve()
    desktop = root / (ID + '.desktop')
    assert list(root.glob('*.desktop')) == [desktop]
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(desktop)
    entry = parser['Desktop Entry']
    assert entry['Name'] == entry['X-AppImage-Name'] == NAME
    assert entry['Icon'] == entry['StartupWMClass'] == ID
    assert entry['X-AppImage-Version'] == (root / 'usr/payload/version').read_text().strip()[:8]
    icon = root / (ID + '.png')
    png = icon.read_bytes()
    assert png[:8] == b'\x89PNG\r\n\x1a\n'
    assert struct.unpack('>II', png[16:24]) == (512, 512)
    assert (root / '.DirIcon').resolve() == icon
    for installed in (root / 'usr/payload' / icon.name,
                      root / 'usr/share/icons/hicolor/512x512/apps' / icon.name):
        assert installed.read_bytes() == png
    assert (root / 'usr/share/applications' / desktop.name).read_bytes() == desktop.read_bytes()
    print('Gear Lever metadata and embedded icon: OK; sha256=' + hashlib.sha256(png).hexdigest(), flush=True)

    with tempfile.TemporaryDirectory(prefix='kyty branding ') as temporary:
        home = Path(temporary)
        env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(home / 'data'),
                   XDG_CONFIG_HOME=str(home / 'config'), XDG_CACHE_HOME=str(home / 'cache'),
                   KYTY_APPIMAGE_DATA_DIR=str(home / 'application data'), SDL_VIDEODRIVER='x11')
        with (home / 'launcher.log').open('w+') as log:
            process = subprocess.Popen([str(root / 'AppRun')], env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    # SDL's accelerated renderer can recreate the native window during
                    # startup. Query all properties together and retry if it disappears.
                    query = subprocess.run(['xprop', '-name', NAME, '-len', '64', '-f',
                                            '_NET_WM_ICON', '32c', 'WM_NAME', 'WM_CLASS',
                                            '_NET_WM_ICON'],
                                           capture_output=True, text=True)
                    if process.poll() is not None:
                        log.seek(0)
                        raise AssertionError('Launcher exited: ' + log.read())
                    if (query.returncode == 0 and NAME in query.stdout
                            and ID in query.stdout and '512, 512' in query.stdout):
                        break
                    time.sleep(0.1)
                else:
                    log.seek(0)
                    raise AssertionError('Branded window properties did not settle: '
                                         + query.stdout + query.stderr + '\n' + log.read())
                print('Packaged window title, desktop identity and 512px window icon: OK', flush=True)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
