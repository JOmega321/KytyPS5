#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ ${1:-} == --radv-matrix-conversions ]]; then
    export radv_cooperative_matrix2_nv=true
    shift
    echo '[FP8] Opting into RADV experimental matrix conversions for this process only.'
fi
dll=$(realpath -- "${1:?Pass your amd_fidelityfx_upscaler_vk.dll from SDK 2.4.0}")
[[ -f "$dll" ]] || { echo "DLL not found: $dll" >&2; exit 2; }
# Artifact ZIPs lose executable permission bits.
chmod u+x "$here/native-frame-test" "$here/prebuilt/bin/kyty-python3" "$here/prebuilt/bin/spirv-opt" "$here/prebuilt/bin/spirv-val" "$here"/prebuilt/python/bin/python*
(cd "$here" && sha256sum --quiet -c SHA256SUMS)
echo '[FP8] Package checksums: OK'
work=$(mktemp -d "$PWD/fsr412-fp8-test.XXXXXX")
echo "Experimental standalone FP8 test. Results: $work"
cp "$here/SOURCE_COMMIT" "$work/"
if ! "$here/native-frame-test" --fp8-check 2>&1 | tee "$work/feature-check.log"; then
    echo 'FP8 prerequisites failed. Keep the working INT8 AppImage.' >&2
    echo 'For RADV, this test can be retried with --radv-matrix-conversions if that opt-in was not used.' >&2
    exit 1
fi
"$here/prebuilt/bin/kyty-python3" -u "$here/recover_assets.py" "$dll" "$work/assets" --fp8 --spirv-tools "$here/prebuilt/bin" > "$work/recovery.json" 2> >(tee "$work/preparation.log" >&2)
"$here/native-frame-test" "$work/assets" "$work/frame-1440p-r11g11b10.raw" 1280x720 2560x1440 3 r11g11b10 no-sharpen fp8 2>&1 | tee "$work/native-test.log"
echo "FP8 frame execution and readback passed. Log: $work/native-test.log"
echo 'This does not enable FP8 in Kyty. Reference image quality, performance and game integration remain unverified.'
