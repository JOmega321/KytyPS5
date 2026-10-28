# FSR 4.1.2 Vulkan DLL → Linux experiment

This branch contains shader extraction, an offline host-call recorder, and an
**experimental native INT8 integration in Kyty**. See
[the AppImage setup guide](../../docs/FSR412-LINUX.md). It replays the audited
Vulkan shader graph; it does not export AMD's Windows `ffxCreateContext` /
`ffxDispatch` ABI. The earlier FSR4 AppImage branch remains separate.

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

## Integration status

The four metadata recipes in `recipes/` cover both output tiers and the normal
and Ultra Performance INT8 models. CI rechecks shader selection, initializers,
constant buffers and dispatch rules against three newly recorded host frames.
`recover_assets.py DLL NEW_FOLDER --all` prepares all four sets. The AppImage
bundles the interpreter and installs them through its FSR4 setup page.

Native validation and nonuniform test frames exercise all four families with
software Vulkan. The game uses the existing PS5 Pro input path and retains FSR3
fallback on initialization or dispatch failure. Radeon in-game visual checks,
reference comparison, temporal stability and performance remain unverified.

Format reference (including its actual numeric magic and zlib decoder):
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/api/internal/ffx_blob_compression.h
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/api/internal/ffx_blob_decompress.h

The implementation here is original code in the Kyty GPL-2.0 project. AMD's
binaries and any shaders/weights extracted by a user retain their own terms.
