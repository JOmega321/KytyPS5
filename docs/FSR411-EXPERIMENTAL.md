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
