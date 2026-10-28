#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit a synthetic host trace. Emit metadata only, never shader/weight bytes.

This observes what the Windows CPU provider requests on a simulated device.
It does not establish GPU compatibility, pixel correctness or runtime readiness.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct

from extract_vk import reflect


def local_file(root, name):
    if not isinstance(name, str) or Path(name).name != name or name in ('', '.', '..'):
        raise ValueError('trace asset must be a filename inside the capture directory')
    path = root / name
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'missing regular capture asset: {name}')
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('capture asset exceeds audit limit')
    return path.read_bytes()


def analyze(root, dll):
    pipelines, views, resources = {}, {}, {}
    frames = collections.defaultdict(list)
    initializers = {}
    profile = None
    for line in (root / 'trace.jsonl').read_text().splitlines():
        if len(line) > 1024 * 1024:
            raise ValueError('trace event exceeds audit limit')
        e = json.loads(line)
        op = e['op']
        if op == 'profile':
            if profile is not None or not e['synthetic'] or e['gpu_executed']:
                raise ValueError('expected exactly one offline profile')
            profile = e
        elif op in ('buffer', 'image'):
            resources[e['id']] = e
        elif op == 'buffer_view':
            views[e['id']] = e
        elif op == 'pipeline':
            code = local_file(root, e['shader'])
            info = reflect(code)
            if e['entry'] not in [x['name'] for x in info['entry_points']]:
                raise ValueError('pipeline entry does not match shader')
            pipelines[e['id']] = {**e, 'sha256': hashlib.sha256(code).hexdigest(), 'reflection': info}
        elif op == 'dispatch':
            if e['pipeline'] not in pipelines or len(e['groups']) != 3 or any(not isinstance(x, int) or x < 1 for x in e['groups']):
                raise ValueError('invalid dispatch or missing pipeline')
            pipeline = pipelines[e['pipeline']]
            recorded = {'entry': pipeline['entry'], 'sha256': pipeline['sha256'], 'groups': e['groups'], 'constants': {}}
            frames[e['frame']].append(recorded)
            bindings = {(b['set'], b['binding']): b for b in e['bindings']}
            if len(bindings) != len(e['bindings']):
                raise ValueError('duplicate descriptor binding')
            for binding in pipeline['reflection']['bindings']:
                observed = bindings.get((binding['set'], binding['binding']))
                if binding['name'] in ('CsTensorSizes', 'MLSR_Optimized_Constants', 'AutoExposureSPDConstants', 'cbRCAS') and (not observed or 'data' not in observed):
                    raise ValueError(f"constant buffer was not captured: {binding['name']}")
                # Uniforms describe host decisions, not model weights. Preserve exact bits
                # for checking a proposed native replay against the provider's CPU code.
                if observed and observed['type'] == 6 and 'data' in observed:
                    data = local_file(root, observed['data'])
                    if len(data) > 4096 or len(data) % 4:
                        raise ValueError('unexpected constant buffer size')
                    recorded['constants'][binding['name']] = list(struct.unpack('<'+'I'*(len(data)//4), data))
                if binding['name'] != 'InitializerBuffer':
                    continue
                if not observed or 'data' not in observed:
                    raise ValueError('model initializer was not captured')
                data = local_file(root, observed['data'])
                if not data or not any(data):
                    raise ValueError('empty or zero model initializer')
                digest = hashlib.sha256(data).hexdigest()
                buffer = observed.get('buffer')
                if buffer is None:
                    buffer = views[observed['view']]['buffer']
                if buffer not in resources:
                    raise ValueError('initializer refers to unknown resource')
                at = dll.find(data)
                initializers[digest] = {'sha256': digest, 'size': len(data), 'buffer': buffer,
                                        'dll_file_offset': at if at >= 0 else None,
                                        'byte_match_in_dll': at >= 0}
    if profile is None or not frames:
        raise ValueError('no complete offline profile and dispatches')
    for number, dispatches in frames.items():
        if not any(x['entry'].startswith('fsr4_model_') for x in dispatches):
            raise ValueError(f'frame {number} contains no FSR4 model dispatch; refusing fallback')
    if not initializers:
        raise ValueError('no initializer binding observed')
    return {'format_version': 1, 'dll_sha256': hashlib.sha256(dll).hexdigest(),
            'synthetic_profile': 'fp8' if profile['fp8'] else 'int8',
            'gpu_executed': False, 'runtime_ready': False, 'image_validated': False,
            'initializer_upload_observed': True, 'initializers': list(initializers.values()),
            'frames': dict(sorted(frames.items())),
            'status': 'Host decisions and initializer bytes observed on a synthetic device; no GPU execution.'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('dll', type=Path)
    p.add_argument('report', type=Path)
    a = p.parse_args()
    result = analyze(a.capture, a.dll.read_bytes())
    a.report.write_text(json.dumps(result, indent=2) + '\n')
    summary = {k: v for k, v in result.items() if k != 'frames'}
    summary['frame_dispatch_counts'] = {k: len(v) for k, v in result['frames'].items()}
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
