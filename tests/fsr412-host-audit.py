import copy
import importlib.util
import json
from pathlib import Path
import struct
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/fsr412-vk'))
from analyze_capture import analyze, local_file
from adapt_spirv import pad_rgb_writes
from prune_bindings import prune_unused_bindings
from legalize_fp8 import remove_dead_opaque_locals, bridge_reverse_matrix_conversions


def shader():
    # Structural SPIR-V fixture for the reflection audit, not a GPU program.
    def string(s):
        b=s.encode()+b'\0';b+=b'\0'*(-len(b)%4)
        return list(struct.unpack('<'+'I'*(len(b)//4),b))
    def ins(op, *a):return [(len(a)+1)<<16|op,*a]
    words=[0x07230203,0x10500,0,20,0]
    words+=ins(15,5,1,*string('fsr4_model_test'))
    words+=ins(5,2,*string('InitializerBuffer'))+ins(71,2,33,21)+ins(71,2,34,0)
    words+=ins(5,3,*string('CsTensorSizes'))+ins(71,3,33,257)+ins(71,3,34,0)
    return struct.pack('<'+'I'*len(words),*words)


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.root=Path(self.temp.name)
        (self.root/'test.spv').write_bytes(shader())
        self.weights=bytes(range(256))*4
        (self.root/'weights.bin').write_bytes(self.weights)
        (self.root/'constants.bin').write_bytes(struct.pack('<4I',1920,1080,0,0))
        self.events=[{'op':'profile','synthetic':True,'gpu_executed':False,'fp8':False},
                     {'op':'buffer','id':10,'size':1024},
                     {'op':'pipeline','id':20,'entry':'fsr4_model_test','shader':'test.spv'},
                     {'op':'dispatch','frame':0,'pipeline':20,'groups':[1,1,1],
                      'bindings':[{'set':0,'binding':21,'type':7,'buffer':10,'data':'weights.bin'},
                                  {'set':0,'binding':257,'type':6,'buffer':11,'data':'constants.bin'}]}]

    def tearDown(self):self.temp.cleanup()

    def run_audit(self, events=None):
        (self.root/'trace.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in (events if events is not None else self.events)))
        return analyze(self.root,b'dll-prefix'+self.weights+b'dll-suffix')

    def test_exact_initializer_location_and_constant_bits(self):
        r=self.run_audit()
        self.assertEqual(r['initializers'][0]['dll_file_offset'],10)
        self.assertEqual(r['initializers'][0]['size'],1024)
        self.assertEqual(r['frames'][0][0]['constants']['CsTensorSizes'],[1920,1080,0,0])
        self.assertFalse(r['gpu_executed'])
        self.assertFalse(r['runtime_ready'])
        self.assertFalse(r['image_validated'])
        self.assertTrue(r['initializer_upload_observed'])
        self.assertNotIn(self.weights.hex(),json.dumps(r))

    def test_reject_missing_or_zero_initializer(self):
        del self.events[-1]['bindings'][0]['data']
        with self.assertRaisesRegex(ValueError,'not captured'):self.run_audit()
        self.events[-1]['bindings'][0]['data']='weights.bin'
        (self.root/'weights.bin').write_bytes(b'\0'*1024)
        with self.assertRaisesRegex(ValueError,'zero'):self.run_audit()

    def test_reject_unmatched_pipeline_or_descriptors(self):
        events=copy.deepcopy(self.events);events[-1]['pipeline']=100
        with self.assertRaisesRegex(ValueError,'missing pipeline'):self.run_audit(events)
        events=copy.deepcopy(self.events);events[-1]['bindings'].append(events[-1]['bindings'][0])
        with self.assertRaisesRegex(ValueError,'duplicate'):self.run_audit(events)

    def test_reject_claimed_gpu_execution_and_empty_trace(self):
        events=copy.deepcopy(self.events);events[0]['gpu_executed']=True
        with self.assertRaisesRegex(ValueError,'offline'):self.run_audit(events)
        with self.assertRaisesRegex(ValueError,'no complete'):self.run_audit([])

    def test_reject_escaping_asset_and_symlink(self):
        for name in ['../weights.bin','/tmp/test.bin','.','']:
            with self.assertRaises(ValueError):local_file(self.root,name)
        (self.root/'link.bin').symlink_to(self.root/'weights.bin')
        with self.assertRaises(ValueError):local_file(self.root,'link.bin')


def image_shader(rgb=True):
    def ins(op,*a):return [(len(a)+1)<<16|op,*a]
    def literal(s):
        b=s.encode()+b'\0';b+=b'\0'*(-len(b)%4)
        return list(struct.unpack('<'+'I'*(len(b)//4),b))
    words=[0x07230203,0x10500,0,20,0]
    words+=ins(17,1)+ins(17,56)+ins(11,17,*literal('GLSL.std.450'))+ins(14,0,1)
    words+=ins(15,5,14,*literal('main'),9)+ins(16,14,17,1,1,1)
    words+=ins(71,9,34,0)+ins(71,9,33,0)
    words+=ins(19,1)+ins(22,2,32)+ins(23,3,2,3 if rgb else 4)
    words+=ins(21,4,32,1)+ins(23,5,4,2)+ins(25,6,2,1,0,0,0,2,0)
    words+=ins(32,7,0,6)+ins(33,8,1)+ins(59,7,9,0)
    words+=ins(43,2,10,0)+ins(44,3,11,*([10]*(3 if rgb else 4)))
    words+=ins(43,4,12,0)+ins(44,5,13,12,12)
    words+=ins(54,1,14,0,8)+ins(248,15)+ins(61,6,16,9)
    words+=ins(12,3,18,17,43,11,11,11)  # extended-instruction result, previously missed
    words+=ins(99,16,13,11)+ins(99,16,13,18)+ins(253)+ins(56)
    return struct.pack('<'+'I'*len(words),*words)


class ShaderAdaptationTests(unittest.TestCase):
    def test_pad_both_constant_and_extended_instruction_rgb_writes(self):
        raw=image_shader()
        adapted,count=pad_rgb_writes(raw)
        self.assertEqual(count,2)
        self.assertGreater(len(adapted),len(raw))
        self.assertEqual(pad_rgb_writes(adapted),(adapted,0))
        if shutil.which('spirv-val'):
            with tempfile.TemporaryDirectory() as folder:
                p=Path(folder)/'test.spv';p.write_bytes(adapted)
                subprocess.run(['spirv-val','--target-env','vulkan1.3',str(p)],check=True)

    def test_leave_rgba_shader_byte_identical(self):
        raw=image_shader(False)
        self.assertEqual(pad_rgb_writes(raw),(raw,0))

    def test_reject_malformed_instruction_stream(self):
        with self.assertRaises(ValueError):pad_rgb_writes(image_shader()[:-1])


class Fp8LegalizationTests(unittest.TestCase):
    def fixture(self, opaque=False, live=False):
        raw=image_shader(False)
        w=list(struct.unpack('<'+'I'*(len(raw)//4),raw));parts=[];at=5
        while at<len(w):
            n=w[at]>>16;parts.append(w[at:at+n]);at+=n
        def ins(op,*a):return [((len(a)+1)<<16)|op,*a]
        out=w[:5];out[3]=22 if live else 21 if opaque else 20
        for a in parts:
            op=a[0]&65535
            if not opaque and op==15:a=a+[19];a[0]=(len(a)<<16)|15
            if not opaque and op==19:out+=ins(71,19,34,0)+ins(71,19,33,99)
            if op==54:out+=ins(32,19,7,6) if opaque else ins(59,7,19,0)
            out+=a
            if opaque and op==248:out+=ins(59,19,20,7)
            if opaque and op==61 and a[2]==16:
                out+=ins(62,20,16)
                if live:out+=ins(61,6,21,20)
        return raw,struct.pack('<'+'I'*len(out),*out)

    def test_prune_unreferenced_descriptor_preserves_all_function_bytes(self):
        original,modified=self.fixture()
        converted,dead=prune_unused_bindings(modified)
        self.assertEqual(dead,[19]);self.assertEqual(converted,original)

    def test_remove_only_dead_opaque_locals(self):
        _,modified=self.fixture(opaque=True)
        converted,dead=remove_dead_opaque_locals(modified)
        self.assertEqual(dead,[20]);self.assertLess(len(converted),len(modified))
        if shutil.which('spirv-val'):
            with tempfile.TemporaryDirectory() as folder:
                p=Path(folder)/'test.spv';p.write_bytes(converted)
                subprocess.run(['spirv-val','--target-env','vulkan1.3',str(p)],check=True)
        _,live=self.fixture(opaque=True,live=True)
        self.assertEqual(remove_dead_opaque_locals(live),(live,[]))

    def test_fp8_e4m3_shared_staging_preserves_all_finite_values(self):
        for b in range(256):
            sign=-1 if b&128 else 1;e=(b>>3)&15;m=b&7
            if e==15 and m==7:continue
            value=sign*((m/8)*2**-6 if e==0 else (1+m/8)*2**(e-7))
            half=struct.unpack('<e',struct.pack('<e',value))[0]
            f32=struct.unpack('<f',struct.pack('<f',half))[0]
            self.assertEqual(value,f32)
        addresses=[lane+32*k for lane in range(32) for k in range(8)]
        self.assertEqual(sorted(addresses),list(range(256)))


if __name__=='__main__':unittest.main()
