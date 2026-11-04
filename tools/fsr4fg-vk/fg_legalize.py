#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""SPIR-V legalization the FSR 4.0.2 frame generation programs need beyond tools/fsr412-vk's FP8 preparation.

1. bridge_reverse_matrix_conversions_any: NV_cooperative_matrix2 only converts accumulator -> A/B. The SDK also
   converts B -> accumulator (16x16, subgroup scope). tools/fsr412-vk/legalize_fp8.py bridges that through
   workgroup memory for one-subgroup workgroups (32x1x1); this delegates those unchanged, so upscaler assets stay
   byte-identical. For 32x1x2 and 32x1x4 workgroups each subgroup gets its own 256-element slice, chosen by
   SubgroupId, its lanes by SubgroupLocalInvocationId, and synchronizes with subgroup-scope barriers. FP8/FP16 to
   FP32 is exact, so values are unchanged.
2. retype_half_images: Vulkan needs a 32-bit sampled type for images (VUID-StandaloneSpirv-OpTypeImage-04656). Image
   types with a 16-bit float sampled type become 32-bit; each read's result is converted back to the 16-bit vector
   the program uses and each write's texel is converted up. Duplicate type declarations created by the change are
   merged. Reads through a sampler may round differently from a native half fetch.
3. retype_int64_sampled_images: one program fetches a 64-bit integer sampled image. AMD's view of it is R32G32_UINT,
   and Mesa has no 64-bit texture type, so spirv_to_nir fails on it (RADV then crashes on the NULL it gets back). The
   image type becomes 32-bit and each fetch rebuilds the value as R | G << 32. The value is exactly the same.
