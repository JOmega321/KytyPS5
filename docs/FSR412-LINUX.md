# Experimental native FSR 4.1.2 INT8 and FP8

Branch: `chen-kytyps5-fsr4-upscaler`, consolidating the INT8 and FP8 work.

## Use the AppImage

1. Replace the AppImage and launch it. Settings, saves, shader caches and generated
   assets remain under the existing XDG data locations.
2. In **FSR4 setup**, choose your own `amd_fidelityfx_upscaler_vk.dll` from
   AMD FSR SDK 2.4.0 (4.1.2.4555), then click **Build FSR4 INT8 + FP8 assets**.
   Existing INT8 users need this preparation once to add the FP8 sets.
3. In **Graphics**, select **FSR 4.1.2**, then choose **INT8** or
   **FP8 (RDNA4, experimental)** under **FSR 4 precision**.
4. **RADV matrix conversions** is enabled automatically when FSR 4 FP8 is
   selected. No separate toggle or Extra options entry is needed. This opts only
   the game process into the driver's experimental matrix-conversion extension.
5. Enable **FSR 4 status watermark** and restart the game. It reports the actual
   successful precision: FP8, INT8, INT8 because FP8 is unavailable, awaiting
   upscale, or FSR 3.1 fallback.

INT8 remains the default. If FP8 features, assets or pipeline creation are
unavailable, the game reports the reason and attempts FSR 4.1.2 INT8. If that is
also unavailable, it retains the existing FSR 3.1 fallback. The launcher footer
reports the selected option; the in-game watermark reports runtime use. Saved
FP8 selections also enable matrix conversions even if the old toggle was off.
INT8 and other upscalers do not enable this option. Advanced Extra options can
still explicitly override generated launch variables. The
legacy benchmark variable `BB_FSR411_VARIANT` cannot override the menu choice.

## Assets and preparation

Default folder: `~/.local/share/KytyPS5-chenxiao07/fsr4_412/`, or under
`$XDG_DATA_HOME`; `KYTY_FSR412_DIR` can override it. The INT8 families are in
the root and FP8 families in `fp8/`. Both have version markers. Old 4.1.1 assets
remain separate.

Preparation checks the exact signed DLL SHA-256, shader identities and model
initializers, then installs all eight sets together. It preserves an existing
recognized installation in a sibling `.previous-*` folder on replacement;
failed preparation leaves the active installation intact. The AppImage bundles
Python, the standard library, pinned SSA tools and a strict SPIR-V validator.
No host compiler, shader tools, Wine or Proton are needed. No AMD DLL, shader
bytes or model weight bytes are distributed.

The source CLI can prepare INT8 alone with `recover_assets.py DLL NEW_FOLDER
--all`. Add `--include-fp8 --spirv-tools TOOLS_FOLDER` to prepare both precisions.
The standalone `--fp8` test mode remains separate and never installs into Kyty.
Logs are under the existing application `logs/` folder.

## Renderer and validation scope

Both precisions use the same `DemonsSoulsSuperResolution::Install` PS5 Pro
constructor patch and display mode 5 as FSR 3.1, consuming that path's scene
color, depth, motion vectors, jitter and reset state. Scene color remains
RGBA16F, motion RG16F; output supports RGBA16F and the game's packed
B10G11R11 HDR view. Internal history stays RGBA16F. This does not add FSR4 frame
generation; the existing frame-generation backend remains in use.

The four normal/Ultra Performance families cover both output tiers through
3840x2160, including 2560x1440. CPU host captures on explicitly synthetic
adapters check shader selection, initializers, constants and dispatch groups.
FP8 uses native float8 cooperative matrix multiplication and its own weights.
Compatibility conversions remove unused globals and opaque local copies, pad
RGB writes where required, and stage unsupported reverse matrix-use conversions
through shared FP16/FP32 storage. All 116 final FP8 shaders pass strict Vulkan
1.3 SPIR-V validation. The shader adaptations and final hashes are recorded
in each installation's provenance.

The user confirmed INT8 working in Demon's Souls on an RX 9060 XT with less
ghosting than FSR 3.1. The user also ran three native standalone FP8 frames
(1280x720 -> 2560x1440, packed HDR, sharpening off) on the same GPU with all
feature checks enabled and finite, populated readback. Khronos runtime
validation was not requested in that hardware test. These observations do not
establish equivalence with AMD's reference, broad game compatibility or
performance. The user subsequently confirmed in-game FP8 activation after
enabling RADV matrix conversions. Broader FP8 image quality and performance
remain unverified.

The host supplies Vulkan drivers. INT8 requires Vulkan 1.3, compute derivatives,
integer dot products, FP16/INT8/INT16, extended storage-image formats and push
descriptors. FP8 additionally enables float8 cooperative matrices, matrix
conversions, variable pointers, 16-bit storage, Vulkan memory model and full
32-lane compute subgroups. Unsupported FP8 features are never forced on a device.

Updates use manual AppImage replacement. Switching precision, preparing assets
or changing output size/model applies on the next game launch.
