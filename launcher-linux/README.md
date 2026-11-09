# kyty_launcher (Linux)

The settings window for Linux: the pages and settings of the Windows launcher (`launcher-windows/`), drawn with
Dear ImGui on SDL2's renderer. Both are linked in statically (the emulator's own `3rdparty/` copies), so the program
needs nothing installed beyond what the emulator needs. `build-linux.sh` builds it next to `kyty_emulator`, and the
Linux package carries it.

This fork retains eight navigation pages, including **FSR4 setup**. The keyboard controls editor opens as a modal
from **Game → Keyboard controls → Edit controls…**, alongside the optional in-game keyboard/mouse prompts.

It reads and writes `launcher-settings.json` next to `run_ds.sh`, with the Windows launcher's keys (unknown keys are kept),
or in `$XDG_CONFIG_HOME/KytyPS5/` when that folder is read-only. PLAY starts `run_ds.sh` in a terminal window with
the settings as the emulator's switches, its output also in `logs/<time>.log` (`_Build/run-logs/` in a build tree).
The precompile and DLSS's build (`precompile-linux.sh`, `get-dlss-linux.sh`) run in a terminal window too, where they
can ask y/N and for the sudo password.
The AppImage instead keeps its settings and other writable state in its
[AppImage data folder](../packaging/chen-appimage/README.md); replacing the AppImage preserves that state.

```
./kyty_launcher                the window
./kyty_launcher --play         the game with the saved settings, no window (Steam: a non-Steam game); its exit code
./kyty_launcher --lang pt      the window in English (en), Chinese (zh) or Portuguese (pt) for this run
./kyty_launcher --dry-run      the scripts the buttons run, printed
./kyty_launcher --screenshot <folder>   each page as <n>-<page>.bmp
```

## Settings and what they set

