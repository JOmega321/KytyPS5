# Repository guidance

## Scope and project

This file applies to this Git repository. In the current workspace the repository is `source/`; sibling
`downloaded/` and `FSR4 FG TEST/` directories contain downloaded builds. Run Git and build commands from the
repository root. Read any deeper `AGENTS.md` before editing files in its directory.

This is `JOmega321/KytyPS5`, incorporating chenxiao07's Demon's Souls work. It is a C++ PS5 emulator with Vulkan
1.3 rendering, HLE system libraries and native x86-64 guest execution. The supported game configurations here are
Demon's Souls PPSA01341 versions 01.007.000 and 01.005.000; other games and versions are untested.

Windows is the primary platform. Shared-code changes must preserve Windows support; Linux-specific code needs
appropriate platform guards. Use the toolchain versions documented in [README.md](README.md) and the workflows.

## Repository map

| Location | Purpose |
| --- | --- |
| `src/loader`, `src/kernel`, `src/common` | Guest loading, memory, threads, files and platform helpers |
| `src/libs` | HLE libraries, audio, input, saves and system services |
| `src/graphics/guest_gpu` | PM4 parsing, guest GPU state and graphics execution |
| `src/graphics/shader/recompiler` | RDNA2 decode, IR and SPIR-V emission |
| `src/graphics/host_gpu` | Vulkan renderer, resource caches, memory tracking and pipelines |
| `src/graphics/presentation` | Swapchain, window, video output, overlays and presentation |
| `src/local` | Fork-specific performance switches, upscalers, frame generation, HUD and APR helpers |
| `launcher-linux`, `launcher-windows` | Platform launchers; the Linux launcher uses SDL2/ImGui |
| `tools`, `tests`, `packaging/chen-appimage` | Preparation/profiling tools, tests and AppImage packaging |
| `3rdparty` | Pinned dependencies and submodules |

## Preserve the fork's behavior

- Keep native Linux FSR4 INT8/FP8 upscaling and FSR4 frame generation separate. Preserve the FSR3 fallback,
  capability checks, pinned DLL hashes, asset-version checks, startup self-test and active-generator diagnostics.
- Keep AMD DLLs, extracted programs and model weights out of commits and packages. Ship preparation tools;
  users supply/download the supported inputs through the existing workflow.
- Preserve HUD-canvas detection for both supported game versions. Canvas-only hashes for 01.005.000 must not
  enable the 01.007.000 vertex-layout patches.
- Generated frames use submit-based pacing by default. Keep `KYTY_FG_PACE_GPU=1` available for comparison.
  Present-call timing alone does not establish actual display-flip pacing.
- Preserve Linux APR file retention, bounded asset prefetch and their runtime disable switches.
- Keep FSR4 Setup alongside the Linux keyboard editor. Align navigation/screenshot indices and filenames with
  CI when changing launcher pages. Optional keyboard prompt replacement supports an extracted 01.007.000
  folder only; the emulator's 01.005.000 support does not extend that optional feature to 01.005.000 or `.zar` files.
- Runtime behavior uses `KYTY_*` switches whose defaults are release settings. Boolean optimization paths use
  `0` to disable them; numeric modes must respect their declared ranges. Invalid values can abort.
- Shader/pipeline caches are scoped by game version and GPU/driver. Version the relevant cache key when changing
  SPIR-V emitted for already-cached shaders. Keep precompile-only speedups on the tool side where possible.
- Measure before optimizing and prefer stability. Read the
  [performance playbook](.claude/skills/ps5-emu-optimization/SKILL.md) before performance or reverse-engineering work.

## Linux build

The native launcher is `kyty_launcher` in `launcher-linux/`; the separate Qt launcher is disabled by
`build-linux.sh`. The wrapper builds Release with O3, ThinLTO and `x86-64-v3`. `KYTY_MARCH=native` is for local
CPU-specific builds, not portable packages.

Prerequisites include Git, Clang/Clang++, `lld`, CMake, Ninja, `glslangValidator`, Python 3 and the SDL development
dependencies listed in README.md. The wrapper initializes missing submodules; CMake fetches additional pinned
dependencies. Source archives without `.git` are not supported by the wrapper.

