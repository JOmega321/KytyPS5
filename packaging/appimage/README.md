# PR1747 experimental test build

This branch applies https://github.com/KytyPS5/KytyPS5/pull/1747 at head
`930d50b208427417630849a5d2e23c6da7e22776` to the existing AppImage baseline
(upstream `1be1c6c4f928e52f740ae7f76841c1da74220e54`). It retains later upstream
changes during the three-way patch application. The stable `appimage` branch
is unchanged. The integration also extends the newer baseline's BufferResource
serializer for the two added fields, with a cache round-trip regression test.
The PR is unmerged and gameplay on AMD/Linux is not established.

Download `KytyPS5-PR1747-test-x86_64` from the successful Actions run, extract
it, mark the AppImage executable, and add your game folder in the fresh launcher.

The test defaults to separate data under
`${XDG_DATA_HOME:-$HOME/.local/share}/KytyPS5-PR1747`, settings under
`${XDG_CONFIG_HOME:-$HOME/.config}/KytyPS5-PR1747/Kyty/Kyty.ini`, and XDG cache
under `${XDG_CACHE_HOME:-$HOME/.cache}/KytyPS5-PR1747`. Existing normal saves
are not migrated automatically. `KYTY_APPIMAGE_DATA_DIR`,
`KYTY_PR1747_CONFIG_HOME`, and `KYTY_PR1747_CACHE_HOME` permit explicit overrides.
The built-in directory updater remains disabled for this fork build.

CI builds from source and runs `resource_tracking` (including indexed-write
footprint regression cases) and `resource_materialization`, then checks the
packaged emulator CLI and launcher startup under Xvfb. The GPU-backed descriptor
caller test included in the PR is not run on this hosted runner. Actual Demon's
Souls gameplay and RX 9060 XT hardware rendering need verification on your PC.
