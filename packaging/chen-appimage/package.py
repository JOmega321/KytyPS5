#!/usr/bin/env python3
"""Stage a native installation and bundle its non-driver ELF dependencies."""
import os
import hashlib
import pathlib
import re
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
build = pathlib.Path(sys.argv[1]).resolve()
appdir = pathlib.Path(sys.argv[2]).resolve()
payload = appdir / 'usr/payload'
payload.mkdir(parents=True)
subprocess.run(['cmake', '-DCMAKE_INSTALL_PREFIX=' + str(payload),
                '-DCMAKE_INSTALL_LOCAL_ONLY=1', '-P', str(build / 'cmake_install.cmake')], check=True)
for name in ('kyty_launcher', 'kyty_shader_precompile'):
    shutil.copy2(build / name, payload / name)
for source, target in [('tools/local/run_ds.sh', 'run_ds.sh'), ('precompile-linux.sh', 'precompile-linux.sh'),
                       ('tools/local/dlss-bridge/get-dlss-linux.sh', 'get-dlss-linux.sh')]:
    shutil.copy2(root / source, payload / target)
    (payload / target).chmod(0o755)
bridge = payload / 'dlss-bridge'
bridge.mkdir()
for source in ('tools/local/dlss-bridge/kyty_dlss_bridge.cpp', 'tools/local/dlss-bridge/LICENSE.txt', 'src/local/kyty-dlss-bridge.h'):
    shutil.copy2(root / source, bridge)
static = payload / 'tools/local/static-precompile'
static.mkdir(parents=True)
for source in (root / 'tools/local/static-precompile').glob('*'):
    if source.name == 'pass-states.json' or source.suffix == '.hints':
        shutil.copy2(source, static)
shutil.copytree(root / 'tools/fsr4cap', payload / 'tools/fsr4cap')
# Prebuilt preparation tools, isolated from the emulator and the host's Python environment.
capture_build = root / '_Build/fsr-capture-tools'
capture = payload / 'tools/fsr4cap/prebuilt'
shutil.copytree(capture_build / 'bin', capture / 'bin')
for name in ('source-revision.txt', 'source-submodules.txt', 'tool-sha256.txt', 'dxil-spirv-source.tar.gz'):
    shutil.copy2(capture_build / name, capture / name)
python_version = f'python{sys.version_info.major}.{sys.version_info.minor}'
python_home = capture / 'python'
(python_home / 'bin').mkdir(parents=True)
shutil.copy2(pathlib.Path(sys.executable).resolve(), python_home / 'bin' / python_version)
stdlib = pathlib.Path('/usr/lib') / python_version
shutil.copytree(stdlib, python_home / 'lib' / python_version,
                ignore=shutil.ignore_patterns('__pycache__', 'test', 'tests', 'idlelib', 'tkinter', 'ensurepip', 'sitecustomize.py'))
wrapper = capture / 'bin/python3'
wrapper.write_text('#!/usr/bin/env bash\nset -euo pipefail\n'
    'here="$(cd -- "$(dirname -- "$0")" && pwd)"\n'
    'unset PYTHONPATH PYTHONSTARTUP\nexport PYTHONHOME="$here/../python"\n'
    f'exec "$PYTHONHOME/bin/{python_version}" -s "$@"\n')
wrapper.chmod(0o755)
# Notices for the interpreter, standard library, SPIR-V tools, and util-linux helper.
for name in ('python3.12', 'python3.12-minimal', 'libpython3.12-stdlib', 'libpython3.12-minimal',
             'spirv-tools', 'util-linux', 'gcc-mingw-w64-x86-64', 'mingw-w64-common'):
    notice = pathlib.Path('/usr/share/doc') / name / 'copyright'
    if notice.exists():
        target = payload / 'LICENSES/bundled-tools' / (name + '-copyright.txt')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(notice, target)

shutil.copy2(root / 'LICENSE', payload)
shutil.copytree(root / 'LICENSES', payload / 'LICENSES', dirs_exist_ok=True)
for base in (root / '3rdparty', build / '_deps'):
    for source in base.rglob('*'):
        if (source.is_file() and source.stat().st_size < 1024 * 1024
                and re.match(r'^(licen[cs]e|copying|copyright|notice)([._-]|$)', source.name, re.I)
                and '.git' not in source.parts and not any(p.endswith('-build') for p in source.parts)):
            target = payload / 'LICENSES/thirdparty' / source.relative_to(base)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
version = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
(payload / 'version').write_text(version + '\n')
(payload / 'build-manifest.txt').write_text(
    subprocess.check_output(['git', 'submodule', 'status', '--recursive'], cwd=root, text=True)
    + '\n' + (build / 'CMakeCache.txt').read_text())
with (payload / 'build-manifest.txt').open('a') as manifest:
    manifest.write('\nEmbedded native version metadata:\n' + (build / 'kytyGitVersion.h').read_text())
    for command in (['clang', '--version'], ['cmake', '--version'], ['ldd', '--version']):
        manifest.write('\n' + subprocess.check_output(command, text=True))
    for dependency in sorted((build / '_deps').glob('*-src')):
        revision = subprocess.check_output(['git', '-C', str(dependency), 'rev-parse', 'HEAD'], text=True)
        manifest.write(dependency.name + ': ' + revision)
    for archive in sorted((build / 'externals').glob('*.zip')):
        manifest.write(archive.name + ': sha256 ' + hashlib.sha256(archive.read_bytes()).hexdigest() + '\n')
libdir = payload / 'lib'
libdir.mkdir(exist_ok=True)
host = re.compile(r'^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util)\.so|lib(GL|EGL|GLX|GLdispatch|OpenGL|vulkan|drm|gbm))')
def is_elf(path):
    return path.is_file() and path.open('rb').read(4) == b'\x7fELF'

pending = [path for path in payload.rglob('*') if is_elf(path)]
seen = set()
while pending:
    elf = pending.pop()
    result = subprocess.check_output(['ldd', str(elf)], text=True)
    if 'not found' in result:
        raise RuntimeError(result)
    for line in result.splitlines():
        match = re.match(r'\s*(\S+) => (/\S+)', line)
        if not match:
            continue
        name, source = match.groups()
        if name in seen or host.match(name):
            continue
        seen.add(name)
        target = libdir / name
        shutil.copy2(source, target)
        for candidate in (source, os.path.realpath(source)):
            owner = subprocess.run(['dpkg-query', '-S', candidate], text=True, capture_output=True)
            if owner.returncode == 0:
                package = owner.stdout.split(': ', 1)[0].split(':', 1)[0]
                notice = pathlib.Path('/usr/share/doc') / package / 'copyright'
                if notice.exists():
                    shutil.copy2(notice, payload / 'LICENSES' / (package + '-copyright.txt'))
                    break
        pending.append(target)
for elf in (path for path in payload.rglob('*') if is_elf(path)):
    rpath = '$ORIGIN/' + os.path.relpath(libdir, elf.parent)
    subprocess.run(['patchelf', '--set-rpath', rpath, str(elf)], check=True)
    subprocess.run(['strip', '--strip-unneeded', str(elf)], check=True)
for source in ('AppRun', 'kytyps5.desktop', 'kytyps5.svg'):
    shutil.copy2(root / 'packaging/chen-appimage' / source, appdir / source)
(appdir / 'AppRun').chmod(0o755)
subprocess.run(['desktop-file-validate', str(appdir / 'kytyps5.desktop')], check=True)
