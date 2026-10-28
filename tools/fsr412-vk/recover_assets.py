#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Recover the audited INT8 t1080/m0 experimental native frame-test assets.

Uses the user's official DLL. No Wine, capture, DXIL compiler or downloaded weights
are needed. This is not an installation into Kyty or proof of image equivalence.
"""
import argparse
import hashlib
import json
from pathlib import Path

from extract_vk import extract
from adapt_spirv import pad_rgb_writes

DLL_SHA256 = '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'
PREFIX = 'fsr4_model_v07_fp8_no_scale_'
SEQUENCE = ['spd', 'prepass', 'pass0_post'] + [f'pass{k}{s}' for k in range(1, 13) for s in ('', '_post')] + ['postpass', 'rcas']


def recover(data, audit, output):
    digest = hashlib.sha256(data).hexdigest()
    if digest != DLL_SHA256 or audit['dll_sha256'] != digest:
        raise ValueError('DLL does not match the audited official SDK 2.4 Vulkan 4.1.2.4555 build')
    if audit['synthetic_profile'] != 'int8' or audit['gpu_executed'] or audit['runtime_ready']:
        raise ValueError('unexpected host audit provenance')
    frames = list(audit['frames'].values())
    if len(frames) < 3 or any(len(x) != 29 for x in frames):
        raise ValueError('expected three recorded 29-dispatch frames')
    chosen = frames[0]
    names = ['spd' if i == 0 else 'rcas' if i == 28 else x['entry'].removeprefix(PREFIX)
             for i, x in enumerate(chosen)]
    if names != SEQUENCE:
        raise ValueError('dispatch order differs from the audited native replay')
    for frame in frames[1:]:
        if [(x['entry'], x['sha256'], x['groups']) for x in frame] != [(x['entry'], x['sha256'], x['groups']) for x in chosen]:
            raise ValueError('shader selection or group counts changed between recorded frames')
    modules, _, rejected = extract(data, fsr4_only=False)
    if rejected:
        raise ValueError('compressed program validation failed')
    by_hash = {x['sha256']: x for x in modules}
    assets, adaptations = {}, []
    for name, info in zip(names, chosen):
        module = by_hash.get(info['sha256'])
        if not module or info['entry'] not in [x['name'] for x in module['entry_points']]:
            raise ValueError(f'missing audited shader: {name}')
        if 6022 in module['capabilities']:
            raise ValueError('unexpected matrix shader in INT8 recipe')
        adapted, padded = pad_rgb_writes(module['code'])
        assets[name + '.spv'] = adapted
        if padded:
            adaptations.append({'shader': name, 'original_sha256': info['sha256'],
                                'padded_rgb_writes': padded, 'alpha': 1.0})
    if len(audit['initializers']) != 1:
        raise ValueError('expected one initializer for this model')
    weights = audit['initializers'][0]
    at, size = weights['dll_file_offset'], weights['size']
    if not weights['byte_match_in_dll'] or not isinstance(at, int) or at < 0 or size != 131072 or at > len(data)-size:
        raise ValueError('invalid initializer location')
    initializer = data[at:at+size]
    if hashlib.sha256(initializer).hexdigest() != weights['sha256']:
        raise ValueError('initializer digest mismatch')
    assets['initializer.bin'] = initializer
    output.mkdir(parents=True, exist_ok=False)
    target = output / 't1080_m0'
    target.mkdir()
    for name, code in assets.items():
        (target / name).write_bytes(code)
    report = {'dll_sha256': digest, 'model': 'INT8 t1080_m0', 'shader_count': 29,
              'initializer_sha256': weights['sha256'], 'initializer_bytes': size,
              'shader_adaptations': adaptations,
              'game_integration_ready': False, 'image_equivalence_validated': False,
              'scope': 'Experimental standalone native frame test only; other models/tiers are not included.',
              'files': {name: hashlib.sha256(code).hexdigest() for name, code in assets.items()}}
    (output / 'asset-provenance.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('dll', type=Path)
    p.add_argument('new_output', type=Path)
    a = p.parse_args()
    audit = json.loads((Path(__file__).resolve().parent/'host-int8-t1080-m0.json').read_text())
    try:
        report = recover(a.dll.read_bytes(), audit, a.new_output)
    except (ValueError, OSError) as error:
        p.exit(1, f'Recovery failed: {error}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
