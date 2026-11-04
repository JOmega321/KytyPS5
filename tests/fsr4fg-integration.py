#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The pieces of FSR 4 frame generation that only meet in a package must agree, and what must stay does.

Usage: python3 -I tests/fsr4fg-integration.py
Needs no GPU and none of AMD's files. Each check is something that would otherwise fail only in a user's game.
"""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def text(path):
    return (root / path).read_text()


def expect(condition, what):
    if not condition:
        raise SystemExit("FAILED: " + what)
    print("ok:", what)


# The launcher accepts the files prepare_fg.py makes.
version = re.search(r'ASSET_VERSION = "([^"]+)"', text("tools/fsr4fg-vk/prepare_fg.py")).group(1)
expect('"%s"' % version in text("launcher-linux/asset-builder.cpp"), "the launcher expects the asset version prepare_fg.py writes: " + version)

# Everything prepare_fg.py imports is in the package.
package = text("packaging/chen-appimage/package.py")
imports = re.findall(r"^from (\w+) import", text("tools/fsr4fg-vk/prepare_fg.py"), re.M)
for module in imports:
    if (root / "tools/fsr4fg-vk" / (module + ".py")).is_file() or (root / "tools/fsr412-vk" / (module + ".py")).is_file():
        expect(module + ".py" in package, "the package holds " + module + ".py")
expect("prepare_fg.py" in package and "build_assets.sh" in package, "the package holds the preparation tools")
expect("'fsr4fg-gpu'" in package, "the package holds the self-test program beside the emulator")
expect("fsr4fg-gpu" in text("src/local/fsr4-framegen.cpp"), "the emulator looks for that program")
workflow = text(".github/workflows/chen-appimage.yml")
expect(re.search(r"--target [^\n]*fsr4fg-gpu", workflow) is not None, "the release build makes it")
expect(re.search(r"tar -C _Build/appimage[^\n]*\n[^\n]*fsr4fg-gpu", workflow) is not None, "the native artifact carries it")

# The launcher asks for FSR 4 only when it is chosen, so a saved choice elsewhere cannot leak through the environment.
commands = text("launcher-linux/commands.cpp")
expect('"KYTY_FSR4FG_DIR"' in commands.split("KYTY_PIPELINE_CACHE_DIR")[0], "the launcher clears KYTY_FSR4FG_DIR before choosing")
expect('export KYTY_FSR4FG_DIR' not in text("packaging/chen-appimage/AppRun"), "AppRun does not ask for FSR 4 on its own")
block = commands[commands.index('S().Bool("fsr4_framegen")'):]
block = block[:block.index("}\n")]
expect('"KYTY_FSR4FG_DIR"' in block and '"radv_cooperative_matrix2_nv"' in block, "choosing it sets the directory and RADV's matrix setting")
expect('S().Str("fsr4_precision") == "fp8"' in commands and 'if (fp8)\n            Put(set, "radv_cooperative_matrix2_nv", "true");' in commands,
       "FP8 upscaling still sets RADV's matrix setting on its own")
expect('"radv_cooperative_matrix2_nv"' in commands.split("KYTY_PIPELINE_CACHE_DIR")[0], "the setting is still cleared for other selections")

# FSR 3 stays the fallback, and the upscaler's device setup stays as it was asked for.
frame_gen = text("src/local/frame-gen.cpp")
branch = frame_gen[frame_gen.index("if (Fsr4Fg::Requested()) {\n\t\t\tswitch"):]
expect(branch.index("Fsr4Fg::Result::Unavailable") < branch.index("FsrNative::Interpolate(graphics, command, fsr, outputs[0])"),
       "FSR 4 unavailable falls through to FSR 3 built in")
window = text("src/graphics/presentation/window/vulkanWindow.cpp")
expect("graphics.fsr412_fp8_enabled = graphics.fsr412_enabled && fp8_extensions" in window and "fp8_extensions = fp8_requested && fsr_extensions" in window,
       "asking for FSR 4 frame generation does not turn on FP8 upscaling")
expect(text("src/local/fsr4-framegen.cpp").count("AboveGuest") == 2, "AMD's image is kept above the guest's range")

# On a game version without its interface shaders listed, FSR finds no HUD canvas: they can be found and given without a build.
hud = text("src/local/hud-anchor.h")
expect("KYTY_HUD_CANVAS_SHADERS" in hud and "KYTY_HUD_SHADER_LOG" in hud, "the HUD's shaders can be logged and added on another game version")
run = text("src/graphics/guest_gpu/graphicsRun.cpp")
expect(run.count("HudAnchor::LogTargetDraw(") == run.count("HudAnchor::Classify("), "every interface draw path logs")

# Timed from the GPU's end by default, and the old timing one switch away.
present = text("src/graphics/presentation/window/swapchain.cpp")
expect("KYTY_FG_PACE_GPU" in present and "present_scheduler.Wait(frame.present_tick);" in present, "generated frames are paced from the GPU's end")
print("fsr4fg integration: OK")
