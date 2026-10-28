#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Recover audited experimental native FSR 4.1.2 INT8 assets.

Uses the user's official DLL. No Wine, capture, DXIL compiler or downloaded weights
are needed. --all prepares the Kyty integration; image equivalence remains unverified.
"""
import argparse
import hashlib
import json
import os
import shutil
import tempfile
import fcntl
import sys
from pathlib import Path

from extract_vk import extract
from adapt_spirv import pad_rgb_writes

DLL_SHA256 = '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'
ASSET_VERSION = 'FSR412_VULKAN_INT8_4.1.2.4555'
SETS = ('t1080_m0', 't1080_m1', 't2160_m0', 't2160_m1')
PREFIX = 'fsr4_model_v07_fp8_no_scale_'
SEQUENCE = ['spd', 'prepass', 'pass0_post'] + [f'pass{k}{s}' for k in range(1, 13) for s in ('', '_post')] + ['postpass', 'rcas']


def recover(data, audit, output, set_name="t1080_m0", modules=None):
    if set_name not in SETS:
        raise ValueError("unsupported asset family")
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
    if modules is None:
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
    target = output / set_name
    target.mkdir()
    for name, code in assets.items():
        (target / name).write_bytes(code)
    report = {'dll_sha256': digest, 'model': 'INT8 ' + set_name, 'shader_count': 29,
              'initializer_sha256': weights['sha256'], 'initializer_bytes': size,
              'shader_adaptations': adaptations,
              'image_equivalence_validated': False,
              'scope': 'Experimental native replay assets for ' + set_name + '; game rendering remains unverified.',
              'files': {name: hashlib.sha256(code).hexdigest() for name, code in assets.items()}}
    (output / 'asset-provenance.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def recover_all(data, output, install=False):
    if hashlib.sha256(data).hexdigest() != DLL_SHA256:
        raise ValueError('Requires amd_fidelityfx_upscaler_vk.dll 4.1.2.4555 from SDK 2.4.0; DLL hash does not match')
    output = output.absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    with (output.parent / ('.' + output.name + '.lock')).open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('Another preparation is already running for this asset folder')
        if output.exists():
            if not install:
                raise FileExistsError('Output already exists; use a new directory')
            if output.is_symlink() or not output.is_dir() or (output/'ASSET_VERSION').read_text().strip() != ASSET_VERSION:
                raise ValueError('Existing folder is not a recognized native 4.1.2 asset installation; use a new folder')
        stage = Path(tempfile.mkdtemp(prefix='.'+output.name+'.stage-', dir=output.parent))
        backup = None
        try:
            print('[FSR412] Decompressing and validating Vulkan shader programs', file=sys.stderr, flush=True)
            modules, _, rejected = extract(data, fsr4_only=False)
            if rejected:
                raise ValueError('Compressed program validation failed')
            reports = {}
            recipes = Path(__file__).resolve().parent/'recipes'
            for name in SETS:
                print('[FSR412] Recovering and verifying ' + name, file=sys.stderr, flush=True)
                audit = json.loads((recipes/(name+'.json')).read_text())
                # The single-set converter writes only to a new owned staging directory.
                temporary = stage/('convert-'+name)
                reports[name] = recover(data, audit, temporary, name, modules)
                os.rename(temporary/name, stage/name)
                shutil.rmtree(temporary)
            manifest = {'asset_version': ASSET_VERSION, 'dll_sha256': DLL_SHA256,
                        'sets': reports, 'game_rendering_verified': False,
                        'status': 'Experimental native Vulkan INT8 assets prepared; rendering still requires game testing.'}
            (stage/'asset-provenance.json').write_text(json.dumps(manifest, indent=2)+'\n')
            (stage/'ASSET_VERSION').write_text(ASSET_VERSION+'\n')
            print('[FSR412] Installing complete native INT8 assets', file=sys.stderr, flush=True)
            if output.exists():
                backup = Path(tempfile.mkdtemp(prefix=output.name+'.previous-', dir=output.parent))
                backup.rmdir()
                os.rename(output, backup)
            try:
                os.rename(stage, output)
            except BaseException:
                if backup is not None and not output.exists():
                    os.rename(backup, output)
                raise
            if backup is not None:
                print('Previous generated assets preserved at: '+str(backup), file=sys.stderr, flush=True)
            print('[FSR412] Ready: four INT8 model/tier sets; restart the game to use FSR 4.1.2', file=sys.stderr, flush=True)
            return manifest
        finally:
            if stage.exists():
                shutil.rmtree(stage)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('dll', type=Path)
    p.add_argument('new_output', type=Path)
    p.add_argument('--all', action='store_true', help='prepare both quality families and output tiers')
    p.add_argument('--install', action='store_true', help='replace an owned installation, preserving its previous contents')
    a = p.parse_args()
    try:
        print('[FSR412] Checking official Vulkan DLL version and SHA-256', file=sys.stderr, flush=True)
        data = a.dll.read_bytes()
        if a.all:
            report = recover_all(data, a.new_output, a.install)
        else:
            if a.install:
                raise ValueError('--install requires --all')
            audit = json.loads((Path(__file__).resolve().parent/'host-int8-t1080-m0.json').read_text())
            report = recover(data, audit, a.new_output)
    except (ValueError, OSError, KeyError) as error:
        p.exit(2, f'[FSR412] Preparation failed: {error}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
