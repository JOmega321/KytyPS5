# Experimental native Vulkan FSR 4.1.1

Opt in with `KYTY_UPSCALER=fsr4`, or select **FSR 4.1.1 (experimental, INT8)**
in the Linux launcher. FSR 3 remains the fallback for missing features, missing
assets, unsupported formats/sizes, and output/model changes. A restart is
required after changing the output size or between regular/ultra-performance
models. This first implementation selects INT8; it does not enable FP8 matrices.
No FSR 4 frame generation is provided; existing FSR 3 frame generation is unchanged.

## Generate your assets

Use your own `amd_fidelityfx_upscaler_dx12.dll` version 4.1.x. The supplied
4.1.1.2740 DLL passed the static capture compatibility check. AMD binaries and
model data are not committed or distributed in the AppImage.

From the checkout:

```
BB_FSR411_OUT="$HOME/.local/share/KytyPS5-chenxiao07/fsr4_411" \
  bash tools/fsr4cap/build_assets.sh /absolute/path/amd_fidelityfx_upscaler_dx12.dll
export KYTY_FSR411_DIR="$HOME/.local/share/KytyPS5-chenxiao07/fsr4_411"
KYTY_UPSCALER=fsr4 ./_Build/linux/kyty_emulator --game /absolute/path/game
```

From the experimental AppImage:

```
APPIMAGE_EXTRACT_AND_RUN=1 ./KytyPS5-*.AppImage --build-fsr411 /absolute/path/amd_fidelityfx_upscaler_dx12.dll
```

This needs a recent working Proton/Steam runtime and a suitable GPU on the
capture machine. The script reports missing build dependencies (MinGW, CMake,
Ninja, Python, SPIR-V Tools) and builds its pinned dxil-spirv translator.
Proton is used for preparing assets; the emulator itself remains native Linux.
The AppImage exports KYTY_FSR411_DIR to its persistent data folder. Assets are
user-generated and persist across manual AppImage replacement.

## Device requirements and validation

Requires float16, int8/int16, integer dot product, extended storage image formats,
linear compute derivatives and VK_VALVE_shader_mixed_float_dot_product.
The features are queried and enabled at logical-device creation only when FSR 4
is requested; no support is inferred from the GPU's brand alone. A sufficiently
recent Vulkan driver is required, including on RX 9000 cards.

The adapter accepts whole images, RGBA16F color/output and RG16F motion, with
Kyty's depth-aspect image view and render-pixel motion/jitter conventions. It
restores the caller's layouts before fallback. Constant uploads are GPU commands
with uniform-read/transfer-write barriers, rather than host writes over the
replay engine's in-flight ring. Profiling queries are disabled because this
adapter has no per-submit completion tracking. Contexts stay alive until exit;
output/model changes fall back instead of freeing resources in recorded work.

GPU capture, successful FSR 4 dispatch, temporal stability, visual correctness,
performance and interactions with frame generation require real game testing.
A passing build/launcher startup does not establish those results. The existing
FSR 3 option can be selected at any time. Capture errors and runtime fallback
messages must not be interpreted as successful FSR 4 rendering.

## Attribution

Replay engine and capture tools adapted from deadinside28/bloodborne_pc,
commit 8185b88d80573fa300595d5c54450183b4109935, GPL-2.0-or-later:
https://github.com/deadinside28/bloodborne_pc
Original SPDX headers retained; this distribution uses GPL version 2, consistent
with KytyPS5. Capture API headers retain AMD's MIT notices. Kyty modifications
provide renderer/device integration, GPU-ordered constants, FSR 3 fallback,
launcher selection, persistent asset paths and AppImage packaging. The upstream
bbport-specific benchmark is not included or represented as a Kyty validation.

## Launcher options (October 28 update)

