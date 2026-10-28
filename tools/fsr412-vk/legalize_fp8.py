#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Complete SSA legalization of dead Function-local opaque image copies.

SPIRV-Tools SSA rewrite replaces reads with reaching definitions but leaves
stores. Remove an opaque local only when *every* remaining reference is its
own declaration, name, decoration, or a store into that variable. Arithmetic,
image operations and live pointers remain unchanged. Strict spirv-val follows.
"""
import struct


def remove_dead_opaque_locals(code):
    if len(code)<20 or len(code)%4:raise ValueError('malformed SPIR-V byte count')
    words=list(struct.unpack('<'+'I'*(len(code)//4),code));instructions=[];at=5
    if words[0]!=0x07230203:raise ValueError('not SPIR-V')
    opaque=set();pointers={};variables=set()
    while at<len(words):
        count,op=words[at]>>16,words[at]&65535
        if not count or count>len(words)-at:raise ValueError('malformed SPIR-V instruction')
        w=words[at:at+count];instructions.append(w)
        if op in (25,26,27):opaque.add(w[1])
        if op==32 and count==4 and w[2]==7:pointers[w[1]]=w[3]
        if op==59 and count==4 and w[3]==7 and pointers.get(w[1]) in opaque:variables.add(w[2])
        at+=count
    unsafe=set()
    for w in instructions:
        op=w[0]&65535
        if op==59 and w[2] in variables:continue
        if op in (5,71,332) and w[1] in variables:continue
        if op==62 and w[1] in variables:
            unsafe.update(variables.intersection(w[2:]));continue
        unsafe.update(variables.intersection(w[1:]))
    dead=variables-unsafe
    output=words[:5]
    for w in instructions:
        op=w[0]&65535
        if op==59 and w[2] in dead:continue
        if op in (5,71,332,62) and w[1] in dead:continue
        output.extend(w)
    return struct.pack('<'+'I'*len(output),*output),sorted(dead)


def bridge_reverse_matrix_conversions(code):
    """Replace unsupported A/B -> accumulator casts with shared-memory copies.

    NV matrix conversions support accumulator -> A/B. The SDK also emits the
    reverse, which the NV extension does not permit. For the audited 16x16,
    one-subgroup kernels, store FP16 A/B to shared memory, convert its scalars
    exactly to FP32, then load a FP32 accumulator. FP8 -> FP16 -> FP32 is exact.
    No matrix multiplication or pretrained weight is changed.
    """
    w=list(struct.unpack('<'+'I'*(len(code)//4),code));instructions=[];at=5
    constants={};floats={};encodings={};integers={};matrices={};pointer_types={};value_types={};builtin=None;local=None
    result_ops={41,42,43,44,45,46,48,49,50,51,52,54,55,57,59,61,65,66,67,68,70,79,80,81,82,83,113,114,115,124,169,245,4457,4459,5293,5369,5390}
    while at<len(w):
        n,op=w[at]>>16,w[at]&65535
        if not n or n>len(w)-at:raise ValueError('malformed SPIR-V instruction')
        a=w[at:at+n];instructions.append(a)
        if op==43 and n==4:constants[a[2]]=(a[1],a[3])
        if op==22:
            floats[a[1]]=a[2]
            if len(a)>3:encodings[a[1]]=a[3]
        if op==21:integers[a[1]]=tuple(a[2:])
        if op==4456:matrices[a[1]]=tuple(a[2:])
        if op==32:pointer_types[a[1]]=tuple(a[2:])
        if op==16 and n==6 and a[2]==17:local=tuple(a[3:])
        if op==71 and n==4 and a[2]==11 and a[3]==29:builtin=a[1]
        if op in result_ops and n>=3:value_types[a[2]]=a[1]
        at+=n
    def number(id):return constants.get(id,(None,None))[1]
    casts=[]
    for a in instructions:
        if (a[0]&65535)!=115:continue
        dst=matrices.get(a[1]);src=matrices.get(value_types.get(a[3]))
        if dst and src and number(src[4]) in (0,1) and number(dst[4])==2:
            if floats.get(src[0])==8 and encodings.get(src[0])!=4214:raise ValueError('Only audited FP8 E4M3 reverse casts are supported')
            if local!=(32,1,1) or number(src[1])!=3 or src[1:4]!=dst[1:4] or number(src[2])!=16 or number(src[3])!=16 or floats.get(src[0]) not in (8,16) or floats.get(dst[0])!=32:
                raise ValueError('reverse matrix cast is outside the audited 16x16 FP8/FP16 -> FP32 subgroup case')
            casts.append((a[2],src,dst))
    if not casts:return code,0
    next_id=w[3];declarations=[];annotations=[];new_globals=[];capabilities=[]
    def fresh():
        nonlocal next_id
        id=next_id;next_id+=1;return id
    def ins(op,*args):return [((len(args)+1)<<16)|op,*args]
    def declare(op,*args):
        id=fresh();declarations.append(ins(op,id,*args));return id
    uint=next((id for id,t in integers.items() if t==(32,0)),None)
    if uint is None:raise ValueError('audited shader has no uint32 type')
    half=next((id for id,bits in floats.items() if bits==16),None)
    if half is None:half=declare(22,16)
    if not any((a[0]&65535)==17 and a[1]==9 for a in instructions):capabilities.append(ins(17,9))
    float32=next(id for id,bits in floats.items() if bits==32)
    def const(value):
        found=next((id for id,pair in constants.items() if pair==(uint,value)),None)
        if found is None:
            found=fresh();declarations.append(ins(43,uint,found,value));constants[found]=(uint,value)
        return found
    zero,c16,c256,scope,semantics=const(0),const(16),const(256),const(2),const(0x108)
    arr_half=declare(28,half,c256);arr_float=declare(28,float32,c256)
    ptr_arr_half=declare(32,4,arr_half);ptr_arr_float=declare(32,4,arr_float)
    ptr_half=declare(32,4,half);ptr_float=declare(32,4,float32)
    buf_half=fresh();declarations.append(ins(59,ptr_arr_half,buf_half,4));new_globals.append(buf_half)
    buf_float=fresh();declarations.append(ins(59,ptr_arr_float,buf_float,4));new_globals.append(buf_float)
    if builtin is None:
        ptr_input=next((id for id,t in pointer_types.items() if t==(1,uint)),None)
        if ptr_input is None:ptr_input=declare(32,1,uint)
        builtin=fresh();declarations.append(ins(59,ptr_input,builtin,1));new_globals.append(builtin)
        annotations.append(ins(71,builtin,11,29))
    cast_types={}
    for result,src,dst in casts:
        target=(half,*src[1:])
        id=next((id for id,t in matrices.items() if t==target),None)
        if id is None:id=declare(4456,*target);matrices[id]=target
        cast_types[result]=(id,src[0])
    # Ensure every newly used constant is declared before the first function.
    offsets=[const(32*k) for k in range(8)]
    output=w[:5];output[3]=next_id
    inserted_annotations=False;inserted_declarations=False;inserted_caps=False
    for a in instructions:
        op=a[0]&65535
        if not inserted_caps and op!=17:
            for x in capabilities:output.extend(x)
            inserted_caps=True
        if not inserted_annotations and (19<=op<=39 or op==5341):
            for x in annotations:output.extend(x)
            inserted_annotations=True
        if not inserted_declarations and op==54:
            for x in declarations:output.extend(x)
            inserted_declarations=True
        if op==15:
            a=a+[id for id in new_globals if id not in a[3:]];a[0]=(len(a)<<16)|op
        if op!=115 or a[2] not in cast_types:
            output.extend(a);continue
        half_matrix,source_component=cast_types[a[2]]
        def emit(op,*args):output.extend(ins(op,*args))
        def value(op,type,*args):
            id=fresh();emit(op,type,id,*args);return id
        matrix_value=a[3]
        if floats.get(source_component)==8:matrix_value=value(115,half_matrix,matrix_value)
        ph0=value(65,ptr_half,buf_half,zero)
        emit(4458,ph0,matrix_value,zero,c16)
        emit(224,scope,scope,semantics)
        lane=value(61,uint,builtin)
        for offset in offsets:
            index=value(128,uint,lane,offset)
            ph=value(65,ptr_half,buf_half,index);h=value(61,half,ph)
            f=value(115,float32,h);pf=value(65,ptr_float,buf_float,index);emit(62,pf,f)
        emit(224,scope,scope,semantics)
        pf0=value(65,ptr_float,buf_float,zero)
        emit(4457,a[1],a[2],pf0,zero,c16)
        emit(224,scope,scope,semantics)
    output[3]=next_id
    return struct.pack('<'+'I'*len(output),*output),len(casts)
