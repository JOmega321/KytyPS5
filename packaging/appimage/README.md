# KytyPS5 AppImage

This fork's **Build AppImage** GitHub Actions workflow builds native Linux
x86-64 binaries, bundles their runtime dependencies, and uploads an AppImage
plus its SHA-256 checksum as the `KytyPS5-AppImage-x86_64` artifact. It runs on
pushes to `appimage`.
The original Windows/macOS/Linux workflow is unchanged.

When only packaging or CI files have changed, the workflow can reuse a
previous successful build's binaries. It requires an exact match for every
file outside `packaging/` and `.github/`, verifies the previous artifact's
SHA-256 checksum, then repackages and repeats the startup checks. Source or
dependency changes trigger a complete build.

Download and extract the artifact ZIP, mark the `.AppImage` executable in your
file manager, and launch it. A current Vulkan 1.3 driver is required from the
host. The Ubuntu 24.04 build targets distributions with glibc 2.39 or newer;
it is not intended for older Linux releases. Fedora 44 meets that baseline.

```bash
chmod +x KytyPS5-*-x86_64.AppImage
./KytyPS5-<version>-x86_64.AppImage
```

Settings use the launcher's normal location (`~/.config/Kyty/Kyty.ini`, unless
overridden by `XDG_CONFIG_HOME` or a local `Kyty.ini`). Saves, shader caches,
patches, compatibility data, and logs use
`${XDG_DATA_HOME:-$HOME/.local/share}/KytyPS5`. Replacing the AppImage preserves
these files. Existing saves from a different installation must be copied there
manually. Set `KYTY_APPIMAGE_DATA_DIR` to an absolute path to override it.

For CLI use, pass `--emulator` before the emulator arguments. Use absolute
paths for game files because the working directory is the data directory:

```bash
./KytyPS5-<version>-x86_64.AppImage --emulator --game /absolute/path/to/game
```

If FUSE is unavailable, use `APPIMAGE_EXTRACT_AND_RUN=1` when launching.
The official in-place updater is disabled by `KYTY_BUILD_ORIGIN=Fork`; install
updates by downloading a newly built AppImage from this fork.

## Local packaging

Build and install using the repository's Linux instructions, then run:

```bash
bash packaging/appimage/package.sh _Build/linux/install
```

The script checks the linuxdeploy download against a SHA-256 digest. If the
upstream continuous asset changes, inspect the new release and update the
digest deliberately. Output defaults to `_Build/appimage-output/`.

The workflow checks the packaged emulator's `--help` and runs the graphical
launcher under Xvfb. Actual game rendering, sound, input, and Fedora/Wayland
behavior require testing on a desktop with a supported GPU.
