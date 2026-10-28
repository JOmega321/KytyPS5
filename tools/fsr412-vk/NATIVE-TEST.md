# Experimental native FSR 4.1.2 INT8 frame test

This package goes beyond the shader-loader prototype: it recovers the model
initializer and the 29 selected shaders from your own AMD SDK 2.4.0 Vulkan DLL,
then executes a standalone native Linux Vulkan replay on synthetic inputs.
It does **not** enable FSR 4.1.2 in Kyty, change game settings, or establish
equivalence with AMD's output or correct game rendering. No frame generation.

## Run

Extract the complete artifact. Put your `amd_fidelityfx_upscaler_vk.dll` beside
`run-native-test.sh`, then run this one command in that directory:

```bash
bash run-native-test.sh ./amd_fidelityfx_upscaler_vk.dll
```

The script creates a fresh `fsr412-native-test.XXXXXX` folder containing recovered
assets, a log, provenance/checksums, and a raw RGBA16F 640x360 output image. It
checks the exact DLL SHA-256 before extraction. It never installs these assets
into your game or replaces existing output directories.

Requirements: x86-64 Linux, Python 3's standard library, the Vulkan loader, a
working Vulkan driver, and standard C++ runtime libraries. The included binary
needs no compiler, Wine, Proton, DXIL translator, or Windows driver. Its device
check requires Vulkan 1.3, FP16/INT8/INT16, integer dot products, linear compute
derivatives, extended storage-image formats, and push descriptors. This test
does not require `VK_VALVE_shader_mixed_float_dot_product`.

The first run can spend time compiling shaders. Completion means command
submission/readback and basic finite/nonzero-pixel checks passed. It does not
mean the image matches AMD's reference. Keep `native-test.log` for diagnosis.
If the machine uses software Vulkan, the log names that CPU implementation;
software execution is not evidence of Radeon speed or compatibility.

## Audited scope

- Official Vulkan DLL: version 4.1.2.4555, SHA-256
  `4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7`.
- Normal INT8 model, output tier up to 1920x1080 (`t1080_m0`) only.
  No 1440p/2160p model, Ultra Performance model, or FP8 path is included yet.
- The provider's CPU setup was recorded under Wine using an explicitly synthetic
  Windows adapter identity and fake Vulkan device. Three frames each recorded
  29 dispatches, including FSR4 model kernels; the tool rejects FSR3 fallback.
- Observed 131072-byte initializer SHA-256:
  `a54e552ff69a3f7199861417f7ae461d1f67f636ab532c6ec9d8ea5d2963735a`.
  It matches DLL bytes beginning at file offset 12741056.
- The selected SPIR-V is validated. A small conversion pads RGB storage-image
  writes to RGBA with alpha 1 to satisfy Vulkan's image-view component rule.
  RGB values and neural arithmetic are preserved. Adaptations are recorded in
  `asset-provenance.json`; this is not proof of full image equivalence.
- The native test uses Kyty's existing FSR 4.1.1 replay host implementation with
  recovered 4.1.2 assets to investigate compatibility. The shader and weight
  hashes in the provenance identify what actually ran.

## Source

The corresponding C++ source is under `source/`. Python conversion source is
included beside this file. License: GPL-2.0-or-later for original/test code;
included SDK interface headers retain AMD's MIT notice. No AMD DLL, shader,
weight, or frame bytes are distributed in this package.

Full source and CI: https://github.com/JOmega321/KytyPS5/tree/chen-kytyps5-fsr412-linux
The exact packaged source revision is in `SOURCE_COMMIT`.
To rebuild the frame test, provide Vulkan-Headers at commit
`73814d006638aa00bd553215b054f0f02e457e22` and Vulkan development libraries:

```bash
bash source/tools/fsr412-vk/build-frame-test.sh ./rebuilt /path/to/Vulkan-Headers/include
```

`FSR412_VALIDATE=1` enables Khronos validation when a sufficiently recent
validation-layer package is installed. Normal users do not need that package.
