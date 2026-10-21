import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]

def decode(data):
    nodes = {}
    def read(offset, parent):
        count, children = struct.unpack_from('<II', data, offset)
        offset += 8
        props = {}
        for _ in range(count):
            key, size = struct.unpack_from('<32sI', data, offset)
            offset += 36
            if offset + size > len(data):
                raise ValueError('Truncated ADT')
            props[key.rstrip(b'\0').decode()] = data[offset:offset+size]
            offset += (size + 3) & ~3
        path = parent + '/' + props['name'].rstrip(b'\0').decode()
        nodes[path] = props
        for _ in range(children):
            offset = read(offset, path)
        return offset
    if read(0, '') != len(data):
        raise ValueError('Trailing ADT data')
    return nodes

class FirstEntryDeviceTree(unittest.TestCase):
    def test_hardware_and_nvram(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'kernel.adt'
            subprocess.run([sys.executable, str(ROOT/'scripts/make-giulia-adt.py'),
                '--output', str(path), '--dram-size', '0x10000000'], check=True)
            nodes = decode(path.read_bytes())
        cpu = nodes['/device-tree/cpus/cpu0']
        self.assertEqual(cpu['compatible'], b'qcom,kryo\0')
        self.assertEqual(struct.unpack('<I',cpu['timebase-frequency'])[0], 19200000)
        self.assertEqual(len([p for p in nodes if p.startswith('/device-tree/cpus/')]), 1)
        bus = nodes['/device-tree/arm-io']
        _, base, _ = struct.unpack('<QQQ', bus['ranges'])
        gic = nodes['/device-tree/arm-io/intc']
        dist, dist_size, redist, redist_size = struct.unpack('<4Q',gic['reg'])
        self.assertEqual((base+dist,dist_size,base+redist,redist_size),
                         (0x17100000,0x10000,0x17180000,0x200000))
        self.assertFalse(any('ufshc' in p or 'gpu' in p for p in nodes))
        chosen = nodes['/device-tree/chosen']
        nvram = chosen['nvram-proxy-data']
        self.assertEqual(len(nvram),8192)
        self.assertEqual(struct.unpack_from('<I',nvram,16)[0],zlib.adler32(nvram[20:]))
        self.assertEqual(struct.unpack('<Q',chosen['darwinboot-memory-size'])[0],0x10000000)

    def test_reject_unqualified_memory_window(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'bad.adt'
            result = subprocess.run([sys.executable,str(ROOT/'scripts/make-giulia-adt.py'),
                '--output',str(path),'--dram-size','0x300000000'],capture_output=True)
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(path.exists())

    def test_dense_data_handoff(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'data.adt'
            subprocess.run([sys.executable,str(ROOT/'scripts/make-giulia-adt.py'),
                '--output',str(path),'--dram-size','0x10000000','--data-ramdisk'],check=True)
            nodes=decode(path.read_bytes())
        self.assertEqual(nodes['/device-tree/chosen/memory-map']['RAMDisk'],bytes(16))
        self.assertNotIn('PackedRAMDisk',nodes['/device-tree/chosen/memory-map'])
        self.assertEqual(nodes['/device-tree/chosen']['ephemeral-storage'],struct.pack('<I',1))

if __name__ == '__main__':
    unittest.main()
