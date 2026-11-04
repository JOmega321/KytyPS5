#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Sets up FSR 4 frame generation from the user's copy of AMD's DLL (FidelityFX SDK 2.4.0, MIT). None of AMD's bytes are
# in this project: the DLL comes from AMD's pinned URL or the user's disk, and prepare_fg.py refuses any other hash.
#   build_assets.sh --self-check
#   build_assets.sh --download|/path/to/amd_fidelityfx_framegeneration_vk.dll [OUTPUT_DIR]
# OUTPUT_DIR is replaced only when everything succeeded: a failure leaves what was there.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
python=${KYTY_FSR412_PYTHON:-$here/../fsr412-vk/prebuilt/bin/kyty-python3}
if [[ ! -x "$python" ]]; then python=python3; fi
tools=${KYTY_FSR412_SPIRV_TOOLS:-$here/../fsr412-vk/prebuilt/bin}
url=https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll
if [[ ${1:-} == --self-check ]]; then
    "$python" -u "$here/prepare_fg.py" --help > /dev/null
    for tool in spirv-opt spirv-val; do
        [[ -x "$tools/$tool" ]] || { echo "Missing bundled tool: $tool" >&2; exit 2; }
    done
    echo 'Bundled FSR 4 frame generation tools: OK'
    exit 0
fi
source=${1:?Pass --download or amd_fidelityfx_framegeneration_vk.dll from FidelityFX SDK 2.4.0}
out=${2:-${KYTY_FSR4FG_ASSETS:-${XDG_DATA_HOME:-$HOME/.local/share}/KytyPS5-chenxiao07/fsr4fg}}
mkdir -p -- "$(dirname -- "$out")"
stage=$(mktemp -d "$out.stage.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
chmod 755 "$stage"
dll=$stage/amd_fidelityfx_framegeneration_vk.dll
if [[ $source == --download ]]; then
    command -v curl > /dev/null || { echo 'curl is needed to download from AMD; choose a DLL file instead.' >&2; exit 2; }
    echo "Downloading from AMD: $url"
    curl -fL --retry 3 -o "$dll" "$url"
else
    cp -- "$source" "$dll"
fi
echo 'Preparing the programs for this driver (checks the DLL hash first)...'
"$python" -u "$here/prepare_fg.py" "$dll" "$stage/prepared" --spirv-tools "$tools"
old=$out.old.$$
if [[ -e $out ]]; then mv -- "$out" "$old"; fi
mv -- "$stage" "$out"
trap - EXIT
rm -rf -- "$old"
echo "Ready: FSR 4 frame generation files in $out"
