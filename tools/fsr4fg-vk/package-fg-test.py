#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package the FSR 4 frame generation GPU test without AMD data (after tools/fsr412-vk/package-fp8-test.py).

Usage: package-fg-test.py FSR4FG_GPU_BINARY SPIRV_TOOLS_DIR NEW_OUTPUT_DIR
The bundle holds the test program, the preparation scripts, pinned spirv-opt/spirv-val, a private Python runtime and
licences. The user supplies AMD's DLL (or lets run-fg-test.sh download it from AMD's GitHub, checked by SHA-256).
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

repo = Path(__file__).resolve().parents[2]
binary, tools, out = (Path(x).resolve() for x in sys.argv[1:])
out.mkdir(parents=True, exist_ok=False)
here = repo / "tools/fsr4fg-vk"
shutil.copy2(binary, out / "fsr4fg-gpu")
for name in ("prepare_fg.py", "fg_legalize.py", "run-fg-test.sh", "FG-TEST.md", "prepared-manifest.json"):
    shutil.copy2(here / name, out / name)
(out / "fsr412-vk").mkdir()
for name in ("adapt_spirv.py", "extract_vk.py", "prune_bindings.py", "legalize_fp8.py"):
    shutil.copy2(repo / "tools/fsr412-vk" / name, out / "fsr412-vk" / name)
prebuilt = out / "prebuilt"
(prebuilt / "bin").mkdir(parents=True)
for name in ("spirv-opt", "spirv-val"):
    shutil.copy2(tools / "build/tools" / name, prebuilt / "bin" / name)
licenses = out / "LICENSES"
licenses.mkdir()
shutil.copy2(repo / "LICENSE", out / "LICENSE")
shutil.copy2(tools / "source/LICENSE", licenses / "SPIRV-Tools-LICENSE.txt")
shutil.copy2(tools / "source/external/spirv-headers/LICENSE", licenses / "SPIRV-Headers-LICENSE.txt")
version = "python%d.%d" % sys.version_info[:2]
pyhome = prebuilt / "python"
(pyhome / "bin").mkdir(parents=True)
shutil.copy2(Path(sys.executable).resolve(), pyhome / "bin" / version)
shutil.copytree(Path("/usr/lib") / version, pyhome / "lib" / version,
                ignore=shutil.ignore_patterns("__pycache__", "test", "tests", "idlelib", "tkinter", "ensurepip",
                                              "sitecustomize.py", "config-*", "*.a", "*.o"))
wrapper = prebuilt / "bin/kyty-python3"
wrapper.write_text('#!/usr/bin/env bash\nset -euo pipefail\nhere="$(cd -- "$(dirname -- "$0")" && pwd)"\n'
                   'unset PYTHONPATH PYTHONSTARTUP\nexport PYTHONHOME="$here/../python"\n'
                   'exec "$PYTHONHOME/bin/' + version + '" -s "$@"\n')
wrapper.chmod(0o755)
source = out / "source/tools/fsr4fg-vk"
source.mkdir(parents=True)
for name in ("pehost.c", "pehost.h", "fg_runtime.c", "fg_runtime.h", "gpu_fg.c", "vk_thunks.h", "gen_vk_thunks.py", "build-gpu.sh", "package-fg-test.py"):
    shutil.copy2(here / name, source / name)
shutil.copytree(here / "ffx", source / "ffx")
(out / "SOURCE_COMMIT").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True))
(out / "TOOL_SOURCES").write_text("SPIRV-Tools 28f99f74a2a922ff85fc8fd966d6b3d6a45be795\n"
                                  "SPIRV-Headers 86f980c731e62ae4eaf383d320449d71687936bf\n"
                                  "Vulkan-Headers 73814d006638aa00bd553215b054f0f02e457e22\n")
libdir = prebuilt / "lib"
libdir.mkdir()


def elf(p):
    return p.is_file() and p.open("rb").read(4) == b"\x7fELF"


host = re.compile(r"^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util)\.so|lib(vulkan|GL|EGL|GLX|drm|gbm))")
seen, pending = set(), [p for p in out.rglob("*") if elf(p)]
while pending:
    p = pending.pop()
    result = subprocess.check_output(["ldd", str(p)], text=True)
    if "not found" in result:
        raise RuntimeError(result)
    for line in result.splitlines():
        match = re.match(r"\s*(\S+) => (/\S+)", line)
        if not match:
            continue
        name, path = match.groups()
        if name in seen or host.match(name):
            continue
        seen.add(name)
        target = libdir / name
        shutil.copy2(path, target)
        pending.append(target)
        owner = subprocess.run(["dpkg-query", "-S", os.path.realpath(path)], text=True, capture_output=True)
        if not owner.returncode:
            package = owner.stdout.split(": ", 1)[0].split(":", 1)[0]
            notice = Path("/usr/share/doc") / package / "copyright"
            if notice.is_file():
                shutil.copy2(notice, licenses / (package + "-copyright.txt"))
for name in ("python3.12", "python3.12-minimal", "libpython3.12-stdlib", "libpython3.12-minimal"):
    p = Path("/usr/share/doc") / name / "copyright"
    if p.is_file():
        shutil.copy2(p, licenses / (name + "-copyright.txt"))
for p in (p for p in out.rglob("*") if elf(p)):
    subprocess.run(["patchelf", "--set-rpath", "$ORIGIN/" + os.path.relpath(libdir, p.parent), str(p)], check=True)
    subprocess.run(["strip", "--strip-unneeded", str(p)], check=True)
(out / "SHA256SUMS").write_text("".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.relative_to(out).as_posix() + "\n"
                                        for p in sorted(out.rglob("*")) if p.is_file() and p.name != "SHA256SUMS"))
print("FSR 4 frame generation GPU test packaged. No AMD DLL, shaders or weights included.")
