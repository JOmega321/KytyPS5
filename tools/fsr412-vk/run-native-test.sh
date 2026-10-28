#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
dll=${1:?Pass the path to amd_fidelityfx_upscaler_vk.dll from SDK 2.4.0}
if [[ ! -f "$dll" ]]; then echo "DLL file not found: $dll" >&2; exit 1; fi
if [[ ! -f "$here/native-frame-test" ]]; then
    echo 'The prebuilt native-frame-test binary must be beside this script.' >&2; exit 1
fi
# GitHub artifact ZIPs do not preserve executable permission bits.
chmod u+x "$here/native-frame-test"
(cd "$here" && sha256sum -c SHA256SUMS)
work=$(mktemp -d "$PWD/fsr412-native-test.XXXXXX")
echo "Experimental standalone test. Results: $work"
python3 "$here/recover_assets.py" "$dll" "$work/assets" > "$work/recovery.json"
"$here/native-frame-test" "$work/assets" "$work/frame-640x360-rgba16f.raw" 320x180 640x360 3 2>&1 | tee "$work/native-test.log"
echo "Test completed. Log: $work/native-test.log"
echo 'This does not install FSR 4.1.2 into Kyty; game rendering and image equivalence are still unverified.'
