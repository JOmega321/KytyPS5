# FSR 4.1.2 Vulkan DLL → Linux experiment

This is an **asset conversion prototype**, not a working FSR 4.1.2 replacement.
It does not export AMD's `ffxCreateContext` / `ffxDispatch` API and cannot yet
be selected as an upscaler in Kyty. The current FSR4 branch is unchanged.

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
- No GPU is available in the development environment. No dispatch, game frame,
  image equivalence, temporal stability or performance result is claimed.

## What remains before integration

1. Select the correct INT8/FP8 shader permutations for each model and output tier.
   Entry-point names alone are insufficient: many variants share the same name.
2. Recover/verify the model weights, host constants, resource layouts, barriers,
   dispatch order and workgroup counts. The 4.1.1 replay is a reference, not proof
   that its dispatch rules or initializers fit 4.1.2.
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
