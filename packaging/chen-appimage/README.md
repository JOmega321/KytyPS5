# chenxiao07 Linux AppImage

Upstream integrated through chenxiao07/KytyPS5 commit `bc9d4444644880f25a01009cfb9d73b8a52c34ec`.
The `chen-kytyps5-fsr4-upscaler` branch adds native FSR 4.1.2 and AppImage packaging.

The application is named **Demon's Souls (KyTy)** in Gear Lever, the desktop menu
and the Linux launcher. Its blue soul-wisp PNG is embedded at the AppDir root,
through `.DirIcon`, in the hicolor icon theme and beside the launcher for its
window icon. See [ICON.md](ICON.md) for artwork provenance. In Gear Lever, replace
the existing integration with the new AppImage; a previously cached name/icon may
need removal and reimport. Settings, saves, caches and FSR assets retain their paths.

The workflow builds the native SDL2/ImGui launcher, emulator and shader precompiler
on Ubuntu 24.04 using Clang 18, ThinLTO and x86-64-v3. It requires glibc 2.39 or
newer and an AVX2/FMA/BMI2 CPU, plus host Vulkan 1.3 drivers. Fedora 44 is a target;
older distributions are not promised. GPU drivers are not bundled. SDL's X11,
Wayland, ALSA, PulseAudio, D-Bus and udev support is enabled by the development
packages installed in the workflow. Packaging does not change emulation performance.

Download the AppImage artifact from the successful workflow run, unzip it, then:

```sh
chmod +x KytyPS5-chenxiao07-*-x86_64-*.AppImage
./KytyPS5-chenxiao07-*-x86_64-*.AppImage
```

AppImage filenames end with the build's start date in `MMDDYY` format, using
America/New_York time: for example `KytyPS5-chenxiao07-abcdef12-x86_64-102826.AppImage`.
The first published package in this dated naming series each day has no revision;
additional packages use `102826v1`, `102826v2`, and so on. Failed builds without
a published package do not consume a revision, and rerunning a published build
keeps its name. Builds are serialized to avoid duplicate daily numbers. GitHub
artifact names include the same suffix; `BUILD-INFO.json` records the source and
naming metadata. Older artifacts retain their existing names.

Choose your game in the settings window. `--emulator --help` opens the emulator's
CLI help; `--precompile --help` selects the shader tool. Use **absolute paths**
for game folders, archives and other file arguments. All arguments are forwarded.
Konsole or another supported desktop terminal is needed for launcher actions.
On systems without FUSE, set `APPIMAGE_EXTRACT_AND_RUN=1` or extract with
`--appimage-extract` and run `squashfs-root/AppRun`.

State lives in `${XDG_DATA_HOME:-$HOME/.local/share}/KytyPS5-chenxiao07/`:
`launcher-settings.json`, `_SaveData`, `_PipelineCache`, logs, patches and optional
DLSS files. `KYTY_APPIMAGE_DATA_DIR` can select another writable directory. To keep
terminal-launched programs usable after the GUI exits, AppRun installs its payload
in `.runtime/<source-revision>/` inside that state folder and reuses it on later
launches. Previous runtime directories may be removed when no old processes run;
preserve the other directories to retain saves and settings.

Updates are manual AppImage replacement. There is no upstream auto-sync or updater.
Startup checks exercise CLI help, launcher dry-run and all GUI pages under Xvfb
using a fresh HOME with spaces. These do not verify real GPU rendering, audio,
controllers, game compatibility or frame rate.
