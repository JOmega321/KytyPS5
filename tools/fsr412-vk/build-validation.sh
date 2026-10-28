#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# CI/developer helper: Ubuntu 24.04's stock validation layer predates KHR derivatives.
# Build the relevant Khronos release; never filter or suppress validation errors.
set -euo pipefail
out=${1:?new validation build directory required}
headers=${2:?pinned Vulkan-Headers source directory required}
mkdir -p "$out"
out=$(cd "$out" && pwd)
headers=$(cd "$headers" && pwd)
cmake_bin=${CMAKE:-cmake}
jobs=${FSR412_BUILD_JOBS:-2}
checkout() {
    local name=$1 url=$2 revision=$3
    if [[ ! -d "$out/$name/.git" ]]; then
        git init -q "$out/$name"
        git -C "$out/$name" remote add origin "$url"
        git -C "$out/$name" fetch -q --depth 1 origin "$revision"
        git -C "$out/$name" checkout -q --detach FETCH_HEAD
    fi
    [[ $(git -C "$out/$name" rev-parse HEAD) == "$revision" ]] || { echo "Unexpected source revision: $name" >&2; exit 1; }
}
checkout validation https://github.com/KhronosGroup/Vulkan-ValidationLayers.git 3ba0e590a2e76475b3eef4043355b498fb64e752
checkout utility https://github.com/KhronosGroup/Vulkan-Utility-Libraries.git 50563f48368d75281bc2fb1c3407dc531ce28910
checkout spirv-headers https://github.com/KhronosGroup/SPIRV-Headers.git 09913f088a1197aba4aefd300a876b2ebbaa3391
prefix="$out/deps"
"$cmake_bin" -S "$headers" -B "$out/headers-build" -DCMAKE_INSTALL_PREFIX="$prefix" -DVULKAN_HEADERS_ENABLE_TESTS=OFF
"$cmake_bin" --install "$out/headers-build"
"$cmake_bin" -S "$out/spirv-headers" -B "$out/spirv-headers-build" -DCMAKE_INSTALL_PREFIX="$prefix" -DSPIRV_HEADERS_ENABLE_TESTS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
"$cmake_bin" --install "$out/spirv-headers-build"
"$cmake_bin" -G Ninja -S "$out/utility" -B "$out/utility-build" -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O0 -DNDEBUG' -DBUILD_TESTS=OFF
"$cmake_bin" --build "$out/utility-build" -j "$jobs"
"$cmake_bin" --install "$out/utility-build"
"$cmake_bin" -G Ninja -S "$out/validation" -B "$out/build" -DCMAKE_PREFIX_PATH="$prefix" -DBUILD_TESTS=OFF -DBUILD_WERROR=OFF -DUSE_ROBIN_HOOD_HASHING=OFF -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF -DBUILD_WSI_WAYLAND_SUPPORT=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O0 -DNDEBUG'
"$cmake_bin" --build "$out/build" -j "$jobs"
