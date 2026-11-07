# Local tools

Developer tools: the Windows bench and profiling scripts (`windows/`, `bench-windows.py`), the static shader
precompile (`static-precompile/`), the Linux launcher (`run_ds.sh`) and the Linux DLSS bridge (`dlss-bridge/`).
Building, running and the developer's workflow are in [docs/CURRENT-PLAYABLE.md](../../docs/CURRENT-PLAYABLE.md).

## Running and measuring (Windows)

- `bench-windows.py`: the client of the emulator's live command file (`KYTY_LIVE_FILE`: measures, counters,
  traces, profiles) and the bench save handling (`prepare` installs a fixed save, `restore` puts the user's back).
- `windows/bench-run.ps1`: a launch to the HUD from a fixed bench save; `windows/walk-run.ps1`: the walk route;
  `windows/ab-spot.ps1`: a same-process ABBA of a runtime switch; `windows/ab-exe.ps1`: an ABBA of two builds.
- `windows/prof-spot.ps1` and `windows/profreport.cpp`: render-thread profiles; `prof-callees.py`,
  `prof-dll-callers.py`, `prof-hot-lines.py`, `prof-inline.py`, `prof-slices.py`, `guest-prof.py`: their views.
- Flicker: the live `screenshots <path> <count>` command takes the next frames small into one file; `flicker.py`
  scores each against the median of its neighbours and names the wrong ones (a frame with objects missing differs
  from both neighbours, motion only a little), with a strip of the worst beside a neighbour (numpy; PIL for the strip).
- Live captures: `live-trace.py`, `live-trace-report.py`, `walk-report.py`, `render-faults.py` (`trace`),
  `live-census-report.py`, `census-compare.py` (`census`), `persec-report.py`, `ab-counters.py`
  (measures and counters), `frame-capture-report.py` (`capture`), `slow-timeline.py` (`KYTY_SLOW_LOG_MS`).
- Hitches: the run log's SLOW Frame lines (`KYTY_HITCH_LOG_MS`, default 100 ms) carry `unix_ms`;
  `windows/record.ps1` records the window with wall-clock timestamps (ffmpeg 8+), `windows/capture.ps1` and
  `windows/screen.ps1` take stills, `hitch-sheet.py` shows the screen before and after each slow frame and
  `hitch-prof.py` splits a profile into the slow frames' samples and the rest.
- `windows/key.ps1`, `windows/keys.ps1`: input for scripted walks; `windows/record-fg.ps1`: a 30 fps game with
  DLSS frame generation, for recordings.

## Diagnostics

- `windows/stackdump.cpp`, `windows/threads.ps1`: the threads and stacks of a running emulator.
- `windows/guest-disasm.py`, `windows/pe-func.py`, `windows/vmmap.py`: guest code and the address space.
- `spirv-stats.py`, `windows/spirv-stats.cpp`: driver statistics of dumped SPIR-V (`KYTY_SHADER_DUMP`).
- `demons-souls-save.py`: Demon's Souls save editing (positions and worlds).

## Building

- [static-precompile](static-precompile/README.md): every shader and pipeline of the game from its files, for the
  static pipeline cache that `precompile-windows.ps1` (Linux: `precompile-linux.sh`) makes.
- [PGO](PGO.md): the profile the release build uses and how to retrain it (`windows/pgo-train.ps1`).

Keep reusable tools here and one-off scripts out of the repository; git keeps the history of the removed ones.

## Linux file streaming

Loose game assets use cached read-only descriptors and positional reads, reusing APR's per-thread staging buffer.
One worker at background CPU/I/O priority prefetches small audio streams (up to 16 MiB) and the first 256 KiB of
recorded upcoming assets. The queue is bounded to 32 files and 32 MiB, including the active job. Guest memory writes
still use APR's existing overwrite preparation and copy path. Archive reads keep their existing implementation.

- `KYTY_KEPT_FILES=0`: compare against the original open/read/close path.
- `KYTY_FILE_PREFETCH=0`: retain descriptors without background read-ahead.
- `KYTY_FILE_FOLLOW=0`: disable recording-based look-ahead, retaining audio-stream prefetch.
- `KYTY_FILE_OPEN_MAX`: descriptor cap (default 512, maximum 4096, further limited by the process's open-file limit).
  Active readers are never evicted; cache pressure falls back to ordinary APR reads.
- `KYTY_FILE_IDLE_CLOSE`: idle retention in seconds (default 10; 0 disables expiry, but capacity eviction still applies).
- `KYTY_FILE_FOLLOW_WINDOW`: recorded successors to warm (default 8, maximum 32).

File-order recordings live under `_PipelineCache/files/<title>_<version>.files`. New recordings benefit the next
session; shipped recordings can seed the first. Files are treated as immutable for the lifetime of a retained
descriptor. No textures, shader caches, saves, or guest synchronization rules are changed by this path.

Build `kept_files_linux_tests` and run `ctest -R kept_files_linux --output-on-failure` in the build directory.
Compare the same loading/traversal route with retention and prefetch independently disabled; count slow APR reads
and frame hitches, as well as memory pressure. A successful build is not a measured gameplay speedup.
