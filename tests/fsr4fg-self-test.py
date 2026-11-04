#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""fsr4fg-gpu --self-test: whatever the driver does to the child, the parent must report it and write a verdict; a
verdict holds only while what it tested is unchanged, and only if the failure would happen again.

Usage: python3 -I tests/fsr4fg-self-test.py FSR4FG_GPU DLL PREPARED_DIR [--hardware]
Without --hardware it needs any Vulkan driver (CI: software Vulkan, which lacks FP8, so nothing gets as far as AMD's
shaders) and injects the faults. --hardware needs a GPU that runs FSR 4: it adds a pass, generated frames made wrong on
purpose, and the real crash on AMD's 64-bit sampled image program when it is left as shipped.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types
from pathlib import Path

root = Path(__file__).resolve().parents[1]
FWD_MV_PROGRAM = "2a2a5b6951177669"
hardware = "--hardware" in sys.argv
arguments = [a for a in sys.argv[1:] if a != "--hardware"]
if len(arguments) != 3:
    raise SystemExit(__doc__)
gpu, dll, prepared = (str(Path(a).resolve()) for a in arguments)


def run(state, programs=prepared, *extra, inject=None, binary_dll=None, matrix2=None):
    env = dict(os.environ)
    env.pop("FSR4FG_INJECT", None)
    if inject:
        env["FSR4FG_INJECT"] = inject
    if matrix2 is not None:
        env["radv_cooperative_matrix2_nv"] = matrix2
    r = subprocess.run([gpu, binary_dll or dll, programs, str(state), "--self-test", *extra], env=env, capture_output=True, text=True, timeout=300)
    verdict = json.loads((Path(state) / "self-test.json").read_text()) if (Path(state) / "self-test.json").is_file() else None
    return r, verdict


def expect(condition, what, r=None):
    if not condition:
        raise SystemExit("FAILED: %s\n%s" % (what, (r.stdout + r.stderr) if r else ""))
    print("ok:", what)


def ran(r):
    return "running AMD's runtime" in r.stdout


def kept(r):
    return "kept verdict" in r.stdout


with tempfile.TemporaryDirectory(prefix="fsr4fg-self-test-") as temp:
    temp = Path(temp)

    # Anything the driver cannot do, the reason names it, and the failure is kept. (Hardware: the extension turned off.)
    state = temp / "lacks"
    r, v = run(state, matrix2="false")
    expect(r.returncode == 1 and v["verdict"] == "fail" and not v["transient"], "a driver lacking features fails", r)
    expect(v["reason"].startswith("the driver lacks: "), "the reason lists what it lacks: " + v["reason"], r)
    r, v2 = run(state, matrix2="false")
    expect(r.returncode == 1 and kept(r) and not ran(r) and v2["reason"] == v["reason"], "that verdict is kept, reason and all", r)
    r, _ = run(state, prepared, "--force", matrix2="false")
    expect(ran(r), "--force tests again", r)

    # A changed program invalidates the verdict, even with the manifest untouched.
    copy = temp / "programs"
    shutil.copytree(prepared, copy)
    program = copy / next(n for n in os.listdir(copy) if n.endswith(".spv"))
    program.write_bytes(program.read_bytes() + b"\0\0\0\0")
    r, _ = run(state, str(copy), matrix2="false")
    expect(ran(r) and not kept(r), "a changed program is tested again", r)

    # What may not happen twice is not kept.
    for fault, why in (("hang", "SIGTERM ends it"), ("hang-hard", "it ignores SIGTERM: SIGKILL ends it")):
        state = temp / fault
        r, v = run(state, prepared, "--self-test-timeout", "2", inject=fault)
        expect(r.returncode == 1 and v["verdict"] == "fail" and v["transient"], "a hang ends in a failure (%s)" % why, r)
        expect("no result within 2 s" in v["reason"], "the reason says so: " + v["reason"], r)
        r, _ = run(state, prepared, "--self-test-timeout", "2", inject=fault)
        expect(ran(r) and not kept(r), "a timeout is tested again", r)

    # A crash reports where it was and is kept.
    state = temp / "crash"
    r, v = run(state, inject="crash")
    expect(r.returncode == 1 and v["verdict"] == "fail" and not v["transient"], "a crash in the child leaves a verdict", r)
    expect(v["reason"].startswith("crashed: STOPPED by signal 11"), "the reason has the signal: " + v["reason"], r)
    r, _ = run(state, inject="crash")
    expect(kept(r), "a crash is kept", r)

    # Stopped on something the host does not serve: AMD's runtime stopping is told apart from a missing feature.
    state = temp / "fatal"
    r, v = run(state, inject="fatal")
    expect(r.returncode == 1 and v["reason"].startswith("AMD's runtime stopped: "), "a provider stop has its own reason: " + v["reason"], r)

    # Not testable at all: no verdict, a different status.
    state = temp / "refused"
    r, v = run(state, binary_dll=str(copy / "manifest.json"))
    expect(r.returncode == 2 and v is None, "another DLL is refused without a verdict", r)

    if hardware:
        state = temp / "hardware"
        r, v = run(state)
        expect(r.returncode == 0 and v["verdict"] == "pass", "AMD's runtime passes on this GPU", r)
        expect(v["margin_db"]["worst"] >= v["margin_db"]["needed"], "worst frame %.2f dB, %.1f needed" % (v["margin_db"]["worst"], v["margin_db"]["needed"]), r)
        r, _ = run(state)
        expect(r.returncode == 0 and kept(r) and not ran(r), "the pass is kept", r)

        # R and G swapped in the forward motion program: it runs, and its frames are wrong.
        source = (root / "tools/fsr4fg-vk/fg_legalize.py").read_text()
        swapped = source.replace("[81, uint, r, fetched, 0], [81, uint, g, fetched, 1]", "[81, uint, r, fetched, 1], [81, uint, g, fetched, 0]")
        if swapped == source:
            raise SystemExit("FAILED: the test no longer finds the fetch to swap in fg_legalize.py")
        broken = types.ModuleType("broken_legalize")
        exec(compile(swapped, "broken_legalize", "exec"), broken.__dict__)
        sys.path[:0] = [str(root / "tools/fsr4fg-vk"), str(root / "tools/fsr412-vk")]
        from prepare_fg import modules  # noqa: E402
        found = modules(Path(dll).read_bytes())
        name = next(n for n in found if n.startswith(FWD_MV_PROGRAM))
        wrong = temp / "wrong"
        shutil.copytree(prepared, wrong)
        (wrong / (name + ".spv")).write_bytes(broken.retype_int64_sampled_images(found[name])[0])
        r, v = run(state, str(wrong))
        expect(r.returncode == 1 and v["reason"].startswith("generated frames are wrong"), "wrong frames fail: " + v["reason"], r)

        # AMD's program as shipped: RADV 26.2.5 crashes on it. Another Mesa may cope, which is fine.
        shipped = temp / "shipped"
        shutil.copytree(prepared, shipped)
        (shipped / (name + ".spv")).unlink()
        r, v = run(state, str(shipped))
        if v["verdict"] == "fail":
            expect("compiling pipeline" in v["reason"] and FWD_MV_PROGRAM in v["reason"], "the real crash names the program: " + v["reason"], r)
        else:
            print("note: this driver now compiles AMD's 64-bit sampled image program as shipped")
print("fsr4fg self-test: OK")