Source and build directories may contain spaces. To select a separate build directory:

```sh
KYTY_BUILD_DIR=/tmp/kyty-build ./build-linux.sh
KYTY_BUILD_DIR=/tmp/kyty-build ./build-linux.sh kyty_shader_precompile fsr4fg-gpu
```

Use the same build directory for later targets. `KYTY_CMAKE_ARGS` affects the wrapper's first configure only;
use a fresh directory when changing configuration. Windows uses `build-windows.cmd` and clang-cl, not MSVC `cl.exe`.

Linux PGO uses `tools/pgo/kyty-linux.profdata`; `tools/pgo/kyty.profdata` is the Windows profile. Check compiler/profile
compatibility. Record compiler and optimization settings when reporting validation or measurements.

## Validation

Choose existing checks appropriate to the changed behavior. Documentation-only edits normally need diff and
link checks. Build the required test targets before running CTest; many tests are excluded from the default build.
Configure with `BUILD_TESTING=ON` for registered tests. For example:

```sh
cmake --build /tmp/kyty-build --target kyty_launcher_prompts_tests --parallel 2
ctest --test-dir /tmp/kyty-build -R '^launcher_keyboard_prompts$' --output-on-failure
python3 -I tests/fsr4fg-integration.py
python3 -I tests/fsr412-vk-extract.py
python3 -I tests/fsr412-host-audit.py
```

Follow existing CMake patterns for new tests: use a small standalone target where possible, or
`add_kyty_full_emulator_test` when the full emulator is required. Register with CTest and connect the test to the
appropriate aggregate target or CI step. Some FSR/tooling tests require DLLs, prepared assets or a packaged build;
read their usage before running them. Run isolated Python tooling with `python3 -I`.

For launcher changes, check `--dry-run` and screenshots as appropriate. Create the screenshot output folder first.
Keep tests isolated from the user's game files, saves, caches and settings. Format changed C++ with the pinned
clang-format version in `.pre-commit-config.yaml`.

Distinguish source/contract checks, SPIR-V validation, actual GPU execution and gameplay quality. Report what was
tested and what remains unverified. Leave gameplay to the user unless requested. Never overwrite a running
emulator executable, and do not compile while collecting frame-rate measurements.

## Upstream integration and publishing

- `main` is this fork's default branch, formerly `chen-kytyps5-fsr4-upscaler`. Inspect the current working tree,
  remotes and history before changes, and preserve unrelated local work and untracked files.
- Chen's integration branch is `experiment/perf-40fps-20260926` at `https://github.com/chenxiao07/KytyPS5.git`.
  Fetch by URL when no remote is configured. Check current refs and the
  [integration notes](docs/RELEASE-NOTES.md) rather than assuming a previous session's hashes are current.
- Official `KytyPS5/KytyPS5` is a separate, heavily diverged upstream. Keep its integration separate from Chen
  updates unless requested. Resolve Chen merge conflicts by retaining this fork's native FSR4, launcher,
  packaging, cache and APR behavior alongside the upstream fixes.
- Use `Area: what changed` commit subjects and the actual contributor's identity. Disclose AI assistance in
  assisted contributions. PR/issue text must come from the human contributor; follow README.md's AI Use policy.
- Verify the remote head before publishing and stay within the user's authorized scope. Preserve existing history.
- `.github/workflows/chen-appimage.yml` builds on pushes to `main` and manual dispatch. Tagged Windows/Linux
  release workflows use `v*` tags or manual dispatch; consult each workflow for its exact triggers.
- Verify CI for the exact commit before handing over a release build. Check artifact digests, `SHA256SUMS` and
  `BUILD-INFO.json` when delivering downloads. AppImage state must remain in its writable data folder across updates.

## Further references

- [Linux launcher](launcher-linux/README.md): settings, keyboard prompts and FSR4 Setup.
- [AppImage packaging](packaging/chen-appimage/README.md): downloads, runtime layout and persistent state.
- [Native FSR4 upscaling](docs/FSR412-LINUX.md) and
  [frame generation](tools/fsr4fg-vk/README.md): separate setup and validation requirements.
- [Local tools](tools/local/README.md): profiling, benchmarks and shader precompilation.
