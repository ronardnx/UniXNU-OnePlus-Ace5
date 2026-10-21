import importlib.util,json,tempfile,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('decoder',Path(__file__).resolve().parents[1]/'scripts/decode-ios16-full-log.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class Tests(unittest.TestCase):
    def test_byte_preservation_success_failure_and_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory);raw=base/'raw.log'
            parts=[bytes(range(64)),b'\0\xff\xe4\xbd',b'\xa0\xe5\xa5\xbd\n']
            lines=[b'EXEC pid=4 ppid=1 name=test\n']
            for i,part in enumerate(parts):
                lines.append(f'ULOG pid=4 name=test ticks={i:x} bytes={len(part)} hex={part.hex()}\n'.encode())
            lines += [lines[1],b'EXIT pid=4 ppid=1 name=test status=0x0\n',
                      b'EXIT pid=5 ppid=1 name=failed status=0x6\n',b'SMP --- end 0\n']
            raw.write_bytes(b''.join(lines));r=module.decode([raw],base/'out')
            self.assertEqual((base/'out/processes/test-4.log').read_bytes(),b''.join(parts))
            self.assertEqual(r['counts']['replayed_ulog_records'],1)
            self.assertEqual(r['incomplete_records'],[])
            self.assertEqual(len([x for x in r['events'] if x.startswith('EXIT ')]),2)

    def test_rotation_in_middle_of_record(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory);data=bytes(range(64))
            line=f'ULOG pid=4 name=test ticks=1 bytes=64 hex={data.hex()}\n'.encode()
            a=base/'serial-000.log';b=base/'serial-001.log'
            a.write_bytes(line[:91]);b.write_bytes(line[91:])
            r=module.decode([a,b],base/'out')
            self.assertEqual((base/'out/processes/test-4.log').read_bytes(),data)
            self.assertEqual(r['incomplete_records'],[])

if __name__=='__main__':unittest.main()
