#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=${1:-$here/out}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -fPIC -shared "$here/shader_pack.cpp" -o "$out/libfsr412_spirv.so"
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra "$here/native_probe.cpp" -ldl -o "$out/native-probe"
