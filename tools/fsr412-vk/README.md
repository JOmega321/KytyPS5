# FSR 4.1.2 Vulkan DLL → Linux experiment

This branch contains shader extraction, an offline host-call recorder, and an
**experimental standalone native INT8 frame test**. It does not export AMD's
`ffxCreateContext` / `ffxDispatch` API and cannot yet be selected as an upscaler
in Kyty. The existing FSR4 AppImage branch is unchanged.

See [NATIVE-TEST.md](NATIVE-TEST.md) for the prebuilt frame-test package and the
single-command test. The initial `native-probe` described below only reads
shader bytes; it does not execute frames.

The SDK 2.4 Vulkan DLL contains SPIR-V in the publicly documented compressed
shader format. `extract_vk.py` decompresses those GPU programs entirely on Linux,
deduplicates them and records their entry points, descriptor bindings, capabilities,
extensions, execution modes and specialization constants. It also lists the DLL's
Windows imports. The extraction uses only Python's standard library; no Wine,
Proton, MinGW, DXIL translation or AMD Windows driver is needed for this step.

The native ELF library `libfsr412_spirv.so` reads the converted `shaders.spvpack`.
Its own small C API exposes shader words and entry-point names to native Linux
code. `native-probe` loads that library with `dlopen` and reads every program.
This demonstrates native access to the converted GPU code, not correct GPU
execution, a complete converted DLL, or a functioning upscaler.

## Run the prototype

Use your own `amd_fidelityfx_upscaler_vk.dll` from AMD's official SDK 2.4.0.
The DX12 DLL and driver `amdvkffx64.dll` are different files.

```bash
python3 extract_vk.py /path/to/amd_fidelityfx_upscaler_vk.dll /path/to/new-output-directory
./native-probe ./libfsr412_spirv.so /path/to/new-output-directory/shaders.spvpack
```

The CI artifact includes the Linux binaries, extractor, headers and this README.
It contains no AMD DLL, extracted shaders or model weights. On Fedora, Python 3
is normally already available; only the Python standard library is used.
Building these two native binaries from source uses `bash build-native.sh out`.

## Verified input and result

Input: official SDK `v2.4.0`, upscaler Vulkan DLL version `4.1.2.4555`.
SHA-256: `4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7`.

- All effects: 1,778 compressed shader occurrences, 1,145 unique SPIR-V modules.
- FSR4 model kernels: 1,389 occurrences, 1,002 unique modules, 67,960,420 bytes.
- Native ELF loader: all 1,002 converted FSR4 modules read successfully on Linux.
- Unit tests cover malformed PE files, invalid compression/size limits, duplicate
  shaders, and native pack bounds and instruction checks.
- No Radeon GPU is available in the development environment. The separate frame
  test can use Mesa software Vulkan. A software result does not establish
  hardware compatibility, image equivalence, temporal stability, or performance.

## Host setup and initializer recovery

`capture_vk.c` implements a fake Vulkan device. `capture_main.c` runs the signed
DLL's CPU provider under Wine and redirects its Windows adapter queries to an
explicitly synthetic identity. It refuses FSR3 fallback and records selected
pipelines, buffer uploads, descriptors, constants, groups and barriers.
It performs no GPU work. The SDK requires a Windows adapter LUID and Windows
GDI ASIC-family/revision queries before exposing its FSR4 provider.

CI observed the normal INT8 model at 1280x720 -> 1920x1080: three frames of
29 dispatches and one 131072-byte initializer, byte-identical to DLL contents
at offset 12741056. `host-int8-t1080-m0.json` contains hashes and observations,
not shader or initializer bytes. `recover_assets.py` checks the user's exact
DLL hash, recovers those assets without Wine and records file hashes/provenance.

`native_frame_test.cpp` uses the existing replay host implementation to test
these 4.1.2 assets. Its Vulkan device does not require VALVE mixed float dot
products: the selected official INT8 shaders use KHR integer dot products and
compute derivatives. `adapt_spirv.py` pads RGB storage-image writes to RGBA to
satisfy the image-view component rule, preserving RGB and supplying alpha 1.
The corresponding shader hashes and modifications are recorded separately.

## What remains before integration

1. Extend the audited selection beyond INT8 normal-model output tier 1080p.
   Entry-point names alone are insufficient: many variants share the same name.
2. Compare all host constants, resource layouts, barriers and dispatch rules
   across resolutions and models. The initializer and 29-pass order are now
   observed for the first INT8 model; that does not validate all replay behavior.
3. Replace or bridge the Windows CPU provider. This DLL imports MSVCP140,
   VCRUNTIME140, Windows CRT API sets, Kernel32, VERSION and vulkan-1.dll. Renaming
   or mechanically rewriting the file header does not implement those ABIs.
4. Validate against AMD's output on a real GPU before connecting it to Kyty's
   PS5 Pro input path. Never mark shader extraction as a successful upscaler.

Format reference (including its actual numeric magic and zlib decoder):
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/api/internal/ffx_blob_compression.h
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/api/internal/ffx_blob_decompress.h

The implementation here is original code in the Kyty GPL-2.0 project. AMD's
binaries and any shaders/weights extracted by a user retain their own terms.
