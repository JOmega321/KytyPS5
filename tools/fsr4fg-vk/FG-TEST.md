# FSR 4 frame generation: standalone GPU test

This runs **AMD's own FSR Frame Generation 4.0.2 runtime** on your Linux Vulkan driver, outside the game. Nothing in
the emulator, its settings or its shader caches is touched. It answers one question first: does AMD's runtime work on
RADV this way, and do its generated frames look right?

How it works: a small Linux loader runs the CPU code of `amd_fidelityfx_framegeneration_vk.dll` (FidelityFX SDK
2.4.0, MIT) without Wine, and hands it your real Vulkan device through Windows-calling-convention forwarders. When
AMD's runtime creates a shader, the test swaps in a prepared copy where RADV needs one (FP8 matrix conversions,
16-bit image types, RGB writes). The preparation runs on your machine from your copy of the DLL; the package
contains none of AMD's shaders or weights.

The test renders a synthetic scene: a static background, three textured rectangles moving by known amounts each
frame and a static HUD strip. For every pair of frames it asks AMD for the frame in between, reads it back and
compares it with the **exact** in-between picture, and with two simple stand-ins: repeating the older frame and
blending both. Correct frame generation should beat both stand-ins in the moving areas.

## Run

Extract the whole ZIP, then either pass your DLL or let the script fetch AMD's (pinned URL, SHA-256 checked):

```bash
bash run-fg-test.sh --download
# or
bash run-fg-test.sh /path/to/amd_fidelityfx_framegeneration_vk.dll
```

Default: 2560x1440 output, 1280x720 render size, 12 frames. Other sizes:
`bash run-fg-test.sh --download --display 1920x1080 --render 960x540 --frames 24`.

- `--organic`: non-repeating object textures. The default checker repeats every 32 pixels, so a motion estimate can match
  the wrong cell. Compare both before reading a low score as a fault.
- `--timing-frames N`: N more frames in a row without drawing the scene, for GPU time under sustained load. Scored
  frames alone leave the GPU idle for seconds between dispatches, so their times swing with its clock (2.5 to 4.1 ms
  here for the same work).
- `--provider 3.1.6`: AMD's FSR 3.1.6 through the same path.

On RADV the script sets `radv_cooperative_matrix2_nv=true` for its own process only (needed for FP8 matrix
conversions), unless you already set it.

A new `fsr4fg-gpu-test.XXXXXX` folder holds:

- `gpu-test.log`: feature check, how many AMD shaders were replaced, per-frame GPU time and scores, summary.
- `results/metrics.csv`: per frame, PSNR (dB) against the exact midpoint for the whole picture, the moving areas and
  the HUD: generated, repeated, blended and next frame.
- `results/*.ppm`: one frame's generated picture, exact midpoint, blend and previous frame.
- `preparation.log`, `prepared/manifest.json`: what was changed in which shader.

## Self-test

For the launcher: can AMD's runtime run here, whatever the driver does? `fsr4fg-gpu DLL PREPARED_DIR STATE_DIR --self-test`
runs itself as a child, so a crash or hang in the driver cannot take the answer with it, and writes
`STATE_DIR/self-test.json` (exit 0 pass, 1 fail, 2 could not test), with the child's output in `self-test.log`. It takes
about 8 s with an empty shader cache, 4 s with a filled one.

- The child runs the non-repeating scene at 1280x720 from 640x360 for 6 frames, with `--guest-layout` (below).
- It passes when the worst frame's moving areas are at least 12 dB above the better of repeating and blending. On an
  RX 9060 XT the correct programs' worst frame is 17.37 dB. With R and G swapped in the forward motion program, the
  best any frame reached was about 6.7 dB. That is one kind of damage, so a pass says the path works, not that the
  picture is right in a game.
- A verdict holds until the driver, the GPU, AMD's DLL, the prepared programs' contents, `radv_cooperative_matrix2_nv`
  or the test's rules change; `--force` tests anyway. Failures that may not repeat (timeouts, the child killed from
  outside, a failed Vulkan call) are tested again on the next start. `--self-test-timeout SECONDS` defaults to 120.

`--guest-layout` reserves the emulator's guest address range the way it does on Linux at startup, so AMD's DLL is
relocated from its preferred base (`0x180000000`, inside that range). The pictures are byte-identical to an unrelocated
run.

`tests/fsr4fg-self-test.py` checks all of this, with injected crashes and hangs on software Vulkan and, with
`--hardware`, a pass, wrong frames and the real RADV crash on AMD's 64-bit sampled image program left as shipped.

## What it does not show

A synthetic scene is not Demon's Souls: textures, disocclusions, particles, camera motion and the HUD differ. GPU
time here is AMD's work alone with nothing else on the GPU. It says nothing about frame pacing or latency in the game.
The comparison is against an exact midpoint, not against AMD's Windows output.

The `RESULT` line only asks for 1 dB over repeating and blending. A deliberately broken shader (R and G swapped in the
forward motion pass) still passed it on the checker scene at 13.8 dB. Read the moving-area numbers and the pictures.
