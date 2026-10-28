#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
install_dir="$(realpath -- "${1:?Usage: package.sh INSTALL_DIR [OUTPUT_DIR] [install|repack]}")"
output_dir="${2:-$root/_Build/appimage-output}"
mkdir -p -- "$output_dir"
output_dir="$(realpath -- "$output_dir")"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
appdir="$work/KytyPS5.AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/licenses/kytyps5"
case "${3:-install}" in
    install) cp -a "$install_dir/." "$appdir/usr/bin/" ;;
    repack) cp -a "$install_dir/." "$appdir/" ;;
    *) echo 'Expected install or repack mode' >&2; exit 2 ;;
esac
test -x "$appdir/usr/bin/kyty_launcher"
test -x "$appdir/usr/bin/kyty_emulator"
test -f "$appdir/usr/bin/qt.conf"
install -m 755 "$root/packaging/appimage/AppRun" "$appdir/AppRun"
install -m 644 "$root/packaging/appimage/kytyps5.desktop" "$appdir/kytyps5.desktop"
install -m 644 "$root/src/launcher/icons/kyty.svg" "$appdir/kytyps5.svg"
install -m 644 "$root/LICENSE" "$appdir/usr/share/licenses/kytyps5/LICENSE"
cp -a "$root/LICENSES" "$appdir/usr/share/licenses/kytyps5/"

# Fail closed if the continuous asset is replaced; update this digest deliberately.
deploy="$work/linuxdeploy-x86_64.AppImage"
curl --fail --location --retry 3 --output "$deploy" \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
echo "8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1  $deploy" | sha256sum --check
chmod +x "$deploy"

# Enumerate Qt plugins as well as the main executables so their non-Qt
# dependencies are included. Keep the CMake installation's Qt layout intact.
libraries=()
while IFS= read -r -d '' library; do
    libraries+=(--library "$library")
done < <(find "$appdir/usr/bin/lib" "$appdir/usr/bin/plugins" \
    -type f -name '*.so*' -print0)
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=x86_64
export VERSION="${VERSION:-$(git -C "$root" rev-parse --short HEAD)}"
export OUTPUT="$output_dir/KytyPS5-${VERSION}-x86_64.AppImage"
# Repackaged Qt libraries must resolve against the existing AppDir, even
# when the runner has no matching system Qt installation.
LD_LIBRARY_PATH="$appdir/usr/lib:$appdir/usr/bin/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
"$deploy" --appdir "$appdir" \
    --executable "$appdir/usr/bin/kyty_launcher" \
    --executable "$appdir/usr/bin/kyty_emulator" \
    --executable "$appdir/usr/bin/kyty_updater" \
    "${libraries[@]}" \
    --desktop-file "$appdir/kytyps5.desktop" \
    --icon-file "$appdir/kytyps5.svg"

# linuxdeploy copies --library inputs into usr/lib, but the Qt plugins
# remain in their original CMake location. Point those originals at the
# deployed libraries so no global Qt/LD_LIBRARY_PATH override is needed.
while IFS= read -r -d '' plugin; do
    relative_libdir="$(realpath --relative-to="$(dirname -- "$plugin")" "$appdir/usr/lib")"
    patchelf --set-rpath "\$ORIGIN/$relative_libdir" "$plugin"
    dependencies="$(ldd "$plugin")"
    if [[ "$dependencies" == *'not found'* ]]; then
        printf 'Unresolved Qt plugin dependencies for %s:\n%s\n' "$plugin" "$dependencies" >&2
        exit 1
    fi
done < <(find "$appdir/usr/bin/plugins" -type f -name '*.so*' -print0)

# The Qt libraries now live in usr/lib. Keep the original relative layout
# available to Qt without retaining a second, unpatched copy of each library.
rm -rf -- "$appdir/usr/bin/lib"
ln -s ../lib "$appdir/usr/bin/lib"
python3 "$root/packaging/appimage/audit_appdir.py" "$appdir" --arch x86_64 --resolve
"$deploy" --appdir "$appdir" --output appimage
test -s "$OUTPUT"
chmod +x "$OUTPUT"
cd -- "$output_dir"
sha256sum "$(basename -- "$OUTPUT")" > "$(basename -- "$OUTPUT").sha256"
