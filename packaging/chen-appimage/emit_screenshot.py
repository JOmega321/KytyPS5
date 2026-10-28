#!/usr/bin/env python3
"""Emit small PNG screenshots in CI logs for visual review without workspace access."""
import base64
from pathlib import Path
import struct
import sys
import zlib

def png_from_bmp(path):
    data=path.read_bytes()
    assert data[:2]==b'BM'
    offset=struct.unpack_from('<I',data,10)[0]
    width,height=struct.unpack_from('<ii',data,18)
    bits=struct.unpack_from('<H',data,28)[0]
    compression=struct.unpack_from('<I',data,30)[0]
    assert 0<width<=4096 and 0<abs(height)<=4096 and bits in (24,32) and compression in (0,3)
    stride=((width*bits+31)//32)*4
    assert offset+stride*abs(height)<=len(data)
    rows=[]
    for y in range(abs(height)):
        row=offset+(abs(height)-1-y if height>0 else y)*stride
        pixels=bytearray()
        for x in range(width):
            at=row+x*(bits//8)
            pixels.extend((data[at+2],data[at+1],data[at]))
        rows.append(b'\0'+pixels)
    def chunk(kind,payload):
        return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,abs(height),8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows),9))+chunk(b'IEND',b'')

for name in sys.argv[1:]:
    path=Path(name)
    encoded=base64.b64encode(png_from_bmp(path)).decode()
    print('QA_IMAGE_BEGIN '+name)
    for at in range(0,len(encoded),8000):print('QA_IMAGE_DATA '+encoded[at:at+8000])
    print('QA_IMAGE_END '+name)
