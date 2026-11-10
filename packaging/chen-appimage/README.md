# chenxiao07 Linux AppImage

The fork includes Chen's `v20261110` updates through `3ec8cf56`.
The `main` branch (formerly `chen-kytyps5-fsr4-upscaler`) retains native FSR 4.1.2 INT8/FP8 upscaling,
experimental native FSR4 frame generation and Linux APR file retention/bounded asset prefetch.
See the [integration notes](../../docs/RELEASE-NOTES.md) for the included changes.

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

Open the [AppImage workflow](https://github.com/JOmega321/KytyPS5/actions/workflows/chen-appimage.yml),
choose a successful `main` run for the commit you want, and download its
`KytyPS5-chenxiao07-AppImage-x86_64-<date>` artifact. This is this fork's build;
Chen's separate release packages do not include its native Linux FSR4 integration. Unzip the artifact, then:

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

## Linux launcher features

- **Game → Keyboard controls → Edit controls…** opens the key/mouse layout editor.
- **Game → Keyboard and mouse prompts** is off by default. It applies matching icons and bindings on Play and
  restores the original icons on the next Play when disabled. It requires an extracted **PPSA01341 01.007.000**
  folder; `.zar` archives and 01.005.000 are not supported by this optional feature. Other emulator support for
  01.005.000 is unchanged. See [keyboard prompts](../../launcher-linux/README.md#keyboard-and-mouse-prompts).
- **FSR4 setup** prepares upscaling and frame-generation assets separately from the supported AMD DLLs.
  **Graphics** selects the upscaler/precision and frame generator separately. Native FSR4 failure retains the
  FSR3 fallback with a reason in the log. See [upscaling](../../docs/FSR412-LINUX.md) and
  [frame generation](../../tools/fsr4fg-vk/README.md#in-the-emulator).

Builds from `c7a98d4b` include both the keyboard editor and FSR4 Setup page. Check `BUILD-INFO.json` in the
artifact to identify its source commit when replacing an older AppImage.

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
