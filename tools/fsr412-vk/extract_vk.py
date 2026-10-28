#!/usr/bin/env python3
"""Extract SDK 2.4's compressed Vulkan programs. Does NOT convert the Windows host runtime.

Format reference: AMD SDK v2.4.0 api/internal/ffx_blob_{compression,decompress}.h
0x465a5846 followed by uint32 uncompressed size, then a zlib stream (despite
its source comment saying raw deflate). No AMD binary or model data is distributed.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct
import zlib

MAGIC = struct.pack('<I', 0x465A5846)
MAX_SHADER = 16 * 1024 * 1024
PACK_MAGIC = b'FSR4SPV\0'
RECORD = struct.Struct('<QII128s')


def u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise ValueError('truncated PE header')
    return struct.unpack_from('<I', data, offset)[0]


def sections(data):
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('expected a Windows PE64 DLL')
    pe = u32(data, 60)
    if pe + 24 > len(data) or data[pe:pe+4] != b'PE\0\0':
        raise ValueError('invalid PE signature')
    machine, count = struct.unpack_from('<HH', data, pe+4)
    optional, flags = struct.unpack_from('<HH', data, pe+20)
    if machine != 0x8664 or not flags & 0x2000 or optional < 112:
        raise ValueError('expected an x86-64 DLL')
    if pe+24+optional > len(data) or struct.unpack_from('<H', data, pe+24)[0] != 0x20b:
        raise ValueError('expected a complete PE32+ header')
    start = pe+24+optional
    if not 1 <= count <= 96 or start+count*40 > len(data):
        raise ValueError('invalid PE section table')
    result = []
    for i in range(count):
        off = start+i*40
        name = data[off:off+8].split(b'\0')[0].decode('ascii', errors='replace')
        size, raw = struct.unpack_from('<II', data, off+16)
        characteristics = u32(data, off+36)
        if raw+size > len(data):
            raise ValueError('PE section outside file')
        if size and characteristics & 0x40 and characteristics & 0x40000000:
            result.append((name, raw, raw+size))
    return result


def imports(data):
    pe = u32(data, 60)
    count = struct.unpack_from('<H', data, pe+6)[0]
    optional = struct.unpack_from('<H', data, pe+20)[0]
    table = pe+24+optional
    def offset(rva):
        for i in range(count):
            at = table+i*40
            virtual_size, address, raw_size, raw = struct.unpack_from('<IIII', data, at+8)
            if address <= rva < address+raw_size:
                result = raw+rva-address
                if result >= len(data): break
                return result
        raise ValueError('import RVA outside initialized sections')
    def string(rva):
        at = offset(rva)
        end = data.find(b'\0', at, min(at+1024, len(data)))
        if end < 0: raise ValueError('unterminated import name')
        return data[at:end].decode('ascii')
    rva = u32(data, pe+24+120)
    if not rva: return []
    at = offset(rva)
    result = []
    for _ in range(128):
        original, timestamp, chain, name, first = struct.unpack_from('<IIIII', data, at)
        if not any((original, timestamp, chain, name, first)): return result
        functions = []
        thunk = offset(original or first)
        for i in range(4096):
            entry = struct.unpack_from('<Q', data, thunk+i*8)[0]
            if not entry: break
            functions.append('#'+str(entry & 65535) if entry >> 63 else string(entry+2))
        else: raise ValueError('unterminated import table')
        result.append({'dll': string(name), 'functions': functions})
        at += 20
    raise ValueError('too many import descriptors')


def literal(words):
    return struct.pack('<'+'I'*len(words), *words).split(b'\0', 1)[0].decode('utf-8', errors='strict')


def reflect(code):
    if len(code) < 20 or len(code) % 4 or code[:4] != b'\x03\x02\x23\x07':
        raise ValueError('not SPIR-V')
    words = struct.unpack('<'+'I'*(len(code)//4), code)
    if words[4] or not 0 < words[3] <= 10000000:
        raise ValueError('invalid SPIR-V header')
    names, decorations, constants = {}, collections.defaultdict(dict), {}
    info = {'entry_points': [], 'capabilities': [], 'extensions': [], 'execution_modes': []}
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 65535
        if count == 0 or i+count > len(words):
            raise ValueError('invalid SPIR-V instruction')
        args = words[i+1:i+count]
        if op == 5 and len(args) >= 2:
            names[args[0]] = literal(args[1:])
        elif op == 15 and len(args) >= 3:
            info['entry_points'].append({'model': args[0], 'id': args[1], 'name': literal(args[2:])})
        elif op == 17 and len(args) == 1:
            info['capabilities'].append(args[0])
        elif op == 10:
            info['extensions'].append(literal(args))
        elif op in (16, 331) and len(args) >= 2:
            info['execution_modes'].append({'opcode': op, 'entry': args[0], 'mode': args[1], 'operands': list(args[2:])})
        elif op == 71 and len(args) >= 3 and args[1] in (1, 33, 34):
            decorations[args[0]][{1:'spec_id', 33:'binding', 34:'set'}[args[1]]] = args[2]
        elif op in (43, 50) and len(args) == 3:
            constants[args[1]] = args[2]
        i += count
    info['bindings'] = [{'id': key, 'name': names.get(key, ''), **value}
                        for key, value in sorted(decorations.items()) if 'binding' in value]
    info['specialization_constants'] = [{'id': key, 'name': names.get(key, ''), **value,
                                        'default': constants.get(key)}
                                       for key, value in sorted(decorations.items()) if 'spec_id' in value]
    return info


def extract(data, fsr4_only=True):
    unique = {}
    occurrences = 0
    invalid = 0
    for section, start, end in sections(data):
        pos = start
        while True:
            pos = data.find(MAGIC, pos, end)
            if pos < 0:
                break
            at = pos
            pos += 4
            if at+10 > end:
                continue
            size = u32(data, at+4)
            if not 20 <= size <= MAX_SHADER or size % 4 or data[at+8] != 0x78:
                continue
            stream = zlib.decompressobj()
            try:
                code = stream.decompress(memoryview(data)[at+8:end], size+1)
                if len(code) != size or not stream.eof:
                    invalid += 1
                    continue
                info = reflect(code)
            except (zlib.error, ValueError, UnicodeError):
                invalid += 1
                continue
            if fsr4_only and not any(e['name'].startswith('fsr4_model_') for e in info['entry_points']):
                continue
            occurrences += 1
            digest = hashlib.sha256(code).hexdigest()
            if digest not in unique:
                unique[digest] = {'code': code, 'sha256': digest, **info, 'locations': []}
            unique[digest]['locations'].append({'file_offset': at, 'section': section})
    return list(unique.values()), occurrences, invalid


def write_pack(path, modules):
    offset = 16 + len(modules)*RECORD.size
    table, codes = [], []
    for module in modules:
        name = module['entry_points'][0]['name'].encode()
        if len(name) >= 128:
            raise ValueError('entry point name too long')
        table.append(RECORD.pack(offset, len(module['code']), 0, name))
        codes.append(module['code'])
        offset += len(module['code'])
    with path.open('wb') as out:
        out.write(PACK_MAGIC + struct.pack('<II', 1, len(modules)))
        for record in table:
            out.write(record)
        for code in codes:
            out.write(code)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dll', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--all-shaders', action='store_true', help='also include analytical FSR2/3 kernels')
    args = parser.parse_args()
    data = args.dll.read_bytes()
    modules, occurrences, invalid = extract(data, not args.all_shaders)
    if not modules:
        parser.error('no compatible compressed FSR4 Vulkan programs found; DX12/driver DLLs are not interchangeable')
    args.output.mkdir(parents=True, exist_ok=False)
    programs = args.output/'modules'
    programs.mkdir()
    for module in modules:
        (programs/(module['sha256']+'.spv')).write_bytes(module['code'])
    write_pack(args.output/'shaders.spvpack', modules)
    version = None
    at = data.find(struct.pack('<I', 0xFEEF04BD))
    if 0 <= at <= len(data)-16:
        version = [value for word in struct.unpack_from('<II', data, at+8) for value in (word >> 16, word & 65535)]
    report = {'format_version': 1, 'dll_sha256': hashlib.sha256(data).hexdigest(), 'dll_version': version,
              'unique_modules': len(modules), 'embedded_occurrences': occurrences, 'rejected_candidates': invalid,
              'runtime_ready': False, 'weights_recovered': False,
              'windows_imports': imports(data),
              'status': 'Shader extraction only; no dispatch graph, variant selection or upscaling runtime yet.',
              'modules': [{k:v for k,v in module.items() if k != 'code'} for module in modules]}
    (args.output/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k != 'modules'}, indent=2))


if __name__ == '__main__':
    main()
