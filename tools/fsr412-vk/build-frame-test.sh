#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=${1:?output directory required}
headers=${2:?Vulkan-Headers include directory required}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers \
    -DKYTY_FSR_STANDALONE -I"$headers" "$here/native_frame_test.cpp" \
    "$here/../../src/local/fsr411/fsr411.cpp" -lvulkan -o "$out/native-frame-test"
