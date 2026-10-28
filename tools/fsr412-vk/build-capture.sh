#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=${1:?output directory required}
headers=${2:?Vulkan-Headers include directory required}
mkdir -p "$out"
cc=${MINGW_CC:-x86_64-w64-mingw32-gcc}
"$cc" -std=c2x -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -I"$headers" \
    -shared "$here/capture_vk.c" -Wl,--export-all-symbols,--out-implib,"$out/libvkcap.a" -o "$out/vulkan-1.dll"
"$cc" -std=c2x -O2 -Wall -Wextra -Wno-cast-function-type -I"$headers" \
    "$here/capture_main.c" "$out/libvkcap.a" -o "$out/capture.exe"
