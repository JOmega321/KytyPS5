# Native Linux FSR 4 performance work

Branch: `chen-kytyps5-fsr4-upscaler`, based on `a897c60f` (Chen through `2a439236`).
Existing FSR 4.1.2 assets remain valid. No asset regeneration, cache deletion, settings
migration or worker-default change is required. The launcher, fallback reporting,
automatic RADV FP8 option, watermark, sharpening and branding remain unchanged.

## AppImage PGO

The AppImage uses the existing Linux instrumentation profile with explicitly selected
Clang/LLVM/lld 18 on Ubuntu 24.04, O3, ThinLTO and **x86-64-v3**. It does not use
`-march=native` or Zen-specific code. CI reads the profile using llvm-profdata-18 and
compiles/links a PGO probe before building the actual programs. The build manifest
records the compiler and profile digest; CMakeCache is retained in build evidence.

The profile contains 105,399 function records. README's provenance describes eight
scripted spawn-point scenes with walking, camera turns and fights, rather than a fresh
interactive session on this fork. There are no `Fsr411` or `Fsr4Upscale` records in the
shipped profile. It cannot establish optimized coverage of this native INT8/FP8 replay,
this GPU, PPSA01342 or every gameplay path. Source/hash drift can reduce coverage;
CMake's existing unprofiled/out-of-date warning suppression remains in effect. A readable
profile proves format compatibility, not training quality or a speedup. Retrain with
Clang 18 on representative gameplay before making PGO performance claims.

## Descriptor recording and synchronization

Resource names are resolved into per-pass recipes during shader loading. Unknown names
fail before frame recording. Descriptor writes and info arrays are sized once, then
updated in place. Pointers are formed only after sizing and storage is not resized by
Record. Push descriptors copy host data during the Vulkan call; the arrays are not GPU
resources. Each Upscaler owns its own arrays; calls on one instance must be serialized.
The game adapter already holds its State mutex. GPU resources and the constant ring
retain their original submission/lifetime contract. Pass indices replace per-frame
string construction and numeric parsing; dispatch formulas are unchanged.

The four external images are accessed only by compute shaders inside the replay.
Entry barriers now target COMPUTE_SHADER; exit barriers source COMPUTE_SHADER. Caller
scopes remain ALL_COMMANDS because attachments, copies, exposure/sharpening and fallback
paths differ. Access masks, layouts, ignored queue-family indices, depth/stencil aspects
and restoration on failure are unchanged. Upload/reset transfers use private resources.
The FP8-to-INT8 failure barrier stays broad. Every dependent neural-pass barrier stays.
Standalone tests exercise the replay, **not** the game adapter's layout transitions;
real gameplay synchronization validation remains necessary.

Relevant Vulkan contracts: [push descriptors](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html),
[synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html),
[timestamps](https://docs.vulkan.org/spec/latest/chapters/queries.html).

## Profiling

`BB_FSR4_PROFILE` was already disabled by `false && profile` when introduced in
`a52b5e83`; history supplies no rationale. Do not attribute an undocumented reason to
its author. Inspection found no queue timestamp-capability check, timestamp-bit wrap
handling or explicit fence completion; it discarded unavailable results on ring reuse.
Those are reasons not to enable that implementation unchanged.

The replacement is opt-in standalone profiling (`FSR412_BENCHMARK=1`). The caller owns
and resets the query pool, checks the actual queue's timestampValidBits, waits for a
submission fence, reads results and only then reuses/destroys queries. Both timestamps
are written for skipped passes. The game does not allocate or enable these queries.
CPU record time excludes submission; GPU replay time includes uploads/barriers but
excludes final readback; submit/wait wall time is reported separately. Per-pass timestamp
intervals are diagnostic and include instrumentation effects, not isolated shader cost.
The first frame includes pipeline creation and reset; discard warmup samples.

Example, after building `tools/fsr412-vk/build-frame-test.sh` (use a new output path):

```sh
FSR412_BENCHMARK=1 radv_cooperative_matrix2_nv=true \
  _Build/native/native-frame-test "$HOME/.local/share/KytyPS5-chenxiao07/fsr4_412" \
  /tmp/new-fp8.raw 1280x720 2560x1440 300 r11g11b10 no-sharpen fp8 > /tmp/fp8.log 2>&1
python3 tools/fsr412-vk/summarize-benchmark.py /tmp/fp8.log --warmup 30
```

For INT8, unset the RADV option and change the precision argument to `int8`. Keep the
same resolution, sharpening, input pattern and sample count; alternate run order and
repeat. Never infer gameplay FPS from these serialized synthetic submissions. The
existing `KYTY_SR_TIMING=1` game diagnostic reports aggregate upscale GPU means; it is
not the replacement per-pass profiler and does not supply frame-time distributions.

## Shader workers and gameplay protocol

On 12 logical CPUs, SpareThreads() selects 2 workers; prefetch drops to 1 for its gentle
phase. Follow helpers are a separate pool (normally 2 when recorded work exists).
Background warmup also defaults to 2. The launcher worker control sets prefetch,
follow and background-warmup counts together; “1 worker” is not one total background
thread. Existing code documents contention on other CPUs, not a 5600X result.

Compare 1 and 2 using the launcher's existing control, without changing affinities,
power settings or unrelated options. Test both active background compilation and a
fully warm cache; preserve identical cache snapshots for cold comparisons rather than
deleting the user's store. Fix game version/scene, camera route, render/output size,
precision, sharpening, frame cap and frame generation. Record the active precision and
fallback logs, driver/version, all three worker counts, CPU frame time, GPU upscale time
and frame-time p50/p95/p99/max. Alternate INT8/FP8 × 1/2 workers, use at least three
matched 60-second captures after warmup, and retain raw samples. A shorter cold load
is not necessarily smoother play. No worker-default change is justified without this.

## Local evidence

On RX 9060 XT / RADV GFX1200, Mesa 26.2.5, both native INT8 and native FP8 completed
three frames at 1280x720 → 2560x1440, packed HDR, sharpening on/off. Optimized output
was byte-identical to baseline `a897c60f` in all four cases. Readback was finite,
populated and free of sentinel components. Runtime validation was not enabled in
these hardware checks; this is neither AMD-reference equivalence nor gameplay testing.

A first standalone timing capture, 150 frames with the first 30 discarded, no sharpening:

| Metric (ms) | INT8 mean / p95 / p99 | FP8 mean / p95 / p99 |
| --- | --- | --- |
| GPU replay | 3.061 / 3.143 / 3.170 | 1.724 / 1.777 / 1.779 |
| CPU Record | 0.0215 / 0.0274 / 0.0324 | 0.0244 / 0.0295 / 0.0386 |
| Submit + fence wait | 3.147 / 3.334 / 3.427 | 1.799 / 1.887 / 2.025 |

These are single-run synthetic observations with timestamps, no controlled game workload
or shader-worker contention. They do not measure the optimization's before/after benefit
or establish a general FP8 advantage. Gameplay FPS, CPU frame-time distributions,
1-versus-2 worker selection and visual/temporal quality remain unmeasured.
CI separately checks host recipes, shader validity, INT8 software-Vulkan execution and
synchronization, timestamp reuse beyond eight frames, and the expected unsupported FP8
gate on software Vulkan. A green FP8 audit is not FP8 GPU execution evidence.
