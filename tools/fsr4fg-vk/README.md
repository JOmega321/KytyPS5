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

## In the emulator

`fg_runtime.c` (this tool's test program uses the same code) and `src/local/fsr4-framegen.cpp` run AMD's runtime in the
game, where FSR 3 built in would otherwise make the frame (`FrameGen::Generate`):

1. The launcher's "FSR4 setup" page prepares the files (`build_assets.sh`: AMD's DLL by download or from disk, checked by
   hash; never shipped). Choosing FSR 4 frame generation sets `KYTY_FSR4FG_DIR` and RADV's matrix setting.
2. At startup the emulator runs `fsr4fg-gpu --self-test`, as the game will use it (`FG-TEST.md`). A verdict is kept, so
   this costs a fraction of a second after the first time.
3. The device gets what AMD's programs declare. AMD's thread starts, and its pipelines compile in the background: until
   they are done (some seconds, the first time) frames are shown without a generated one.
4. FSR 3 takes over, with one line in the log saying why (`Frame generation: FSR 4 not used, FSR 3 instead: ...`), when
   the self-test failed or could not run, the device differs from the tested one or lacks a feature, AMD's context is
   not ready within two minutes, or anything stops AMD's runtime later. A crash inside the driver is only caught by the
   self-test.

`KYTY_FRAME_CSV=<file>` records each game frame's time, the render thread's busy part, and frame generation's CPU and
GPU time, for any generator; `tools/local/frame-csv-summary.py` summarizes captures side by side.

Measured with the renderer idle at 1440p: the CPU recording is about 0.9 ms, about half of it one loop in AMD's code (RVA
`0x6a08`, 512-entry arrays for each of 196 dispatches) and about 0.2 ms in the driver; it reached about 2 ms during a
full build. The hand-off to AMD's thread is about 0.02 ms. AMD's barriers are already compute to compute: narrowing them
changed nothing.

`hud-anchor.h`'s interface shaders are 01.007.000's. On another version FSR finds no HUD canvas, gets no HUD-less image
and interpolates the HUD: `KYTY_HUD_SHADER_LOG=1` lists the shaders drawing into canvas-sized targets, and
`KYTY_HUD_CANVAS_SHADERS=<hex,hex>` adds some without a build.

## Limits

The synthetic device answers every feature query positively (FP8 ones only with the FP8 profile), so this records
the RDNA 4 path, not RADV's actual feature set. Group counts and constants are AMD's host decisions for these inputs;
they say nothing about image quality, GPU support or speed. Of the DLL's 416 unique programs, 96 fail Vulkan 1.3
validation as shipped and 38 still fail after `tools/fsr412-vk`'s FP8 preparation (reverse matrix casts in 2- and
4-subgroup workgroups; 16-bit-float image types); a replay needs those two cases handled too. Replay correctness has to be checked against
AMD's own output on real hardware.

## Step 3: AMD's runtime on the real GPU (test only)

- `prepare_fg.py` (with `fg_legalize.py`) prepares all 416 programs from the user's own DLL: the FSR 4.1.2 upscaler's
  FP8 preparation plus reverse matrix casts in 2- and 4-subgroup workgroups (slices chosen by `SubgroupId`), 16-bit
  image types and 64-bit sampled images. 166 programs change and each passes `spirv-val --target-env vulkan1.3`; the
  other 250 already do. `prepared-manifest.json` is the expected result; `tests/fsr4fg-legalize.py` checks that the
  upscaler's FP8 programs still prepare byte-identically.
- Passing validation is not enough for Mesa. `FFX_MLFI_COMPUTE_FWD_MV` fetches a 64-bit integer sampled image. It is
  valid SPIR-V, but Mesa has no 64-bit texture type, so `spirv_to_nir` fails on an assertion and returns NULL, and RADV
  (26.2.5) dereferences that in `radv_shader_spirv_to_nir`: a segfault with no message, inside AMD's context creation.
  AMD's image and view are already `R32G32_UINT`, and RADV cannot sample `R64_UINT` at all, so the fix is in the program
  alone: fetch 32-bit, rebuild `R | G << 32` (`tests/fsr4fg-int64-images.py`). No view or descriptor changes.
- `gpu_fg.c` (`build-gpu.sh`) runs AMD's runtime on the machine's Vulkan device: `gen_vk_thunks.py` generates
  Windows-ABI forwarders for every Vulkan command from the Khronos registry; shader creation swaps in prepared
  modules; matrix pipelines get full subgroups; a synthetic LUID is reported. A synthetic moving scene is scored
  against its exact in-between pictures (`FG-TEST.md`). CI probes AMD's providers through it on software Vulkan
  (llvmpipe: no FP8, so only 3.1.6 is offered; 3.1.6 itself fails to create there, llvmpipe having no wave32) and
  packages it for real hardware.

### Measured: RX 9060 XT 16 GB, RADV (Mesa 26.2.5), 2560x1440 from 1280x720, synthetic scene

| | FSR 4.0.2 | FSR 3.1.6 | blend | repeat |
|---|---|---|---|---|
| Moving areas, checker scene (dB vs exact midpoint) | 32.58 | 51.4 | 12.03 | 11.01 |
| Moving areas, `--organic` (dB) | 41.02 | 51.73 | 18.87 | 16.88 |
| Inside the objects, `--organic` | exact | exact | | |
| GPU time, 2000 frames in a row, mean / p95 (ms) | 1.97 / 2.54 | 1.77 / 2.76 | | |
| Video memory AMD reports (MB) | 119.8 | 78.8 | | |

AMD's DLL also runs relocated above the emulator's guest range, with byte-identical output, and `--self-test` reports
whether all of this works on a machine without being at risk from the driver (`FG-TEST.md`).

Pipelines: 195 for 4.0.2 (4.5 s with an empty shader cache, 0.1 s cached), 18 for 3.1.6. Output is byte-identical
between runs and with or without full subgroups. Also run: 1920x1080 (2.1 ms) and 3840x2160 (6.3 ms), at scored-frame
timing, not sustained. Not run: `mlfi_8k`, Vulkan validation layers (not installed here), AMD's Windows output as a
reference, and anything in a game. FSR 4's remaining error against FSR 3 is at object borders and uncovered areas; the
hard-edged rectangles here say little about either in real content.