Pure standard library; refuses any shape it was not written for.
"""
import struct

OP_NAME, OP_MEMBER_NAME, OP_DECORATE, OP_MEMBER_DECORATE, OP_DECORATE_ID = 5, 6, 71, 72, 332
TYPE_OPERANDS = {23: [2], 24: [2], 25: [2], 27: [2], 28: [2, 3], 29: [2], 32: [3], 4456: [2, 3, 4, 5, 6]}
# Opcodes whose first operand is a result type: from SPIRV-Headers 86f980c731e6 (spirv.core.grammar.json), the
# revision tools/fsr412-vk/build-fp8-tools.sh pins.
RESULT_TYPED = frozenset((
    1, 12, 41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 54, 55, 57, 59, 60, 61, 65, 66, 67, 68, 69, 70, 77, 78,
    79, 80, 81, 82, 83, 84, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 100, 101, 102, 103, 104, 105,
    106, 107, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 126, 127, 128,
    129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 151, 152, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171,
    172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 194,
    195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 207, 208, 209, 210, 211, 212, 213, 214, 215, 227,
    229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 245, 259, 261, 262, 263, 264, 265,
    266, 267, 268, 269, 270, 271, 274, 275, 276, 277, 278, 279, 282, 283, 284, 285, 286, 291, 292, 293, 294,
    295, 296, 299, 300, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 318, 320, 321,
    323, 324, 325, 326, 328, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348,
    349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364, 365, 366, 400, 401, 402,
    403, 4160, 4161, 4162, 4164, 4166, 4181, 4183, 4184, 4195, 4418, 4419, 4420, 4421, 4422, 4423, 4424, 4425,
    4427, 4428, 4429, 4430, 4431, 4432, 4433, 4434, 4447, 4450, 4451, 4452, 4453, 4454, 4455, 4457, 4459, 4460,
    4461, 4462, 4463, 4477, 4479, 4480, 4481, 4482, 4483, 4497, 4500, 4501, 4502, 4503, 4505, 4540, 4541, 4542,
    4545, 5000, 5001, 5002, 5003, 5004, 5005, 5006, 5007, 5011, 5012, 5056, 5074, 5078, 5090, 5101, 5110, 5111,
    5119, 5126, 5129, 5147, 5148, 5158, 5159, 5252, 5253, 5254, 5255, 5257, 5258, 5265, 5267, 5268, 5269, 5270,
    5271, 5272, 5273, 5274, 5275, 5276, 5277, 5278, 5283, 5289, 5292, 5293, 5296, 5300, 5301, 5302, 5307, 5308,
    5320, 5322, 5323, 5324, 5325, 5326, 5327, 5328, 5329, 5330, 5331, 5332, 5333, 5334, 5340, 5345, 5346, 5347,
    5348, 5349, 5350, 5351, 5352, 5359, 5361, 5362, 5363, 5366, 5367, 5369, 5372, 5373, 5374, 5375, 5376, 5377,
    5378, 5379, 5381, 5382, 5384, 5390, 5391, 5392, 5393, 5394, 5395, 5396, 5398, 5427, 5428, 5429, 5430, 5431,
    5432, 5433, 5434, 5435, 5436, 5437, 5438, 5439, 5571, 5572, 5573, 5574, 5575, 5577, 5580, 5585, 5586, 5587,
    5588, 5589, 5590, 5591, 5592, 5593, 5594, 5595, 5596, 5597, 5598, 5600, 5601, 5610, 5611, 5614, 5615, 5631,
    5699, 5713, 5714, 5715, 5716, 5717, 5718, 5719, 5720, 5721, 5722, 5723, 5724, 5725, 5726, 5727, 5728, 5729,
    5730, 5731, 5732, 5733, 5734, 5735, 5736, 5737, 5738, 5739, 5740, 5741, 5742, 5743, 5744, 5745, 5746, 5747,
    5748, 5749, 5750, 5751, 5752, 5753, 5754, 5755, 5756, 5757, 5758, 5759, 5760, 5761, 5762, 5763, 5764, 5765,
    5766, 5767, 5768, 5769, 5770, 5771, 5772, 5773, 5774, 5775, 5776, 5777, 5778, 5779, 5780, 5781, 5782, 5783,
    5784, 5785, 5786, 5787, 5788, 5789, 5790, 5791, 5792, 5793, 5794, 5795, 5796, 5797, 5798, 5799, 5800, 5801,
    5802, 5803, 5804, 5805, 5806, 5807, 5808, 5809, 5810, 5811, 5812, 5813, 5814, 5815, 5816, 5818, 5819, 5840,
    5841, 5842, 5843, 5846, 5847, 5848, 5849, 5850, 5851, 5852, 5853, 5854, 5855, 5856, 5857, 5858, 5859, 5860,
    5861, 5862, 5863, 5864, 5865, 5866, 5867, 5868, 5869, 5870, 5871, 5872, 5873, 5874, 5875, 5876, 5877, 5878,
    5879, 5880, 5881, 5882, 5923, 5924, 5925, 5926, 5927, 5928, 5929, 5930, 5931, 5932, 5933, 5934, 5938, 5949,
    6016, 6017, 6018, 6019, 6020, 6021, 6022, 6023, 6024, 6025, 6026, 6027, 6028, 6029, 6030, 6031, 6032, 6035,
    6116, 6117, 6145, 6163, 6165, 6208, 6237, 6242, 6244, 6251, 6252, 6253, 6254, 6258, 6401, 6402, 6403, 6404,
    6405, 6406, 6407, 6408, 6426, 6428, 6529, 6530, 6531, 6916, 6917, 6918,
))


def decode(code):
    if len(code) < 20 or len(code) % 4:
        raise ValueError("malformed SPIR-V byte count")
    w = list(struct.unpack("<%dI" % (len(code) // 4), code))
    if w[0] != 0x07230203:
        raise ValueError("not SPIR-V")
    out, at = [], 5
    while at < len(w):
        n = w[at] >> 16
        if not n or n > len(w) - at:
            raise ValueError("malformed SPIR-V instruction")
        out.append(list(w[at:at + n]))
        at += n
    return w[:5], out


def encode(header, instructions):
    words = list(header)
    for a in instructions:
        a[0] = (len(a) << 16) | (a[0] & 0xFFFF)
        words.extend(a)
    return struct.pack("<%dI" % len(words), *words)


def local_size(instructions):
    return next((tuple(a[3:6]) for a in instructions if a[0] & 0xFFFF == 16 and len(a) == 6 and a[2] == 17), None)


def bridge_reverse_matrix_conversions_any(code, single_subgroup_bridge):
    """single_subgroup_bridge: tools/fsr412-vk legalize_fp8.bridge_reverse_matrix_conversions."""
    header, ins = decode(code)
    local = local_size(ins)
    if local in (None, (32, 1, 1)):
        return single_subgroup_bridge(code)
    constants, floats, encodings, integers, matrices, pointer_types, value_types, builtins = {}, {}, {}, {}, {}, {}, {}, {}
    for a in ins:
        op = a[0] & 0xFFFF
        if op == 43 and len(a) == 4:
            constants[a[2]] = (a[1], a[3])
        elif op == 22:
            floats[a[1]] = a[2]
            if len(a) > 3:
                encodings[a[1]] = a[3]
        elif op == 21:
            integers[a[1]] = tuple(a[2:])
        elif op == 4456:
            matrices[a[1]] = tuple(a[2:])
        elif op == 32:
            pointer_types[a[1]] = tuple(a[2:])
        elif op == OP_DECORATE and len(a) == 4 and a[2] == 11:
            builtins[a[3]] = a[1]
        if op in RESULT_TYPED and len(a) >= 3:
            value_types[a[2]] = a[1]

    def number(i):
        return constants.get(i, (None, None))[1]

    casts = []
    for a in ins:
        if a[0] & 0xFFFF != 115:
            continue
        dst, src = matrices.get(a[1]), matrices.get(value_types.get(a[3]))
        if dst and src and number(src[4]) in (0, 1) and number(dst[4]) == 2:
            if floats.get(src[0]) == 8 and encodings.get(src[0]) != 4214:
                raise ValueError("only FP8 E4M3 reverse casts are supported")
            if (local[0] != 32 or local[1] != 1 or local[2] not in (2, 4) or number(src[1]) != 3 or src[1:4] != dst[1:4]
                    or number(src[2]) != 16 or number(src[3]) != 16 or floats.get(src[0]) not in (8, 16)
                    or floats.get(dst[0]) != 32):
                raise ValueError("reverse matrix cast outside the 16x16 FP8/FP16 -> FP32 subgroup case")
            casts.append((a[2], src))
    if not casts:
        return code, 0
    next_id = header[3]
    capabilities, annotations, declarations, new_globals = [], [], [], []

    def fresh():
        nonlocal next_id
        next_id += 1
        return next_id - 1

    def mk(op, *args):
        return [op, *args]

    def declare(op, *args):  # a type: [op, result, operands...]
        i = fresh()
        declarations.append(mk(op, i, *args))
        return i

    def declare_value(op, type_, *args):  # a constant or variable: [op, type, result, operands...]
        i = fresh()
        declarations.append(mk(op, type_, i, *args))
        return i

    caps = {a[1] for a in ins if a[0] & 0xFFFF == 17}
    for cap in (9, 61):  # Float16, GroupNonUniform (SubgroupId, SubgroupLocalInvocationId)
        if cap not in caps:
            capabilities.append(mk(17, cap))
    uint = next((i for i, t in integers.items() if t == (32, 0)), None)
    if uint is None:
        raise ValueError("program has no uint32 type")
    half = next((i for i, b in floats.items() if b == 16), None) or declare(22, 16)
    f32 = next(i for i, b in floats.items() if b == 32)

    def const(value):
        found = next((i for i, p in constants.items() if p == (uint, value)), None)
        if found is None:
            found = declare_value(43, uint, value)
            constants[found] = (uint, value)
        return found

    def builtin(number_):
        if number_ in builtins:
            return builtins[number_]
        p_in = next((i for i, t in pointer_types.items() if t == (1, uint)), None)
        if p_in is None:
            p_in = declare(32, 1, uint)
            pointer_types[p_in] = (1, uint)
        var = declare_value(59, p_in, 1)
        annotations.append(mk(OP_DECORATE, var, 11, number_))
        new_globals.append(var)
        builtins[number_] = var
        return var

    subgroup_id, subgroup_lane = builtin(40), builtin(41)
    zero, c8, c16 = const(0), const(8), const(16)
    size = const(256 * local[2])
    subgroup, workgroup, semantics = const(3), const(2), const(0x108)
    arr_half, arr_float = declare(28, half, size), declare(28, f32, size)
    p_arr_half, p_arr_float = declare(32, 4, arr_half), declare(32, 4, arr_float)
    p_half, p_float = declare(32, 4, half), declare(32, 4, f32)
    buf_half = declare_value(59, p_arr_half, 4)
    buf_float = declare_value(59, p_arr_float, 4)
    new_globals += [buf_half, buf_float]
    half_matrix = {}
    for result, src in casts:
        target = (half, *src[1:])
        m = next((i for i, t in matrices.items() if t == target), None)
        if m is None:
            m = declare(4456, *target)
            matrices[m] = target
        half_matrix[result] = (m, src[0])
    offsets = [const(32 * k) for k in range(8)]
    out, done_caps, done_ann, done_decl = [], False, False, False
    for a in ins:
        op = a[0] & 0xFFFF
        if not done_caps and op != 17:
            out += capabilities
            done_caps = True
        if not done_ann and (19 <= op <= 39 or op == 5341):
            out += annotations
            done_ann = True
        if not done_decl and op == 54:
            out += declarations
            done_decl = True
        if op == 15:
            a = a + [g for g in new_globals if g not in a[3:]]
        if op != 115 or a[2] not in half_matrix:
            out.append(a)
            continue
        m, component = half_matrix[a[2]]

        def emit(o, *args):
            out.append(mk(o, *args))

        def value(o, t, *args):
            i = fresh()
            emit(o, t, i, *args)
            return i

        source = a[3]
        if floats.get(component) == 8:
            source = value(115, m, source)
        sub = value(61, uint, subgroup_id)
        lane = value(61, uint, subgroup_lane)
        base = value(196, uint, sub, c8)  # 256 * SubgroupId
        emit(4458, value(65, p_half, buf_half, base), source, zero, c16)  # row-major store, stride 16
        emit(224, subgroup, workgroup, semantics)
        for offset in offsets:
            j = value(128, uint, value(128, uint, base, lane), offset)
            h = value(61, half, value(65, p_half, buf_half, j))
            emit(62, value(65, p_float, buf_float, j), value(115, f32, h))
        emit(224, subgroup, workgroup, semantics)
        emit(4457, a[1], a[2], value(65, p_float, buf_float, base), zero, c16)
        emit(224, subgroup, workgroup, semantics)
    header = list(header)
    header[3] = next_id
    return encode(header, out), len(casts)


READS = {87, 88, 95, 96, 97, 98}  # SampleImplicitLod, SampleExplicitLod, Fetch, Gather, DrefGather, Read


def retype_half_images(code):
    header, ins = decode(code)
    floats = {a[1]: a[2] for a in ins if a[0] & 0xFFFF == 22}
    half = next((i for i, b in floats.items() if b == 16), None)
    f32 = next((i for i, b in floats.items() if b == 32), None)
    retyped = {a[1] for a in ins if a[0] & 0xFFFF == 25 and a[2] == half} if half is not None else set()
    if not retyped:
        return code, 0
    if f32 is None:
        raise ValueError("16-bit image types without a 32-bit float type")
    for a in ins:
        if a[0] & 0xFFFF == 25 and a[2] == half:
            a[2] = f32
    remap = {}
    while True:  # merge type declarations made identical by the retype
        seen, changed = {}, False
        for a in ins:
            op = a[0] & 0xFFFF
            if op not in TYPE_OPERANDS and op not in (21, 22):
                continue
            for p in TYPE_OPERANDS.get(op, []):
                while p < len(a) and a[p] in remap:
                    a[p] = remap[a[p]]
            if op in (28, 29):  # arrays may legally repeat (different strides): never merged
                continue
            key = (op, tuple(a[2:]))
            if key in seen and seen[key] != a[1]:
                remap[a[1]] = seen[key]
                a[0] = 0
                changed = True
            else:
                seen[key] = a[1]
        ins = [a for a in ins if a[0] != 0]
        if not changed:
            break

    def rid(i):
        while i in remap:
            i = remap[i]
        return i

    ins = [a for a in ins if not (a[0] & 0xFFFF in (OP_NAME, OP_MEMBER_NAME, OP_DECORATE, OP_MEMBER_DECORATE, OP_DECORATE_ID)
                                  and a[1] in remap)]
    for a in ins:
        op = a[0] & 0xFFFF
        if op in RESULT_TYPED and len(a) >= 3:
            a[1] = rid(a[1])
        if op in (30, 33):  # struct members, function type
            a[2:] = [rid(x) for x in a[2:]]
    next_id = header[3]
    types, image_of, vectors = {}, {}, {}
    for a in ins:
        op = a[0] & 0xFFFF
        if op == 23:
            vectors[(a[2], a[3])] = a[1]
        elif op == 27:
            image_of[a[1]] = a[2]
        if op in RESULT_TYPED and len(a) >= 3:
            types[a[2]] = a[1]
    vector_of = {v: k for k, v in vectors.items()}
    retyped = {rid(i) for i in retyped}
    new_types = []

    def vector(component, count):
        nonlocal next_id
        if (component, count) not in vectors:
            vectors[(component, count)] = next_id
            new_types.append([23, next_id, component, count])
            next_id += 1
        return vectors[(component, count)]

    def image_type(v):
        t = types.get(v)
        return image_of.get(t, t)

    out, conversions = [], 0
    for a in ins:
        op = a[0] & 0xFFFF
        if op in READS and image_type(a[3]) in retyped:
            component, count = vector_of.get(a[1], (None, 0))
            if component == f32:  # already a 32-bit result (now consistent with the image type)
                out.append(a)
                continue
            if component != half:
                raise ValueError("read of a retyped image into a %s result" % (component,))
            wide = next_id
            next_id += 1
            narrow_type, original = a[1], a[2]
            a = list(a)
            a[1], a[2] = vector(f32, count), wide
            out += [a, [115, narrow_type, original, wide]]
            conversions += 1
            continue
        if op == 99 and image_type(a[1]) in retyped:
            component, count = vector_of.get(types.get(a[3]), (None, 0))
            if component == half:
                up = next_id
                next_id += 1
                out.append([115, vector(f32, count), up, a[3]])
                a = list(a)
                a[3] = up
                conversions += 1
        out.append(a)
    if new_types:
        first = next(i for i, a in enumerate(out) if a[0] & 0xFFFF == 54)
        out[first:first] = new_types
    header = list(header)
    header[3] = next_id
    return encode(header, out), conversions


def retype_int64_sampled_images(code):
    header, ins = decode(code)
    integers = {a[1]: tuple(a[2:]) for a in ins if a[0] & 0xFFFF == 21}
    images = {a[1]: a for a in ins if a[0] & 0xFFFF == 25}
    retyped = {i for i, a in images.items() if integers.get(a[2], (0, 0))[0] == 64 and a[7] == 1}
    if not retyped:
        return code, 0
    uint = next((i for i, t in integers.items() if t == (32, 0)), None)
    if uint is None:
        raise ValueError("64-bit sampled image without a uint32 type")
    wide = {images[i][2] for i in retyped}
    if len(wide) != 1 or integers[next(iter(wide))] != (64, 0) or any(images[i][8] != 0 for i in retyped):
        raise ValueError("64-bit sampled image other than an unsigned one with Unknown format")
    u64 = next(iter(wide))
    for i in retyped:
        twin = images[i][3:]
        if any(j not in retyped and a[2] == uint and a[3:] == twin for j, a in images.items()):
            raise ValueError("retyping a 64-bit sampled image would duplicate an image type")
        images[i][2] = uint
    vectors = {(a[2], a[3]): a[1] for a in ins if a[0] & 0xFFFF == 23}
    constants = {a[3]: a[2] for a in ins if a[0] & 0xFFFF == 43 and a[1] == u64 and len(a) == 5 and a[4] == 0}
    loaded = {a[2] for a in ins if a[0] & 0xFFFF == 61 and a[1] in retyped}
    next_id, declarations = header[3], []

    def fresh():
        nonlocal next_id
        next_id += 1
        return next_id - 1

    def constant(value):
        if value not in constants:
            constants[value] = fresh()
            declarations.append([43, u64, constants[value], value, 0])
        return constants[value]

    if (uint, 4) not in vectors:
        vectors[(uint, 4)] = fresh()
        declarations.append([23, vectors[(uint, 4)], uint, 4])
    uvec4 = vectors[(uint, 4)]
    zero, one, shift = constant(0), constant(1), constant(32)
    out, fetches = [], 0
    for a in ins:
        op = a[0] & 0xFFFF
        used = set(a[1:]) & loaded
        if op != 95 or a[3] not in loaded:
            if used and not (op == 61 and a[2] in loaded):
                raise ValueError("64-bit sampled image used by opcode %d" % op)
            out.append(a)
            continue
        result_type, result = a[1], a[2]
        if result_type != vectors.get((u64, 4)):
            raise ValueError("fetch of a 64-bit sampled image into another result type")
        fetched, r, g, low, high, shifted, value = (fresh() for _ in range(7))
        out += [[95, uvec4, fetched, *a[3:]], [81, uint, r, fetched, 0], [81, uint, g, fetched, 1], [113, u64, low, r],
                [113, u64, high, g], [196, u64, shifted, high, shift], [197, u64, value, low, shifted],
                [80, result_type, result, value, zero, zero, one]]
        fetches += 1
    first = next(i for i, a in enumerate(out) if a[0] & 0xFFFF == 54)
    out[first:first] = declarations
    header = list(header)
    header[3] = next_id
    return encode(header, out), fetches
