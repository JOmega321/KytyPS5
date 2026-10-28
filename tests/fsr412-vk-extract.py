import ctypes
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('extract_vk', ROOT/'tools/fsr412-vk/extract_vk.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def shader():
    name = b'fsr4_model_test\0'
    name += b'\0'*(-len(name)%4)
    names = struct.unpack('<'+'I'*(len(name)//4), name)
    words = [0x07230203, 0x10500, 0, 5, 0, 2<<16|17, 1,
             3<<16|14, 0, 1, (3+len(names))<<16|15, 5, 1, *names,
             6<<16|16, 1, 17, 1, 1, 1, 2<<16|19, 2,
             3<<16|33, 3, 2, 5<<16|54, 2, 1, 0, 3,
             2<<16|248, 4, 1<<16|253, 1<<16|56]
    return struct.pack('<'+'I'*len(words), *words)


def dll(payload):
    data = bytearray(512)
    data[:2] = b'MZ'
    struct.pack_into('<I',data,60,128)
    data[128:132]=b'PE\0\0'
    struct.pack_into('<HH',data,132,0x8664,1)
    struct.pack_into('<HH',data,148,240,0x2022)
    struct.pack_into('<H',data,152,0x20b)
    data[392:400]=b'.rdata\0\0'
    struct.pack_into('<II',data,408,len(payload),512)
    struct.pack_into('<I',data,428,0x40000040)
    return bytes(data)+payload


class ExtractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="fsr412 test ' ")
        cls.root = Path(cls.temp.name)
        subprocess.run(['bash',str(ROOT/'tools/fsr412-vk/build-native.sh'),str(cls.root)],check=True)
        cls.lib = ctypes.CDLL(str(cls.root/'libfsr412_spirv.so'))
        cls.lib.fsr412_spirv_open.argtypes=[ctypes.c_char_p]
        cls.lib.fsr412_spirv_open.restype=ctypes.c_void_p
        cls.lib.fsr412_spirv_count.argtypes=[ctypes.c_void_p]
        cls.lib.fsr412_spirv_count.restype=ctypes.c_size_t
        cls.lib.fsr412_spirv_close.argtypes=[ctypes.c_void_p]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_extract_and_native_load(self):
        raw = shader()
        blob = mod.MAGIC+struct.pack('<I',len(raw))+zlib.compress(raw)
        modules, count, bad = mod.extract(dll(blob+blob))
        self.assertEqual((len(modules),count,bad),(1,2,0))
        self.assertEqual(modules[0]['code'],raw)
        self.assertEqual(modules[0]['entry_points'][0]['name'],'fsr4_model_test')
        path = self.root/'test.spvpack'
        mod.write_pack(path,modules)
        pack = self.lib.fsr412_spirv_open(str(path).encode())
        self.assertTrue(pack)
        self.assertEqual(self.lib.fsr412_spirv_count(pack),1)
        self.lib.fsr412_spirv_close(pack)
        subprocess.run([str(self.root/'native-probe'),str(self.root/'libfsr412_spirv.so'),str(path)],check=True)

    def test_reject_truncated_or_non_pe(self):
        for data in [b'',b'MZ',b'ELF',dll(b'')[:-1]]:
            with self.assertRaises(ValueError):mod.extract(data)

    def test_bound_inflate_and_invalid_stream(self):
        raw = shader()
        for declared in [len(raw)-4, len(raw)+4, mod.MAX_SHADER+4]:
            modules, _, _ = mod.extract(dll(mod.MAGIC+struct.pack('<I',declared)+zlib.compress(raw)))
            self.assertFalse(modules)
        self.assertFalse(mod.extract(dll(mod.MAGIC+struct.pack('<I',len(raw))+b'\x78bad'))[0])

    def test_native_rejects_corrupt_record(self):
        modules,_,_=mod.extract(dll(mod.MAGIC+struct.pack('<I',len(shader()))+zlib.compress(shader())))
        good = self.root/'good.spvpack'
        mod.write_pack(good,modules)
        data=bytearray(good.read_bytes())
        for offset in [0, 16, 24, len(data)-4]:
            bad=bytearray(data);bad[offset:offset+4]=b'\xff'*4
            path=self.root/'bad.spvpack';path.write_bytes(bad)
            self.assertFalse(self.lib.fsr412_spirv_open(str(path).encode()))


if __name__=='__main__': unittest.main()
