#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Remove descriptor globals absent from every function's operand stream.

Conservative static use: even a numeric literal equal to a resource ID keeps
that resource. No function body or arithmetic instruction is changed.
"""
import struct


def prune_unused_bindings(code):
    if len(code)<20 or len(code)%4:
        raise ValueError('malformed SPIR-V byte count')
    words=list(struct.unpack('<'+'I'*(len(code)//4),code))
    if words[0]!=0x07230203:
        raise ValueError('not SPIR-V')
    instructions=[];at=5;bound=set();variables=set();used=set();in_function=False
    while at<len(words):
        count,op=words[at]>>16,words[at]&65535
        if not count or count>len(words)-at:raise ValueError('malformed SPIR-V instruction')
        w=words[at:at+count];instructions.append(w)
        if op==71 and count>=4 and w[2]==33:bound.add(w[1])
        if op==59 and count>=4 and w[3] in (0,2,12):variables.add(w[2])
        if op==54:in_function=True
        if in_function:used.update(w[1:])
        if op==56:in_function=False
        at+=count
    dead=(bound&variables)-used
    if not dead:return code,[]
    output=words[:5]
    for w in instructions:
        op=w[0]&65535
        if op==59 and w[2] in dead:continue
        if op in (5,71,332) and w[1] in dead:continue
        if op==15:
            # Skip the null-terminated entry-point name, then filter its interface.
            i=3
            while i<len(w):
                end=0 in struct.pack('<I',w[i]);i+=1
                if end:break
            w=w[:i]+[x for x in w[i:] if x not in dead]
            w[0]=(len(w)<<16)|op
        output.extend(w)
    return struct.pack('<'+'I'*len(output),*output),sorted(dead)
