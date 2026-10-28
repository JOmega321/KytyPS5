#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
python=${KYTY_FSR412_PYTHON:-$here/prebuilt/bin/kyty-python3}
if [[ ! -x "$python" ]]; then python=python3; fi
tools=${KYTY_FSR412_SPIRV_TOOLS:-$here/prebuilt/bin}
if [[ ${1:-} == --self-check ]]; then
    "$python" -c 'import sys,zlib,struct,json,hashlib,tempfile,fcntl; assert zlib.decompress(zlib.compress(b"FSR412"))==b"FSR412"; print("Bundled Python: OK")'
    for tool in spirv-opt spirv-val; do
        [[ -x "$tools/$tool" ]] || { echo "Missing bundled FP8 tool: $tool" >&2; exit 2; }
        "$tools/$tool" --version
    done
    echo 'Bundled native Vulkan asset tools: OK'
    exit 0
fi
dll=${1:?Choose amd_fidelityfx_upscaler_vk.dll from AMD FSR SDK 2.4.0}
out=${2:-${KYTY_FSR412_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/KytyPS5-chenxiao07/fsr4_412}}
exec "$python" -u "$here/recover_assets.py" "$dll" "$out" --all --install --include-fp8 --spirv-tools "$tools"
