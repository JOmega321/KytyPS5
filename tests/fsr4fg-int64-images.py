#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""fg_legalize.retype_int64_sampled_images: Mesa has no 64-bit texture type, so spirv_to_nir fails on a 64-bit sampled
image and RADV crashes on the NULL it gets back. AMD's frame generation has one such program, reading an R32G32_UINT
view. The fetch must become 32-bit and give back exactly R | G << 32; other programs must not change at all.

Usage: python3 -I tests/fsr4fg-int64-images.py [SPIRV_VAL]
Uses a hand-assembled module, so it needs none of AMD's files. With SPIRV_VAL, both modules are validated too.
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools/fsr4fg-vk"))
from fg_legalize import decode, encode, retype_int64_sampled_images  # noqa: E402

VOID, FN, U32, U64, UVEC2, U64VEC4, IMAGE, POINTER, VARIABLE, C0, COORD, LABEL, LOADED, FETCH, EXTRACT, MAIN = range(1, 17)
OP_TYPE_INT, OP_TYPE_IMAGE, OP_FETCH, OP_READ = 21, 25, 95, 98


def text(s):
    b = s.encode() + b"\0"
    b += b"\0" * (-len(b) % 4)
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


def module(sampled=1, image_format=0, signed=0, consumer=OP_FETCH, result_type=U64VEC4, twin=False):
    ins = [
        [17, 1], [17, 11], [17, 5016], [10, *text("SPV_EXT_shader_image_int64")], [14, 0, 1],
        [15, 5, MAIN, *text("main"), VARIABLE], [16, MAIN, 17, 1, 1, 1],
        [71, VARIABLE, 34, 0], [71, VARIABLE, 33, 0],
        [19, VOID], [33, FN, VOID], [OP_TYPE_INT, U32, 32, 0], [OP_TYPE_INT, U64, 64, signed],
        [23, UVEC2, U32, 2], [23, U64VEC4, U64, 4],
        [OP_TYPE_IMAGE, IMAGE, U64, 1, 0, 0, 0, sampled, image_format],
    ]
    bound = MAIN + 1
    if twin:
        ins.append([OP_TYPE_IMAGE, bound, U32, 1, 0, 0, 0, sampled, image_format])
        bound += 1
    ins += [
        [32, POINTER, 0, IMAGE], [59, POINTER, VARIABLE, 0], [43, U32, C0, 0], [44, UVEC2, COORD, C0, C0],
        [54, VOID, MAIN, 0, FN], [248, LABEL], [61, IMAGE, LOADED, VARIABLE],
        [consumer, result_type, FETCH, LOADED, COORD, *([2, C0] if consumer == OP_FETCH else [])],
    ]
    if result_type == U64VEC4:
        ins.append([81, U64, EXTRACT, FETCH, 0])
    ins += [[253], [56]]
    return encode([0x07230203, 0x00010500, 0, bound, 0], ins)


def body(code):
    header, ins = decode(code)
    start = next(i for i, a in enumerate(ins) if a[0] & 0xFFFF == 54)
    return header, ins, ins[start:]


def evaluate(code, r, g):
    """Runs the instructions after the fetch for a texel (r, g, 0, 1): what the program would see."""
    _, ins, function = body(code)
    values = {a[2]: a[3] | a[4] << 32 for a in ins if a[0] & 0xFFFF == 43 and len(a) == 5}
    values.update({a[2]: a[3] for a in ins if a[0] & 0xFFFF == 43 and len(a) == 4})
    for a in function:
        op = a[0] & 0xFFFF
        if op == OP_FETCH:
            values[a[2]] = [r, g, 0, 1]
        elif op == 81:
            values[a[2]] = values[a[3]][a[4]]
        elif op == 113:
            values[a[2]] = values[a[3]]
        elif op == 196:
            values[a[2]] = (values[a[3]] << values[a[4]]) & (2 ** 64 - 1)
        elif op == 197:
            values[a[2]] = values[a[3]] | values[a[4]]
        elif op == 80:
            values[a[2]] = [values[x] for x in a[3:]]
    return values


def refused(**shape):
    try:
        retype_int64_sampled_images(module(**shape))
    except ValueError as e:
        return str(e)
    raise SystemExit("not refused: %s" % shape)


def main():
    source = module()
    prepared, fetches = retype_int64_sampled_images(source)
    assert fetches == 1
    header, ins, function = body(prepared)
    image = next(a for a in ins if a[0] & 0xFFFF == OP_TYPE_IMAGE)
    assert image[2] == U32 and image[7] == 1, "the image type must become 32-bit sampled"
    fetch = next(a for a in function if a[0] & 0xFFFF == OP_FETCH)
    assert fetch[1] != U64VEC4 and fetch[2] != FETCH and fetch[3] == LOADED, "the fetch must return 32-bit values"
    assert fetch[4:] == [COORD, 2, C0], "the fetch must keep its coordinate and level"
    assert header[3] > MAIN + 1, "new ids must raise the bound"

    # R is the low word: what AMD's splatting pass packs and the reader takes apart.
    values = evaluate(prepared, 0x89ABCDEF, 0x01234567)
    assert values[FETCH] == [0x0123456789ABCDEF, 0, 0, 1], [hex(v) for v in values[FETCH]]
    assert values[EXTRACT] == 0x0123456789ABCDEF
    assert evaluate(prepared, 0xFFFFFFFF, 0xFFFFFFFF)[EXTRACT] == 2 ** 64 - 1
    assert evaluate(prepared, 0, 1)[EXTRACT] == 1 << 32

    # A 64-bit storage image is fine as it is (AMD's splatting pass has one): the bytes must not change.
    storage = module(sampled=2, image_format=40)
    assert retype_int64_sampled_images(storage) == (storage, 0)

    assert "unsigned" in refused(signed=1)
    assert "unsigned" in refused(image_format=40)
    assert "opcode %d" % OP_READ in refused(consumer=OP_READ)
    assert "result type" in refused(result_type=UVEC2)
    assert "duplicate" in refused(twin=True)

    if len(sys.argv) > 1:
        with tempfile.TemporaryDirectory(prefix="fsr4fg-int64-") as temp:
            for name, code in (("source", source), ("prepared", prepared)):
                path = Path(temp) / (name + ".spv")
                path.write_bytes(code)
                r = subprocess.run([sys.argv[1], "--target-env", "vulkan1.3", str(path)], capture_output=True, text=True)
                assert r.returncode == 0, "%s module: %s" % (name, r.stderr)
    print("fsr4fg 64-bit sampled images: OK")


if __name__ == "__main__":
    main()
