#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
out=${1:?build output directory required}
mkdir -p "$out"
out=$(cd "$out" && pwd)
checkout() {
    local path=$1 url=$2 revision=$3
    if [[ ! -d "$path/.git" ]]; then
        git init -q "$path"
        git -C "$path" remote add origin "$url"
        git -C "$path" fetch -q --depth 1 origin "$revision"
        git -C "$path" checkout -q --detach FETCH_HEAD
    fi
    [[ $(git -C "$path" rev-parse HEAD) == "$revision" ]] || { echo 'Unexpected SPIR-V source revision' >&2; exit 1; }
}
checkout "$out/source" https://github.com/KhronosGroup/SPIRV-Tools.git 28f99f74a2a922ff85fc8fd966d6b3d6a45be795
checkout "$out/source/external/spirv-headers" https://github.com/KhronosGroup/SPIRV-Headers.git 86f980c731e62ae4eaf383d320449d71687936bf
cmake -G Ninja -S "$out/source" -B "$out/build" -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG'
cmake --build "$out/build" --target spirv-opt spirv-val --parallel "${FSR412_BUILD_JOBS:-3}"
