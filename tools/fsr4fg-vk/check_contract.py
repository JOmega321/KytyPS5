#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks a frame generation contract (analyze_fg.py) against what FSR SDK 2.4's 4.0.2 provider is known to do.

Usage: python3 -I check_contract.py CONTRACT.json FAMILY [COMMITTED.json]

FAMILY is mlfi_4k or mlfi_8k. With COMMITTED, the fresh contract must equal the committed one exactly.
"""
import json
import sys

DLL_SHA256 = "d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4"
WEIGHTS = {"size": 10657152, "dll_file_offset": 14644400,
           "sha256": "e356c70ad53bca39b27751154ddd71546e0a7c4c84003232ec43b9799bc4c748"}
HELPERS = ["FFX_MLFI_DILATE_MV_PASS", "FFX_MLFI_SPLATTING_64", "FFX_MLFI_COMPUTE_FWD_MV", "FFX_MLFI_SPLATTING_64",
           "FFX_MLFI_SPLATTING_64"]


def fail(why):
    raise SystemExit("contract check failed: " + why)


def main():
    contract = json.load(open(sys.argv[1]))
    family = sys.argv[2]
    if contract["dll_sha256"] != DLL_SHA256:
        fail("not the audited SDK 2.4.0 frame generation DLL")
    if contract["provider"] != "4.0.2":
        fail("provider %r" % contract["provider"])
    if len(contract["uploads"]) != 1 or any(contract["uploads"][0][k] != v for k, v in WEIGHTS.items()):
        fail("the weights upload is not the DLL's audited block: %s" % contract["uploads"])
    frames = contract["frames"]
    if not frames:
        fail("no frames")
    sequences = []
    for name, passes in sorted(frames.items(), key=lambda kv: int(kv[0])):
        labels = [p["label"] for p in passes]
        if labels[:5] != HELPERS:
            fail("frame %s: helper passes %s" % (name, labels[:5]))
        ml = passes[5:]
        if [p["entry"] for p in ml] != ["%s_pass%d" % (family, i) for i in range(191)]:
            fail("frame %s: the ML passes are not %s_pass0..190 in order" % (name, family))
        if [p["label"] for p in ml] != ["FFX_MLFI_PASS_%d" % i for i in range(191)]:
            fail("frame %s: pass labels" % name)
        if any(p["required_subgroup_size"] != 32 for p in ml):
            fail("frame %s: an ML pass without required subgroup size 32" % name)
        for p in passes:
            for b in p["bindings"]:
                if b["type"] == "uniform_buffer" and b["constants"] not in contract["constants"]:
                    fail("missing constants %s" % b["constants"])
        sequences.append([(p["entry"], p["shader_sha256"], tuple(p["groups"]),
                           tuple((b["name"], b.get("resource")) for b in p["bindings"])) for p in passes])
    # After the first (reset) frame every frame runs the same programs with the same group counts; only the two
    # frames' colour inputs alternate and the per-frame constants change.
    steady = [[(e, s, g) for e, s, g, _ in seq] for seq in sequences]
    if any(s != steady[0] for s in steady):
        fail("frames differ in programs or group counts")
    if len(sys.argv) > 3:
        committed = json.load(open(sys.argv[3]))
        if committed != contract:
            keys = [k for k in set(committed) | set(contract) if committed.get(k) != contract.get(k)]
            fail("differs from the committed contract in %s" % sorted(keys))
    print("contract ok: %s, %d frames x %d dispatches, weights at %d" % (family, len(frames), len(sequences[0]),
                                                                         WEIGHTS["dll_file_offset"]))


if __name__ == "__main__":
    main()
