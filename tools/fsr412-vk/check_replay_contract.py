#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check proposed native INT8 replay constants/groups against observed CPU calls.

This checks a synthetic HDR/auto-exposure/sharpening capture (capture_main.c).
It does not compare GPU images or prove the complete resource/barrier contract.
"""
import argparse
import json
import math
from pathlib import Path
import re
import struct
from recover_assets import PREFIX, SEQUENCE


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def groups(name, rw, rh, ow, oh):
    ceil = lambda x, y: (x+y-1)//y
    aw, ah = (ow+7)&~7, (oh+7)&~7
    if name == 'spd':return [ceil(rw,64),ceil(rh,64),1]
    if name == 'rcas':return [ceil(aw,16),ceil(ah,16),1]
    if name == 'prepass':return [ceil(aw,16),ceil(ah,16),1]
    if name == 'postpass':return [ceil(aw,32),ceil(ah,32),1]
    k = int(re.fullmatch(r'pass(\d+)(?:_post)?', name)[1])
    if name.endswith('_post'):
        level = [1,1,1,2,2,2,3,3,3,2,2,1,1][k]
        w,h = aw>>level,ah>>level
        kx = min((((1920>>level)+3)//4)*4+1-w,5)
        ky = min((1080>>level)+1-h,1)
        return [ceil((w+1+kx)+h+h*kx+(w+1+kx)*ky,32),1,1]
    level = [1,1,1,2,2,2,3,3,3,3,2,2,1][k]
    return [ceil(aw>>level,64),ah>>level,1]


def check(audit,rw,rh,ow,oh):
    if audit['synthetic_profile'] != 'int8' or ow>1920 or oh>1080:
        raise ValueError('only the normal INT8 1080p model is supported by this checker')
    checked = 0
    for key, frame in audit['frames'].items():
        number = int(key)
        if len(frame)!=29:raise ValueError('wrong pass count')
        for i, p in enumerate(frame):
            name = 'spd' if i==0 else 'rcas' if i==28 else p['entry'].removeprefix(PREFIX)
            if name != SEQUENCE[i] or p['groups'] != groups(name,rw,rh,ow,oh):
                raise ValueError(f'frame {number} {name}: dispatch rule mismatch')
            constants = p['constants']
            if name in ('prepass','postpass'):
                scale=[f32(ow/rw),f32(oh/rh)]
                floats=[1/ow,1/oh,*scale,1/scale[0],1/scale[1],0.25*(number%4)-0.375,0.125,
                        1/rw,1/rh,ow,oh,ow,oh,0,0]
                want=[bits(x) for x in floats]+[ow,oh,int(number==0),rw,rh,bits(1),bits(0 if number==0 else 1),1,bits(0.5),0]
                if constants.get('MLSR_Optimized_Constants') != want:
                    raise ValueError(f'frame {number} {name}: MLSR constants differ from native replay')
            elif name.startswith('pass'):
                shifts=[1,0,1,1,2,2,2,3,3,3,2,2,1,1,0,0,0]
                want=[v for shift in shifts for v in [((ow+7)&~7)>>shift,((oh+7)&~7)>>shift,0,0]]
                if constants.get('CsTensorSizes') != want:
                    raise ValueError(f'frame {number} {name}: tensor size constants differ')
            elif name=='spd':
                want=[min(12,int(math.log2(max(rw,rh)))),((rw+63)//64)*((rh+63)//64),0,0,bits(1/rw),bits(1/rh),bits(1),0]
                if constants.get('AutoExposureSPDConstants') != want:
                    raise ValueError(f'frame {number}: SPD constants differ')
            else:
                # sharpness=0.5 -> exp2(-1)=0.5; FP16 0.5=0x3800.
                want=[bits(0.5),0x38003800,0,0,bits(1),0,0,0]
                if constants.get('cbRCAS') != want:
                    raise ValueError(f'frame {number}: RCAS constants differ')
            checked += 1
    return {'frames': len(audit['frames']), 'checked_dispatches': checked,
            'constants_and_groups_match_native_replay': True,
            'image_equivalence_validated': False, 'resource_barrier_contract_validated': False}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('audit',type=Path)
    p.add_argument('render',help='WIDTHxHEIGHT')
    p.add_argument('output',help='WIDTHxHEIGHT')
    a=p.parse_args()
    rw,rh=map(int,a.render.split('x'));ow,oh=map(int,a.output.split('x'))
    print(json.dumps(check(json.loads(a.audit.read_text()),rw,rh,ow,oh),indent=2))
