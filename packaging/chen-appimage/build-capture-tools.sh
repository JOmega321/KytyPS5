#!/usr/bin/env bash
# Build the non-AMD capture tools in CI; no compiler or translator download on the user's PC.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
build=$(realpath -m "${1:-_Build/fsr-capture-tools}")
mkdir -p "$build/bin"
revision=7dc52786cdb1f53c54dd5c5c2698dff00ea5f0a3
if [[ ! -d $build/dxil-spirv/.git ]]; then
    git clone https://github.com/HansKristian-Work/dxil-spirv.git "$build/dxil-spirv"
fi
git -C "$build/dxil-spirv" checkout --detach "$revision"
git -C "$build/dxil-spirv" submodule update --init --recursive
git -C "$build/dxil-spirv" apply "$root/tools/fsr4cap/dxil-spirv-class-bindings.patch"
cmake -S "$build/dxil-spirv" -B "$build/dxil-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build "$build/dxil-build" --target dxil-spirv --parallel 4
install -m 755 "$build/dxil-build/dxil-spirv" "$build/bin/dxil-spirv"
while IFS= read -r -d '' library; do
    [[ ! -f $library ]] || install -m 755 "$library" "$build/bin/$(basename -- "$library")"
done < <(find "$build/dxil-build" -name '*.so*' -print0)
x86_64-w64-mingw32-gcc -std=c11 -O1 -Wall \
    -I"$root/tools/fsr4cap/ffx/api/include" -I"$root/tools/fsr4cap/ffx/upscalers/include" \
    "$root/tools/fsr4cap/fsr4cap.c" "$root/tools/fsr4cap/capture.c" "$root/tools/fsr4cap/rootsig.c" \
    -o "$build/bin/fsr4cap.exe" -ld3d12 -ldxguid -static
for tool in spirv-dis spirv-as flock; do
    install -m 755 "$(command -v "$tool")" "$build/bin/$tool"
done
# Preserve corresponding source and notices for redistributed third-party tools.
printf '%s\n' "$revision" > "$build/source-revision.txt"
git -C "$build/dxil-spirv" submodule status --recursive > "$build/source-submodules.txt"
(cd "$build/dxil-spirv" && tar --exclude='.git' --exclude='build' -czf "$build/dxil-spirv-source.tar.gz" .)
sha256sum "$build"/bin/* > "$build/tool-sha256.txt"
