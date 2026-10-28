#!/usr/bin/env python3
"""Audit a staged AppDir; optionally resolve trusted ELF dependencies with ldd."""
import argparse
import configparser
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

MACHINES = {"x86_64": "Advanced Micro Devices X86-64", "aarch64": "AArch64"}
HOST_LIBS = re.compile(
    r"^(ld-linux[^/]*|lib(c|m|dl|pthread|rt|resolv|util|nss_[^.]+)\.so[^/]*|"
    r"lib(GL[^/]*|EGL|OpenGL|vulkan)\.so[^/]*)$"
)


def inside(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def audit(root, arch=None, resolve=False, allow_host=()):
    root = root.resolve()
    issues = []
    elfs = []

    def add(level, kind, path, detail):
        issues.append({"level": level, "kind": kind, "path": str(path), "detail": detail})

    if not root.is_dir():
        add("error", "layout", root, "AppDir does not exist or is not a directory")
        return {"appdir": str(root), "elf_count": 0, "issues": issues, "ok": False}
    apprun = root / "AppRun"
    if not apprun.is_file() or not os.access(apprun, os.X_OK):
        add("error", "entrypoint", "AppRun", "Require an executable file or valid executable symlink")
    desktops = sorted(root.glob("*.desktop"))
    if not desktops:
        add("error", "desktop", ".", "No root desktop entry")
    elif len(desktops) > 1:
        add("warning", "desktop", ".", "Multiple root desktop entries; verify intended entry")
    for desktop in desktops:
        if not inside(desktop.resolve(), root):
            add("error", "desktop", desktop.name, "Desktop entry resolves outside AppDir")
            continue
        parser = configparser.ConfigParser(interpolation=None, strict=False)
        try:
            parser.read_string(desktop.read_text())
            entry = parser["Desktop Entry"]
            for key in ("Type", "Name", "Exec", "Icon"):
                if not entry.get(key, "").strip():
                    add("error", "desktop", desktop.name, "Missing " + key)
            icon = entry.get("Icon", "")
            if icon and not any((root / (icon + ext)).exists() for ext in (".png", ".svg", ".xpm")):
                add("warning", "icon", desktop.name, "No matching root icon basename: " + icon)
        except (OSError, UnicodeError, configparser.Error, KeyError) as exc:
            add("error", "desktop", desktop.name, str(exc))
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if path.is_symlink():
            target = os.readlink(path)
            try:
                resolved = path.resolve()
                if os.path.isabs(target) or not inside(resolved, root):
                    add("error", "symlink", relative, "Absolute or escaping target: " + target)
                elif not path.exists():
                    add("error", "symlink", relative, "Broken target: " + target)
            except (RuntimeError, OSError) as exc:
                add("error", "symlink", relative, str(exc))
            continue
        if not path.is_file():
            continue
        try:
            with path.open("rb") as stream:
                if stream.read(4) == b"\x7fELF":
                    elfs.append(path)
        except OSError as exc:
            add("error", "read", relative, str(exc))
    if elfs and not shutil.which("readelf"):
        add("error", "tool", ".", "Install readelf/binutils to inspect ELF files")
    else:
        for path in elfs:
            relative = path.relative_to(root)
            result = subprocess.run(["readelf", "-h", "-d", str(path)], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    env={**os.environ, "LC_ALL": "C"})
            if result.returncode:
                add("error", "elf", relative, result.stderr.strip())
                continue
            output = result.stdout
            machine = re.search(r"^\s*Machine:\s*(.+)$", output, re.M)
            if arch and (not machine or machine.group(1).strip() != MACHINES[arch]):
                add("error", "architecture", relative, "Unexpected machine: " + (machine.group(1) if machine else "unknown"))
            for needed in re.findall(r"\(NEEDED\).*?\[([^]]+)\]", output):
                if "/" in needed:
                    add("error", "needed_path", relative, "Non-relocatable DT_NEEDED: " + needed)
            rpaths = re.findall(r"\((?:RPATH|RUNPATH)\).*?\[([^]]*)\]", output)
            if not rpaths and ".so" in path.name:
                add("warning", "rpath", relative, "No ELF search path; validate transitive/plugin dependencies")
            for entry in (part for group in rpaths for part in group.split(":")):
                if not entry:
                    add("warning", "rpath", relative, "Empty search entry depends on current directory")
                elif entry.startswith("/"):
                    add("error", "rpath", relative, "Absolute search path: " + entry)
                elif "$ORIGIN" in entry or "${ORIGIN}" in entry:
                    expanded = entry.replace("${ORIGIN}", str(path.parent)).replace("$ORIGIN", str(path.parent))
                    if "$" not in expanded and not inside(Path(expanded).resolve(), root):
                        add("error", "rpath", relative, "Search path escapes AppDir: " + entry)
                elif "$" not in entry:
                    add("warning", "rpath", relative, "Working-directory-relative search path: " + entry)
            if resolve and "(NEEDED)" in output:
                if not shutil.which("ldd"):
                    add("error", "tool", relative, "ldd is unavailable")
                    continue
                # Only opt in for trusted locally built/staged binaries.
                result = subprocess.run(["ldd", str(path)], text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        env={**{k: v for k, v in os.environ.items()
                                                if k not in {"LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"}},
                                             "LC_ALL": "C"})
                if result.returncode:
                    add("error", "ldd", relative, result.stdout.strip())
                for line in result.stdout.splitlines():
                    if "not found" in line:
                        add("error", "missing_library", relative, line.strip())
                    match = re.search(r"^\s*(\S+)\s+=>\s+(/\S+)", line)
                    if match and not inside(Path(match.group(2)).resolve(), root):
                        name = match.group(1)
                        if not HOST_LIBS.fullmatch(name) and not any(p.search(name) for p in allow_host):
                            add("warning", "host_library", relative, name + " resolves outside AppDir: " + match.group(2))
    return {"appdir": str(root), "elf_count": len(elfs), "issues": issues,
            "ok": not any(i["level"] == "error" for i in issues)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appdir", type=Path)
    parser.add_argument("--arch", choices=sorted(MACHINES))
    parser.add_argument("--resolve", action="store_true", help="Run ldd on trusted staged ELFs")
    parser.add_argument("--allow-host", action="append", default=[], metavar="REGEX", help="Allow a known host-supplied library name")
    args = parser.parse_args()
    try:
        patterns = [re.compile(value) for value in args.allow_host]
    except re.error as exc:
        parser.error(str(exc))
    report = audit(args.appdir, args.arch, args.resolve, patterns)
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
