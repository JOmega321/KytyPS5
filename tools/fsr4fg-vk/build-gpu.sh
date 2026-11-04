#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/fsr4fg-vk/build-gpu.sh OUTPUT VULKAN_HEADERS_DIR (the Vulkan-Headers checkout: include/ and registry/)
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=${1:?output path required}
headers=${2:?Vulkan-Headers directory required}
shift 2
gen=$(mktemp -d)
trap 'rm -rf "$gen"' EXIT
python3 -I "$here/gen_vk_thunks.py" "$headers" "$gen/vk_thunks.c"
"${CC:-gcc}" -std=gnu2x -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unknown-pragmas '-D__declspec(x)=' \
    -I"$headers/include" -I"$here" "$@" "$here/pehost.c" "$here/gpu_fg.c" "$gen/vk_thunks.c" -ldl -lm -lpthread -o "$out"
