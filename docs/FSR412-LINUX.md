# Experimental native FSR 4.1.2 INT8

Branch: `chen-kytyps5-fsr412-linux`. Linux x86-64 AppImage, SDK 2.4.0 Vulkan
upscaler 4.1.2.4555. This replaces the experimental 4.1.1 upscaler selection in
this branch. The earlier branch and its generated 4.1.1 assets remain separate.

## Use the AppImage

1. Make the downloaded AppImage executable and open it.
2. In **FSR4 setup**, choose your own `amd_fidelityfx_upscaler_vk.dll` from
   AMD FSR SDK 2.4.0, then click **Build FSR4 INT8 assets**. The AppImage includes
   the Python runtime and extraction code. Preparation runs entirely on Linux;
   Steam, Proton, MinGW and shader-translation tools are unnecessary.
3. In **Graphics**, select **FSR 4.1.2 INT8 (PS5 Pro path, experimental)**.
   Enable **FSR 4 status watermark** to distinguish active FSR4, awaiting upscale,
   and an actual FSR 3.1 fallback. Restart the game after preparing assets or
   changing the output size/model.

The selector keeps the existing `fsr4` setting value. The setup page stores
the Vulkan DLL choice separately in `fsr412_dll`; it preserves the older DLL
setting rather than silently treating a DX12 DLL as the new Vulkan input.

The default asset folder is
`~/.local/share/KytyPS5-chenxiao07/fsr4_412/` (or under `$XDG_DATA_HOME`).
`fsr4_411/` remains untouched. Preparation checks the exact SDK DLL hash,
validates and decompresses the selected shaders, verifies model initializers,
and installs all four sets together. Existing recognized 4.1.2 assets are
preserved in a sibling `.previous-*` folder when replaced. A failed preparation
does not replace the installed assets. Logs are in the existing `logs/` folder.

The game checks `ASSET_VERSION` before creating an upscaler. Missing assets,
unsupported Vulkan features, unsupported sizes or pipeline failures retain the
existing FSR 3.1 fallback and report a reason in the game log. The launcher's
summary says which option is selected; the in-game watermark reports runtime use.

## Rendering path and scope

`KYTY_UPSCALER=fsr4` uses the same `DemonsSoulsSuperResolution::Install` PS5 Pro
constructor patch and display mode 5 as FSR 3.1. It consumes that path's scene
color, depth, motion vectors, jitter and reset state. It does not upscale the
already composed window or introduce a second game patch.

INT8 normal-model and Ultra Performance families are included for output tiers
through 1920x1080 and 3840x2160, including 2560x1440. The signed DLL's Windows
host calls were recorded on an explicitly simulated INT8 adapter. Three frames
per captured configuration were checked against the replay's constants,
dispatch order and group counts. Only the user's own DLL supplies shader and
weight bytes; none are included in the AppImage or repository.

The native programs require Vulkan 1.3, linear compute derivatives, integer dot
products, FP16/INT8/INT16, extended storage-image formats and push descriptors.
Unlike the prior DXIL conversion, these official Vulkan INT8 shaders do not
require `VK_VALVE_shader_mixed_float_dot_product`. RGB storage-image writes are
padded to RGBA with alpha 1 to meet Vulkan's component-count rule; the changes
and resulting hashes are recorded in each asset installation's provenance.

This is an experimental integration. Native standalone frame execution has
passed on Mesa software Vulkan with Khronos validation and on the user's
RX 9060 XT. Those results do not establish equivalence with AMD's reference,
correct rendering in Demon's Souls, or performance. Frame generation continues
to use the project's existing implementation; this does not add FSR4 frame
generation. The FSR4 shader/model selection is explicitly INT8.

Updates use manual AppImage replacement and preserve settings, saves, pipeline
caches and generated assets outside the image. Host Vulkan drivers are required
and are not bundled.