Graphics → FSR 4 status watermark adds an emulator text overlay. It reports
FSR 4.1.1 INT8 only after a successful command recording; after two seconds without
upscaling it says awaiting upscale. A failed backend attempt shows FSR 3.1 fallback.
This is our status indicator, not AMD's DLL watermark or proof of correct GPU output.
It works independently of the FPS overlay. Changes apply next game launch.

FSR 4.1.1 INT8 selects `KYTY_UPSCALER=fsr4`. The same
`DemonsSoulsSuperResolution::Install` used by FSR 3.1 installs the PS5 Pro
constructor patch and display mode 5; the existing super-resolution renderer supplies
the same scene, depth, motion and jitter inputs. The patch log names FSR4 explicitly.
Unsupported game versions retain the game's TAA; this does not add a PS5 Pro patch
to versions that lack it.

Shaders → Shader cache folder accepts a writable directory, with a folder browser
and a default reset. `KYTY_PIPELINE_CACHE_DIR` redirects the runtime pipeline cache,
SPIR-V translations, static precompiled binaries, warmup records, and file-follow
records. Precompile and the launcher's cache-status check use the same directory.
Blank keeps the original `_PipelineCache` in persistent app data. Existing files
are not migrated or deleted. Do not change folders while play/precompile is running.
Driver-managed Mesa/NVIDIA caches are separate and retain their own locations.

CPU controls on Shaders:

- Background shader workers: Automatic (upstream policy), or 1/2/4/8/16 per pool.
  Sets `KYTY_SHADER_PREFETCH_THREADS`, `KYTY_SHADER_FOLLOW_THREADS` and
  `KYTY_SHADER_WARMUP_BACKGROUND_THREADS`. Try 1 on a busy Ryzen 5 5600X;
  less CPU contention trades off against slower preparation and potential later stutter.
- Warm up shaders during play: on by default. Off sets
  `KYTY_SHADER_WARMUP_BACKGROUND=0`, doing warmup before play instead.
- Precompile CPU jobs: Automatic or 1/2/4/8/16 processes, three threads per process,
  via precompile-linux.sh `--jobs`. Applies to offline precompilation only.

These expose existing emulator controls; no FPS gain is asserted without benchmarks.

## Build assets from the menu

Open **FSR4 setup** in the sidebar, or use **Set up FSR 4.1.1 assets** on Graphics.
Choose `amd_fidelityfx_upscaler_dx12.dll` (4.1.1), then **Build FSR4 INT8 assets**.
No terminal command is needed. The page shows the current stage, elapsed time,
capture counts and live stdout/stderr. It keeps a full build log, provides Cancel,
and checks that all four INT8 sets contain their 29 SPIR-V modules and weights.
This checks file completeness, not GPU rendering or compatibility.

The builder still requires Python, SPIR-V tools, MinGW GCC, CMake, Ninja, GCC,
Git, flock, and Steam with a compatible Proton; its output reports missing tools
and unsupported DLLs. Tools may be obtained through Nix if installed. The translator
is downloaded from its pinned source and built locally. DLLs and model assets are
not distributed. Closing the launcher cancels the owned builder process group.
Restart the game after a successful build. Existing assets are retained until the
new set is generated; a failed/cancelled build can be retried from the same page.

## Bundled preparation tools

The AppImage now includes the prebuilt `fsr4cap.exe`, pinned and patched dxil-spirv,
SPIR-V assembler/disassembler, flock, and a private Python runtime with its standard
library and native dependencies. The setup menu automatically uses them. No host
compiler, MinGW, CMake, Ninja, Git, Python or SPIR-V installation is needed, and it
no longer downloads translator source during setup. Source builds outside the
AppImage retain the development-tool fallback.

Your own AMD DLL, Steam/compatible Proton, and host Vulkan driver remain required.
`--fsr411-tools-check` checks the bundled interpreter and native tool startup and
performs a SPIR-V assembly/disassembly round trip without needing the GPU.
This is separate from testing DLL capture on a real GPU. The translator's pinned
corresponding source and notices are included under the capture tool directory.
