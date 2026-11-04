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

## What it does not show

A synthetic scene is not Demon's Souls: textures, disocclusions, particles, camera motion and the HUD differ. GPU
time here is AMD's work alone with nothing else on the GPU. It says nothing about frame pacing or latency in the game.
The comparison is against an exact midpoint, not against AMD's Windows output.

The `RESULT` line only asks for 1 dB over repeating and blending. A deliberately broken shader (R and G swapped in the
forward motion pass) still passed it on the checker scene at 13.8 dB. Read the moving-area numbers and the pictures.
