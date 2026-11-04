#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/fsr4fg-vk/build.sh OUTPUT VULKAN_HEADERS_INCLUDE [extra compiler flags...]
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=${1:?output path required}
headers=${2:?Vulkan-Headers include directory required}
shift 2
# __declspec: AMD's FFX API headers (MIT, unmodified) mark their entry points for a Windows build.
"${CC:-gcc}" -std=gnu2x -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unknown-pragmas '-D__declspec(x)=' \
    -I"$headers" -I"$here" "$@" "$here/pehost.c" "$here/fakevk.c" "$here/record_fg.c" -lm -lpthread -o "$out"
