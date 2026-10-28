#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package a standalone native FP8 test and private preparation tools, without AMD data."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

repo=Path(__file__).resolve().parents[2]
native,tools,out=(Path(x).resolve() for x in sys.argv[1:])
out.mkdir(parents=True,exist_ok=False)
shutil.copy2(native/'native-frame-test',out/'native-frame-test')
for name in ('extract_vk.py','adapt_spirv.py','prune_bindings.py','legalize_fp8.py','recover_assets.py','host-int8-t1080-m0.json','run-fp8-test.sh','FP8-TEST.md'):
    shutil.copy2(repo/'tools/fsr412-vk'/name,out/name)
shutil.copytree(repo/'tools/fsr412-vk/recipes-fp8',out/'recipes-fp8')
prebuilt=out/'prebuilt';(prebuilt/'bin').mkdir(parents=True)
for name in ('spirv-opt','spirv-val'):shutil.copy2(tools/'build/tools'/name,prebuilt/'bin'/name)
licenses=out/'LICENSES';licenses.mkdir()
shutil.copy2(repo/'LICENSE',out/'LICENSE')
shutil.copy2(tools/'source/LICENSE',licenses/'SPIRV-Tools-LICENSE.txt')
shutil.copy2(tools/'source/external/spirv-headers/LICENSE',licenses/'SPIRV-Headers-LICENSE.txt')
version=f'python{sys.version_info.major}.{sys.version_info.minor}'
pyhome=prebuilt/'python';(pyhome/'bin').mkdir(parents=True)
shutil.copy2(Path(sys.executable).resolve(),pyhome/'bin'/version)
shutil.copytree(Path('/usr/lib')/version,pyhome/'lib'/version,ignore=shutil.ignore_patterns('__pycache__','test','tests','idlelib','tkinter','ensurepip','sitecustomize.py','config-*','*.a','*.o'))
wrapper=prebuilt/'bin/kyty-python3'
wrapper.write_text('#!/usr/bin/env bash\nset -euo pipefail\nhere="$(cd -- "$(dirname -- "$0")" && pwd)"\nunset PYTHONPATH PYTHONSTARTUP\nexport PYTHONHOME="$here/../python"\nexec "$PYTHONHOME/bin/'+version+'" -s "$@"\n')
wrapper.chmod(0o755)
source=out/'source';(source/'tools/fsr412-vk').mkdir(parents=True);(source/'src/local/fsr411').mkdir(parents=True)
for name in ('native_frame_test.cpp','build-frame-test.sh','build-fp8-tools.sh','package-fp8-test.py'):shutil.copy2(repo/'tools/fsr412-vk'/name,source/'tools/fsr412-vk'/name)
for name in ('fsr411.cpp','fsr411.h'):shutil.copy2(repo/'src/local/fsr411'/name,source/'src/local/fsr411'/name)
shutil.copy2(repo/'src/local/fsr4-formats.h',source/'src/local/fsr4-formats.h')
(out/'SOURCE_COMMIT').write_text(subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True))
(out/'TOOL_SOURCES').write_text('SPIRV-Tools 28f99f74a2a922ff85fc8fd966d6b3d6a45be795\nSPIRV-Headers 86f980c731e62ae4eaf383d320449d71687936bf\nVulkan-Headers 73814d006638aa00bd553215b054f0f02e457e22\n')
libdir=prebuilt/'lib';libdir.mkdir()
def elf(p):return p.is_file() and p.open('rb').read(4)==b'\x7fELF'
host=re.compile(r'^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util)\.so|lib(vulkan|GL|EGL|GLX|drm|gbm))')
seen=set();pending=[p for p in out.rglob('*') if elf(p)]
while pending:
    p=pending.pop();result=subprocess.check_output(['ldd',str(p)],text=True)
    if 'not found' in result:raise RuntimeError(result)
    for line in result.splitlines():
        match=re.match(r'\s*(\S+) => (/\S+)',line)
        if not match:continue
        name,path=match.groups()
        if name in seen or host.match(name):continue
        seen.add(name);target=libdir/name;shutil.copy2(path,target);pending.append(target)
        owner=subprocess.run(['dpkg-query','-S',os.path.realpath(path)],text=True,capture_output=True)
        if not owner.returncode:
            package=owner.stdout.split(': ',1)[0].split(':',1)[0]
            notice=Path('/usr/share/doc')/package/'copyright'
            if notice.is_file():shutil.copy2(notice,licenses/(package+'-copyright.txt'))
for name in ('python3.12','python3.12-minimal','libpython3.12-stdlib','libpython3.12-minimal'):
    p=Path('/usr/share/doc')/name/'copyright'
    if p.is_file():shutil.copy2(p,licenses/(name+'-copyright.txt'))
for p in (p for p in out.rglob('*') if elf(p)):
    subprocess.run(['patchelf','--set-rpath','$ORIGIN/'+os.path.relpath(libdir,p.parent),str(p)],check=True)
    subprocess.run(['strip','--strip-unneeded',str(p)],check=True)
(out/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(out).as_posix()+'\n' for p in sorted(out.rglob('*')) if p.is_file() and p.name!='SHA256SUMS'))
print('Standalone FP8 package created. No AMD DLL, shaders, weights, or GPU frames included.')
