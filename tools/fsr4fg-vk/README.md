# FSR 4 frame generation: offline recorder

Records what AMD's FSR Frame Generation **4.0.2** provider asks a Vulkan device to do, so a native Linux replay
(the way `tools/fsr412-vk` replays FSR 4.1.2 upscaling) can be built and checked against it. This is step 2 of the
frame generation investigation; nothing here is used by the emulator yet.

- Input: `amd_fidelityfx_framegeneration_vk.dll` from AMD's FidelityFX SDK **v2.4.0**,
  SHA-256 `d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4`. The SDK's `docs/license.md` lists
  it among its MIT-licensed files. It is not stored in this repository.
- **No Wine and no GPU.** `pehost.c` maps the DLL into a Linux process and serves the Win32/CRT functions it imports
  (Windows x64 calling convention, a TEB with its TLS block, events, locks, printf family). Anything else it might call
  stops the process with the function's name. `fakevk.c` is a synthetic Vulkan device (adapted from
  `tools/fsr412-vk/capture_vk.c`): it reports a Radeon RX 9060 XT identity on RADV with FP8 features, allocates host
  memory for buffers, and logs every command. Nothing is executed and no pixels are produced.
- `record_fg.c` drives the provider as the fork's Windows path does (SDK 2 API: create, configure, prepare V2 with
  camera vectors, dispatch), without AMD's frame generation swapchain
  (`FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY`). Depth is reversed-Z with an infinite far plane, as the
  game's; HUD-less colour is supplied. It refuses any provider other than the one asked for (default `4.`).

```sh
bash tools/fsr4fg-vk/build.sh _Build/fsr4fg-record /path/to/Vulkan-Headers/include
mkdir new-empty-dir && cd new-empty-dir
../_Build/fsr4fg-record /path/to/amd_fidelityfx_framegeneration_vk.dll 2560x1440 1280x720 3
python3 -I ../tools/fsr4fg-vk/analyze_fg.py . /path/to/amd_fidelityfx_framegeneration_vk.dll contract.json
python3 -I ../tools/fsr4fg-vk/check_contract.py contract.json mlfi_4k ../tools/fsr4fg-vk/contracts/fg402-2560x1440-render1280x720.json
```

The run directory receives AMD's shaders and buffer contents; keep it local. The contract (`analyze_fg.py`) holds
hashes, group counts, binding names, resource shapes and the small constant buffers only.

## What the recording shows (SDK 2.4.0, synthetic RDNA 4 device)

- The DLL offers `4.0.2` and `3.1.6`. Without FP8 features (`--no-fp8`) it offers only `3.1.6`. It never asked the
  Windows adapter (GDI) queries the 4.1.2 upscaler needed; it decides from Vulkan features.
- Every frame: 5 helper passes (`FFX_MLFI_DILATE_MV_PASS`, three `FFX_MLFI_SPLATTING_64`,
  `FFX_MLFI_COMPUTE_FWD_MV`) then `FFX_MLFI_PASS_0..190`. Display sizes up to 3840x2160 use `mlfi_4k`; 7680x4320 uses
  `mlfi_8k`. Frames after the first (reset) run the same programs with the same group counts.
- All 191 ML passes require a subgroup size of 32 (no full-subgroups flag); the helpers set none.
- One weights upload of 10,657,152 bytes, byte-identical to the DLL at file offset 14,644,400 (both families).
- Every dispatched shader is byte-identical to a SPIR-V module stored uncompressed in the DLL.
- The network works at a third of the display size (853x480 at 2560x1440). Pass 0 reads the HUD-less colour and a
  copy of the previous one; pass 190 also reads the presented colour (`r_hudless_ui`) and writes the generated frame.
- AMD's immutable "linear" sampler is created with `NEAREST` filters on this device (recorded as created).
- AMD clears its two-channel splat targets with stale words in the unused channels; the contract keeps only real ones.
- Reported memory at 2560x1440: 199.8 MB (116.9 MB aliasable).

## Limits

The synthetic device answers every feature query positively (FP8 ones only with the FP8 profile), so this records
the RDNA 4 path, not RADV's actual feature set. Group counts and constants are AMD's host decisions for these inputs;
they say nothing about image quality, GPU support or speed. Of the DLL's 416 unique programs, 96 fail Vulkan 1.3
validation as shipped and 38 still fail after `tools/fsr412-vk`'s FP8 preparation (reverse matrix casts in 2- and
4-subgroup workgroups; 16-bit-float image types); a replay needs those two cases handled too. Replay correctness has to be checked against
AMD's own output on real hardware.
