#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare AMD's FSR 4.0.2 frame generation programs for RADV, from the user's own SDK 2.4.0 DLL.

Usage: prepare_fg.py DLL NEW_OUTPUT_DIR --spirv-tools DIR

Every SPIR-V module the DLL stores (raw, 416 unique) is put through the preparation the native FSR 4 upscaler
uses on FP8 programs (tools/fsr412-vk: unused-binding pruning, SSA rewrite, dead opaque locals, reverse matrix casts,
RGB storage writes padded to RGBA), plus fg_legalize.py (reverse casts in 2- and 4-subgroup workgroups; 16-bit-float
image types; 64-bit sampled images). A module the preparation leaves unchanged is not written. Each written module is named by the
SHA-256 of AMD's original, which is what the GPU harness looks up when AMD's runtime creates the shader, and must
pass `spirv-val --target-env vulkan1.3`. manifest.json records both hashes and what changed. Refuses any other DLL.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
for candidate in (HERE.parent / "fsr412-vk", HERE / "fsr412-vk"):
    if (candidate / "legalize_fp8.py").is_file():
        sys.path.insert(0, str(candidate))
        break
sys.path.insert(0, str(HERE))
from adapt_spirv import pad_rgb_writes  # noqa: E402
from fg_legalize import bridge_reverse_matrix_conversions_any, retype_half_images, retype_int64_sampled_images  # noqa: E402
from legalize_fp8 import bridge_reverse_matrix_conversions, remove_dead_opaque_locals  # noqa: E402
from prune_bindings import prune_unused_bindings  # noqa: E402

DLL_SHA256 = "d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4"
ASSET_VERSION = "KYTY_FSR4FG_VULKAN_FP8_4.0.2_2"
EXPECTED_UNIQUE = 416


def modules(data):
    """Raw SPIR-V modules stored in the DLL: magic, then instructions up to the last OpFunctionEnd."""
    magic, found, at = struct.pack("<I", 0x07230203), {}, 0
    while True:
        at = data.find(magic, at)
        if at < 0:
            break
        i, end = at + 20, None
        while i + 4 <= len(data):
            word = struct.unpack_from("<I", data, i)[0]
            op, n = word & 0xFFFF, word >> 16
            if not n:
                break
            i += 4 * n
            if op == 56:  # OpFunctionEnd; the module ends unless another OpFunction follows
                nxt = struct.unpack_from("<I", data, i)[0] & 0xFFFF if i + 4 <= len(data) else 0
                if nxt != 54:
                    end = i
                    break
        if end:
            code = data[at:end]
            found.setdefault(hashlib.sha256(code).hexdigest(), code)
        at += 4
    return found


def capabilities(code):
    w = struct.unpack("<%dI" % (len(code) // 4), code)
    caps, at = set(), 5
    while at < len(w):
        n, op = w[at] >> 16, w[at] & 0xFFFF
        if op == 17:
            caps.add(w[at + 1])
        if op == 54:
            break
        at += n
    return caps


def main():
    p = argparse.ArgumentParser()
    p.add_argument("dll", type=Path)
    p.add_argument("out", type=Path)
    p.add_argument("--spirv-tools", type=Path, required=True)
    a = p.parse_args()
    data = a.dll.read_bytes()
    if hashlib.sha256(data).hexdigest() != DLL_SHA256:
        raise SystemExit("not AMD's SDK 2.4.0 amd_fidelityfx_framegeneration_vk.dll (SHA-256 differs)")
    tools = a.spirv_tools.resolve()
    opt, val = str(tools / "spirv-opt"), str(tools / "spirv-val")
    a.out.mkdir(parents=True, exist_ok=False)
    found = modules(data)
    if len(found) != EXPECTED_UNIQUE:
        raise SystemExit("expected %d unique SPIR-V modules, found %d" % (EXPECTED_UNIQUE, len(found)))
    manifest = {"asset_version": ASSET_VERSION, "dll_sha256": DLL_SHA256, "modules": {}}
    with tempfile.TemporaryDirectory(prefix="fsr4fg-prepare-") as temp:
        temp = Path(temp)
        for original_sha, code in sorted(found.items()):
            notes, source = {}, code
            if 6022 in capabilities(code):  # CooperativeMatrixKHR: the FP8 programs
                source, dead_globals = prune_unused_bindings(source)
                (temp / "in.spv").write_bytes(source)
                r = subprocess.run([opt, "--target-env=vulkan1.3", "--skip-validation", "--ssa-rewrite", str(temp / "in.spv"),
                                    "-o", str(temp / "ssa.spv")], capture_output=True, text=True, timeout=120)
                if r.returncode:
                    raise SystemExit("%s: SSA rewrite failed: %s" % (original_sha, r.stderr))
                source, dead_locals = remove_dead_opaque_locals((temp / "ssa.spv").read_bytes())
                source, casts = bridge_reverse_matrix_conversions_any(source, bridge_reverse_matrix_conversions)
                notes.update(unused_descriptor_globals=len(dead_globals), dead_opaque_locals=len(dead_locals),
                             reverse_matrix_casts=casts)
            source, half = retype_half_images(source)
            source, int64 = retype_int64_sampled_images(source)
            source, padded = pad_rgb_writes(source)
            if half:
                notes["half_image_conversions"] = half
            if int64:
                notes["int64_image_fetches"] = int64
            if padded:
                notes["padded_rgb_writes"] = padded
            if source == code:
                continue
            (temp / "final.spv").write_bytes(source)
            r = subprocess.run([val, "--target-env", "vulkan1.3", str(temp / "final.spv")], capture_output=True, text=True,
                               timeout=120)
            if r.returncode:
                raise SystemExit("%s: prepared module fails Vulkan validation: %s" % (original_sha, r.stderr))
            (a.out / (original_sha + ".spv")).write_bytes(source)
            notes["prepared_sha256"] = hashlib.sha256(source).hexdigest()
            manifest["modules"][original_sha] = notes
    # Modules left as AMD shipped them must already be valid Vulkan SPIR-V.
    with tempfile.TemporaryDirectory(prefix="fsr4fg-check-") as temp:
        for original_sha, code in sorted(found.items()):
            if original_sha in manifest["modules"]:
                continue
            path = Path(temp) / "m.spv"
            path.write_bytes(code)
            r = subprocess.run([val, "--target-env", "vulkan1.3", str(path)], capture_output=True, text=True, timeout=120)
            if r.returncode:
                raise SystemExit("%s: unchanged module fails Vulkan validation: %s" % (original_sha, r.stderr))
    (a.out / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    (a.out / "ASSET_VERSION").write_text(ASSET_VERSION + "\n")
    counts = {"modules": len(found), "prepared": len(manifest["modules"]),
              "reverse_matrix_casts": sum(m.get("reverse_matrix_casts", 0) for m in manifest["modules"].values()),
              "half_image_programs": sum(1 for m in manifest["modules"].values() if m.get("half_image_conversions")),
              "int64_image_programs": sum(1 for m in manifest["modules"].values() if m.get("int64_image_fetches")),
              "padded_rgb_programs": sum(1 for m in manifest["modules"].values() if m.get("padded_rgb_writes"))}
    print(json.dumps(counts))


if __name__ == "__main__":
    main()
