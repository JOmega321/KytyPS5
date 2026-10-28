#!/usr/bin/env python3
"""Check the packaged launcher's precision-to-environment behavior."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def exports(script):
    result = {}
    for line in script.splitlines():
        if line.startswith("export "):
            for word in shlex.split(line)[1:]:
                key, value = word.split("=", 1)
                result[key] = value
    return result


def main():
    image = str(Path(sys.argv[1]).resolve())
    cases = [
        ("fresh-fp8", {"upscaler": "fsr4", "fsr4_precision": "fp8"}, "fp8", True),
        ("old-toggle-off", {"upscaler": "fsr4", "fsr4_precision": "fp8",
                            "fsr4_radv_matrix_conversions": False}, "fp8", True),
        ("old-toggle-on", {"upscaler": "fsr4", "fsr4_precision": "fp8",
                           "fsr4_radv_matrix_conversions": True}, "fp8", True),
        ("int8", {"upscaler": "fsr4", "fsr4_precision": "int8",
                  "fsr4_radv_matrix_conversions": True}, "int8", False),
        ("default-int8", {"upscaler": "fsr4"}, "int8", False),
        ("fsr3", {"upscaler": "fsr", "fsr4_precision": "fp8"}, None, False),
        ("off", {"upscaler": "off", "fsr4_precision": "fp8"}, None, False),
    ]
    with tempfile.TemporaryDirectory(prefix="kyty precision ") as temporary:
        root = Path(temporary)
        data = root / "data"
        data.mkdir()
        settings = data / "launcher-settings.json"
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_CACHE_HOME=str(root / "cache"), XDG_DATA_HOME=str(root / "xdg"),
                   KYTY_APPIMAGE_DATA_DIR=str(data), APPIMAGE_EXTRACT_AND_RUN="1",
                   radv_cooperative_matrix2_nv="true")
        for name, values, precision, matrix in cases:
            settings.write_text(json.dumps(values))
            run = subprocess.run([image, "--dry-run"], env=env, check=True,
                                 capture_output=True, text=True, timeout=60)
            variables = exports(run.stdout)
            assert variables.get("KYTY_FSR4_PRECISION") == precision, (name, run.stdout)
            assert (variables.get("radv_cooperative_matrix2_nv") == "true") == matrix, (name, run.stdout)
            if not matrix:
                assert any("radv_cooperative_matrix2_nv" in shlex.split(line)[1:]
                           for line in run.stdout.splitlines() if line.startswith("unset ")), (name, run.stdout)
            print(f"Launcher precision: {name}: OK")


if __name__ == "__main__":
    main()
