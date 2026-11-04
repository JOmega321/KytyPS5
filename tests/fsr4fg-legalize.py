#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The frame generation preparation must not change the FSR 4.1.2 upscaler's FP8 preparation.

Usage: python3 -I tests/fsr4fg-legalize.py AMD_FIDELITYFX_UPSCALER_VK_DLL
Every FP8 matrix program of the SDK 2.4.0 upscaler DLL goes through tools/fsr412-vk's reverse-cast bridge and through
fg_legalize.bridge_reverse_matrix_conversions_any; both results (or both refusals) must be identical.
"""
import hashlib
import struct
import sys
import zlib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools/fsr412-vk"))
sys.path.insert(0, str(root / "tools/fsr4fg-vk"))
from fg_legalize import bridge_reverse_matrix_conversions_any  # noqa: E402
from legalize_fp8 import bridge_reverse_matrix_conversions  # noqa: E402
from prepare_fg import capabilities  # noqa: E402

UPSCALER_SHA256 = "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"


def outcome(fn, *args):
    try:
        return fn(*args)
    except ValueError as e:
        return ("refused", str(e))


def main():
    data = Path(sys.argv[1]).read_bytes()
    assert hashlib.sha256(data).hexdigest() == UPSCALER_SHA256, "not the SDK 2.4.0 upscaler DLL"
    magic, at, programs = struct.pack("<I", 0x465A5846), 0, {}
    while True:
        at = data.find(magic, at)
        if at < 0:
            break
        size = struct.unpack_from("<I", data, at + 4)[0]
        try:
            code = zlib.decompressobj().decompress(data[at + 8:], size + 16)
        except zlib.error:
            code = b""
        at += 4
        if len(code) == size and code[:4] == struct.pack("<I", 0x07230203) and 6022 in capabilities(code):
            programs[hashlib.sha256(code).hexdigest()] = code
    assert len(programs) > 100, "expected the upscaler's FP8 matrix programs, found %d" % len(programs)
    for sha, code in sorted(programs.items()):
        a = outcome(bridge_reverse_matrix_conversions, code)
        b = outcome(bridge_reverse_matrix_conversions_any, code, bridge_reverse_matrix_conversions)
        assert a == b, "upscaler program %s prepared differently" % sha
    print("upscaler FP8 matrix programs unchanged by the frame generation preparation: %d" % len(programs))


if __name__ == "__main__":
    main()
