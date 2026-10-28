#!/usr/bin/env python3
"""Only reuse a checksummed successful AppImage when every native build input is unchanged."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import zipfile

base = 'fd5443c1f5010f624120776c9b5f2edd5b519cf2'
allowed = {
    '.github/workflows/chen-appimage.yml', 'docs/FSR411-EXPERIMENTAL.md',
    'packaging/chen-appimage/AppRun', 'packaging/chen-appimage/package.py',
    'packaging/chen-appimage/build-capture-tools.sh', 'packaging/chen-appimage/reuse-native.py',
    'tools/fsr4cap/build_assets.sh', 'tools/fsr4cap/check-tools.sh',
}
subprocess.run(['git', 'fetch', '--no-tags', '--depth=1', 'origin', base], check=True)
changed = subprocess.check_output(['git', 'diff', '--name-only', base, 'HEAD'], text=True).splitlines()
if any(path not in allowed for path in changed):
    print('Native inputs changed: compiling a fresh native build.')
    raise SystemExit(0)
# The full-build recipe remains part of the fingerprint, even when this run skips it.
def native_recipe(text):
    a = text.index('      - name: Configure and build native programs')
    b = text.index('      - name: Preserve native build before packaging', a)
    return '\n'.join(line for line in text[a:b].splitlines() if not line.strip().startswith('if:'))
prior = subprocess.check_output(['git', 'show', base + ':.github/workflows/chen-appimage.yml'], text=True)
if native_recipe(prior) != native_recipe(Path('.github/workflows/chen-appimage.yml').read_text()):
    print('Native recipe changed: compiling a fresh native build.')
    raise SystemExit(0)
archive = Path('_Build/native-input.zip')
archive.parent.mkdir(exist_ok=True)
download = subprocess.run(['curl', '--fail', '--location', '--retry', '3',
                '-H', 'Authorization: Bearer ' + os.environ['GH_TOKEN'],
                '-o', str(archive),
                'https://api.github.com/repos/JOmega321/KytyPS5/actions/artifacts/12797653243/zip'])
if download.returncode:
    print('Native input unavailable: compiling a fresh native build.')
    raise SystemExit(0)
digest = 'fb720fa1ca36bc0e6c1dcc47853a5a1c8d8efa2349035e8023828b5791b45b48'
assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest, 'Native input digest mismatch'
with zipfile.ZipFile(archive) as z:
    name = 'KytyPS5-chenxiao07-fd5443c1-x86_64.AppImage'
    data = z.read(name)
    assert hashlib.sha256(data).hexdigest() == z.read('SHA256SUMS').decode().split()[0]
    image = Path('_Build/native-input.AppImage')
    image.write_bytes(data)
image.chmod(0o755)
subprocess.run([str(image.resolve()), '--appimage-extract'], cwd='_Build', check=True, stdout=subprocess.DEVNULL)
Path('_Build/squashfs-root').rename('_Build/KytyPS5.AppDir')
manifest = {'compiled_source': base, 'native_artifact': 12797653243,
            'successful_run': 40645568266, 'archive_sha256': digest,
            'packaging_source': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
            'native_source_and_recipe_match': True}
Path('_Build/KytyPS5.AppDir/usr/payload/native-reuse.json').write_text(json.dumps(manifest, indent=2))
with open(os.environ['GITHUB_ENV'], 'a') as output:
    output.write('KYTY_REUSE_NATIVE=1\n')
print('Verified identical native source, submodules and build recipe; retained original compiled-source manifest.')
