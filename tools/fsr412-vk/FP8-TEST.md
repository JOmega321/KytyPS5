# Experimental native FSR 4.1.2 FP8 hardware test

The working Kyty AppImage is INT8. This separate package investigates native
FP8 on RDNA4 and never installs or changes the working game's assets/settings.
No FP8 game integration, image equivalence, or performance claim is made yet.

Extract the complete ZIP. Put your `amd_fidelityfx_upscaler_vk.dll` from SDK
2.4.0 (4.1.2.4555) beside the script. On RADV, opt into its experimental matrix
conversion extension for this test's process only:

```bash
bash run-fp8-test.sh --radv-matrix-conversions ./amd_fidelityfx_upscaler_vk.dll
```

Alternatively omit the flag when the required extensions are already exposed.
The script checks the selected GPU first, recovers/verifies all four FP8 asset
families from your exact DLL, strictly validates the final SPIR-V, then runs
three native FP8 frames at 1280x720 -> 2560x1440 into the game's packed HDR
format. There is no INT8 or FSR3 fallback in this test. It uses native FP8
cooperative matrices, with FP16/FP32 staging for reverse-use conversions.

The binaries, Python runtime, standard library, SSA tool and strict validator
are included. A host Vulkan driver/loader is required; compilers, Proton and
shader tools need not be installed. A fresh `fsr412-fp8-test.XXXXXX` folder holds
`feature-check.log`, `native-test.log`, preparation/provenance and raw readback.
Send the feature and native-test logs for the next integration step. If the
feature check fails, send `feature-check.log`; keep using the INT8 AppImage.

Requirements include Vulkan 1.3, FP16/INT8/INT16, linear compute derivatives,
integer dot products, extended storage image formats, push descriptors,
`VK_EXT_shader_float8`, `VK_KHR_cooperative_matrix`, and
`VK_NV_cooperative_matrix2` with matrix conversions. Variable pointers,
16-bit storage, Vulkan memory model, and full wave32 subgroups are checked.
Hardware model alone does not prove the installed driver exposes those features.

## Audit and conversion scope

The SDK's FP8 CPU provider was recorded on an explicitly synthetic RDNA4 device:
three frames each for normal and Ultra Performance models in both output tiers,
plus normal-model 1440p. All 435 dispatches' constants/groups match the native
replay; this is host evidence, not GPU execution. Initializers match DLL bytes:
normal FP8 offset 31619088, Ultra Performance FP8 offset 31488016, both 131072 bytes.

Conversion removes provably unused descriptors, uses pinned SPIRV-Tools SSA
rewrite to eliminate opaque image copies, and pads RGB writes to RGBA where
needed. The SDK also emits A/B -> accumulator matrix conversions that the NV
extension does not permit. For the audited 16x16, 32-thread kernels, these use
shared-memory row-major FP16/FP32 staging with workgroup barriers. Native FP8
matrix multiplication and weights stay FP8. This adaptation needs hardware
and image-quality validation and may add overhead.

All 116 final shaders must pass strict Vulkan 1.3 SPIR-V validation. No final
validation errors are suppressed. Input SSA rewrite bypasses input validation
only because the exact signed SDK shader needs legalization; the resulting
module is checked with the bundled modern validator before it can be used.

GPL source for the native replay/test is under `source/`; preparation source is
beside this file. `SOURCE_COMMIT` identifies the exact branch revision and
`TOOL_SOURCES` pins the unmodified Apache-2.0 SPIRV-Tools/Headers sources. Rebuild
with `source/tools/fsr412-vk/build-frame-test.sh` and pinned Vulkan-Headers.
No AMD binaries, shader bytes, weight bytes, or rendered frames are distributed.
