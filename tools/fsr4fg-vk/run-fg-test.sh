#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs AMD's FSR Frame Generation 4.0.2 on this machine's Vulkan driver, outside the game, and scores the frames it
# generates against the exact in-between pictures of a synthetic scene.
#   bash run-fg-test.sh /path/to/amd_fidelityfx_framegeneration_vk.dll   (your copy from FidelityFX SDK 2.4.0)
#   bash run-fg-test.sh --download                                      (fetch it from AMD's GitHub, pinned + checked)
# Extra options go to the test program, e.g. --display 1920x1080 --render 960x540 --frames 24 --no-hud.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
url=https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll
sha=d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4
arg=${1:?Pass your amd_fidelityfx_framegeneration_vk.dll from SDK 2.4.0, or --download}
shift
# Artifact ZIPs lose executable bits.
chmod u+x "$here/fsr4fg-gpu" "$here/prebuilt/bin/"* "$here"/prebuilt/python/bin/python*
(cd "$here" && sha256sum --quiet -c SHA256SUMS)
echo '[FG] Package checksums: OK'
work=$(mktemp -d "$PWD/fsr4fg-gpu-test.XXXXXX")
echo "[FG] Results go to $work"
cp "$here/SOURCE_COMMIT" "$work/"
if [[ $arg == --download ]]; then
    curl -fL --retry 3 -o "$work/amd_fidelityfx_framegeneration_vk.dll" "$url"
    dll="$work/amd_fidelityfx_framegeneration_vk.dll"
else
    dll=$(realpath -- "$arg")
fi
[[ -f "$dll" ]] || { echo "DLL not found: $dll" >&2; exit 2; }
echo "$sha  $dll" | sha256sum --quiet -c || { echo 'That is not the SDK 2.4.0 frame generation DLL.' >&2; exit 2; }
echo '[FG] AMD DLL: SDK 2.4.0 (checked)'
# RADV exposes VK_NV_cooperative_matrix2 (needed for FP8) only with this; set for this process alone.
if [[ -z ${radv_cooperative_matrix2_nv+x} ]]; then export radv_cooperative_matrix2_nv=true; fi
"$here/prebuilt/bin/kyty-python3" -u "$here/prepare_fg.py" "$dll" "$work/prepared" --spirv-tools "$here/prebuilt/bin" \
    2>&1 | tee "$work/preparation.log"
status=0
"$here/fsr4fg-gpu" "$dll" "$work/prepared" "$work/results" "$@" 2>&1 | tee "$work/gpu-test.log" || status=${PIPESTATUS[0]}
if (( status > 128 )); then
    echo "[FG] exit status $status: stopped by signal $((status - 128)) ($(kill -l $((status - 128)) 2>/dev/null || echo ?))" | tee -a "$work/gpu-test.log"
else
    echo "[FG] exit status $status" | tee -a "$work/gpu-test.log"
fi
case $status in
    0) echo "[FG] Done. Send gpu-test.log and results/metrics.csv; results/*.ppm are the pictures (generated, exact midpoint, blend).";;
    3) echo '[FG] This driver/GPU lacks features AMD FSR 4 frame generation needs (listed above). Send gpu-test.log.' >&2;;
    5) echo "[FG] AMD's runtime did not offer FSR 4.0.2 on this device. Send gpu-test.log." >&2;;
    *) echo "[FG] The test failed (status $status). Send gpu-test.log and preparation.log." >&2;;
esac
echo '[FG] This test does not change the emulator, its settings or caches.'
exit "$status"