| Page | Setting | Emulator switch / option |
| --- | --- | --- |
| Display | Resolution | `--screen-width`, `--screen-height` |
| | Aspect ratio 21:9, 32:9 | `KYTY_RENDER_ASPECT` |
| | Movies and menus at 16:9 (black bars), with 21:9 or 32:9 | `KYTY_PRESENT_ASPECT=fit` |
| | Fullscreen, keep the aspect ratio | `--fullscreen`, `KYTY_PRESENT_ASPECT=fit` |
| | HDR | `--hdr` |
| | Up to 120 fps | `KYTY_VBLANK=120` (run_ds.sh adds `gameMinDeltaTime`) |
| | VSync | `PRESENT_MODE=Fifo` (run_ds.sh's mailbox otherwise) |
| | Low latency | `KYTY_LOW_LATENCY=1` |
| Graphics | Upscaler DLSS / FSR 3 / FSR 4.1.2 | `KYTY_UPSCALER=dlss` / `fsr` / `fsr4` |
| | FSR 4 precision INT8 / FP8 | `KYTY_FSR4_PRECISION=int8` / `fp8`; FP8 also requests RADV matrix conversions |
| | FSR 4 status watermark | `KYTY_FSR4_WATERMARK=1` / `0` |
| | 3D render height | `KYTY_RENDER_HEIGHT` |
| | Game output 1080p (or Automatic for a 16:9 window up to 1080 lines) | `KYTY_HD_OUTPUT=1` |
| | Game output 1440p (with DLSS or FSR 3) | `KYTY_OUTPUT_HEIGHT=1440` |
| | DLSS model | `KYTY_DLSS_PRESET` |
| | Sharpening Off / Low / Medium / High: DLSS (Off by default) | `KYTY_DLSS_SHARPNESS=0.3` / `0.6` / `0.9` (Off: none) |
| | Sharpening Off / Low / Medium / High: FSR 3 (Medium by default) | `KYTY_FSR_SHARPNESS=0` / `0.3` / `0.9` (Medium: none) |
| | Graphics preset Low / Lowest (Lowest: no decals too) | `KYTY_GAME_CONVARS` (Demon's Souls), `KYTY_TEXTURE_REDUCE=1` / `2` |
| | DLSS frame generation 2x-6x | `KYTY_FRAMEGEN=1`-`5` |
| | DLSS runtime | runs `get-dlss-linux.sh <the emulator's folder>` |
| | AMD FSR 3 frame generation | `KYTY_FRAMEGEN=1`, `KYTY_FRAMEGEN_BACKEND=fsr` |
| | FSR 4 frame generation (experimental, with prepared assets) | also `KYTY_FSR4FG_DIR` and `radv_cooperative_matrix2_nv=true`; FSR 3 fallback |
| Game | Console language | `--console-language` |
| | Audio output | `KYTY_AUDIO_OUTPUT` |
| | Keyboard and mouse prompts | key-cap icons in the game folder, and `--keymap` pairs added last (see below) |
| | Keyboard controls | the editor of the layout (Edit controls…) |
| | Cheats | `--game-patch _Patches/<title id>.json`; Choose… sets the mods' `enabled` flags |
| Shaders | Precompile, Quick, efficiency cores | `precompile-linux.sh --game … [--quick] [--cpus <E-cores>]` |
| | Export shader hints | `precompile-linux.sh --export-hints` |
| Advanced | Performance overlay | `KYTY_FPS_HUD=1` / `2` |
| | MangoHud | `MANGO=1` / `0` |
| | Safe GPU mode | `KYTY_COMPUTE_CHAIN=0` |
| | Graphics card (more than one) | `KYTY_GPU=<its name>` (the precompile's too) |
| | Extra options | `KEY=VALUE` (replaces the launcher's; `KEY=` removes it, or sets an emulator default to `0`), `--option value` (replaces the launcher's option of that name; a repeated one such as `--keymap L1=Q --keymap R1=E` keeps each), also the Windows form `-Set A=1,B=2` |
| Experimental | Lock to 30 fps | `KYTY_FLIP_RATE=1` |
| | Direct presentation | `KYTY_PRESENT_DIRECT=1` |
| | AMD Anti-Lag | `KYTY_ANTI_LAG=1`, `KYTY_LOW_LATENCY=1` |
| FSR4 setup | Upscaler and frame-generation asset preparation | separate AMD DLL inputs and preparation controls |

The game is `KYTY_GAME`, the emulator `KYTY_BIN` (the `kyty_emulator` next to the launcher, or `KYTY_BIN` when the
launcher was started with it). The variables above that a start does not set are unset, so what the shell exported
does not change a start.

The terminal is the desktop's own (Konsole on KDE, Ptyxis, Console or GNOME Terminal on GNOME, and so on), else the
first of the usual ones found, or the one chosen on the Advanced page. The folder dialog is kdialog, zenity or qarma when
one is installed, else a built-in one.

The Windows launcher's red-zone protection, Streamline folder and "Keep memory resident" are Windows-only and not shown;
their keys stay in the file.

## FSR4 setup

Prepare upscaling and frame-generation files in separate sections of **FSR4 setup**, then select the features in
**Graphics**. Upscaling uses `amd_fidelityfx_upscaler_vk.dll`; frame generation uses
`amd_fidelityfx_framegeneration_vk.dll`. Both inputs are checked against pinned SDK 2.4.0 hashes.
Selecting FSR4 frame generation does not select FP8 upscaling. Preparing files alone does not enable either feature.
See [native upscaling](../docs/FSR412-LINUX.md) and [native frame generation](../tools/fsr4fg-vk/README.md#in-the-emulator)
for feature requirements, runtime status and fallback behavior.

## Keyboard and mouse prompts

The same feature as the Windows launcher's (Game → Keyboard and mouse prompts, off by default), for an
extracted Demon's Souls game folder (PPSA01341 01.007.000; a ZArchive is refused). On **Play** (or `--play`) it
swaps the 22 button icons in `ui/textures/buttonicons/_ps5` for English key caps and adds the layout's
`--keymap` pairs after the Advanced page's own options; off, the originals are put back on the next Play.
`launch.json` is never rewritten. **Keyboard controls → Edit controls…** opens the editor: click an action, press a
key, a mouse button or the wheel; an input another action has swaps the two; F7, F8, F11 and Pause are the
emulator's. The layout is saved as `keyboard_binding_<control>` and `keyboard_mouse_sensitivity` in
`launcher-settings.json`, the Windows launcher's keys, so one file serves both.

The originals are checked by SHA-256 and kept in `.kyty-keyboard-prompts-v1` inside the game folder (the same
folder and files as the Windows launcher's: a game folder used from both keeps working). A modified, missing or
damaged icon stops the whole batch before anything is written; a failed write is rolled back; the emulator must not
be running. Links inside the game folder are refused (the game folder itself may be a link), and the folder names
are matched ignoring case. The pack (the textures, the blank template, the pixel map: `launcher-windows/KeyboardPrompts`)
is linked into the program, so nothing is installed.

Custom key caps are drawn with the desktop's bold sans font (fontconfig's `sans-serif:bold`; DejaVu Sans, Liberation
Sans or Noto Sans work, a CFF/OpenType-only font does not) to look like the pack's own icons; without a usable TrueType
font only the default layout's icons can be applied, and the editor says so.

`--dry-run` reports the option (`prompts:`) and prints the `--keymap` pairs it would pass; it changes no file.
Create the output folder before using `--screenshot`. It writes the eight navigation pages, including
`8-fsrsetup.bmp`, then the editor as `9-keyboard-controls.bmp`. The tests need no game or ImGui:
`cmake --build <build> --target kyty_launcher_prompts_tests && ctest --test-dir <build> -R launcher_keyboard_prompts --output-on-failure`
(configured with `-DBUILD_TESTING=ON`).
