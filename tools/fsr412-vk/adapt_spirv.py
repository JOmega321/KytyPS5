#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pad RGB image writes to RGBA for native Vulkan RGBA image views.

The SDK shaders write float3 texels to unknown-format storage images. Binding an
RGBA16F image requires at least four texel components (VUID-OpImageWrite-08795).
Preserve RGB exactly and supply opaque alpha. No neural arithmetic is changed.
"""
import struct
from extract_vk import reflect


def pad_rgb_writes(code):
    reflect(code)  # Bounds/instruction validation before walking operands.
    w = list(struct.unpack('<'+'I'*(len(code)//4), code))
    types, values, instructions = {}, {}, []
    at = 5
    while at < len(w):
        n, op = w[at] >> 16, w[at] & 65535
        a = w[at+1:at+n]
        instructions.append((op, a))
        if op in (21, 22, 23):
            types[a[0]] = (op, a[1:])
        at += n
    for op, a in instructions:
        if (op >= 41 or op == 12) and op not in (71, 72, 74, 75) and len(a) >= 2 and a[0] in types:
            values[a[1]] = a[0]
    next_id = w[3]
    declarations, vectors, ones = [], {}, {}
    for type_id, (op, a) in types.items():
        if op == 23 and a[1] == 4:
            vectors[a[0]] = type_id
    for op, a in instructions:
        if op == 43 and len(a) == 3 and types.get(a[0]) == (22, [32]) and a[2] == 0x3f800000:
            ones[a[0]] = a[1]
    def ins(op, *a):
        return [(len(a)+1) << 16 | op, *a]
    output, count, inserted = w[:5], 0, False
    body = []
    for op, a in instructions:
        if op == 99:
            if len(a) < 3:
                raise ValueError('truncated OpImageWrite')
            scalar, width = None, None
            type_id = values.get(a[2])
            if type_id in types:
                kind, operands = types[type_id]
                if kind == 23:
                    scalar, width = operands
            if width == 3:
                if types.get(scalar) != (22, [32]):
                    raise ValueError('RGB image write uses an unsupported scalar type')
                if scalar not in vectors:
                    vectors[scalar] = next_id
                    next_id += 1
                    declarations += ins(23, vectors[scalar], scalar, 4)
                if scalar not in ones:
                    ones[scalar] = next_id
                    next_id += 1
                    declarations += ins(43, scalar, ones[scalar], 0x3f800000)
                result = next_id
                next_id += 1
                body += ins(80, vectors[scalar], result, a[2], ones[scalar])
                a = [a[0], a[1], result, *a[3:]]
                count += 1
        body += ins(op, *a)
    if not count:
        return code, 0
    # Types/constants must precede functions; their IDs need not be monotonically ordered.
    at = 0
    while at < len(body):
        n, op = body[at] >> 16, body[at] & 65535
        if op == 54 and not inserted:
            output += declarations
            inserted = True
        output += body[at:at+n]
        at += n
    if not inserted:
        raise ValueError('no function in shader')
    output[3] = next_id
    adapted = struct.pack('<'+'I'*len(output), *output)
    reflect(adapted)
    return adapted, count
