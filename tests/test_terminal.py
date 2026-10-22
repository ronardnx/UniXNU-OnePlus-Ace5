import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('terminal', ROOT/'scripts/debug_terminal.py')
terminal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(terminal)


class Terminal(unittest.TestCase):
    def test_identity_does_not_select_unrelated_adapters(self):
        ports = [SimpleNamespace(device='COM1',vid=0x1209,pid=1,serial_number='other'),
                 SimpleNamespace(device='COM2',vid=0x1209,pid=1,serial_number='ACE5DBG001')]
        self.assertEqual(terminal.matching_ports(ports), ['COM2'])

    def test_commands_reject_injection(self):
        for command in ('ping\nreplay', 'x'*64, 'ls', 'reboot', 'write 0x1000 1'):
            with self.assertRaises(ValueError):
                terminal.wire_command(command)
        self.assertEqual(terminal.wire_command('ps'), b'ps\n')

    def test_split_utf8_and_historical_topic_export(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = terminal.Capture(Path(directory))
            try:
                data = 'GIULIA WIFI: 测试\nGIULIA GPU: ready\n'.encode()
                a = capture.feed(data[:14])
                b = capture.feed(data[14:])
                self.assertEqual(a+b, data.decode())
                capture.save('COM2')
                self.assertEqual((capture.root/'device-000.log').read_bytes(), data)
                self.assertIn('测试', (capture.root/'wifi.txt').read_text(encoding='utf-8'))
                self.assertNotIn('GPU', (capture.root/'wifi.txt').read_text(encoding='utf-8'))
                self.assertIn('historical', (capture.root/'session.json').read_text())
            finally:
                capture.close()

    def test_queue_polling_and_partial_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = terminal.Capture(Path(directory))
            try:
                commands = terminal.Commands(capture, 'fake')
                with self.assertRaises(ValueError):
                    commands.accept('watch status 0')
                with self.assertRaises(ValueError):
                    commands.accept('watch replay 1')
                with self.assertRaises(ValueError):
                    commands.accept('watch wifi.start 1')
                with self.assertRaises(ValueError):
                    commands.accept('watch reboot.fastboot 1')
                commands.accept('collect')
                with self.assertRaises(ValueError):
                    commands.accept('collect')
                writes = []
                fake = SimpleNamespace(write=lambda data: writes.append(data) or len(data))
                commands.tick(fake)
                self.assertEqual(writes, [b'features\n'])
                commands.next_send = 0
                with self.assertRaises(OSError):
                    commands.tick(SimpleNamespace(write=lambda data: 0))
            finally:
                capture.close()

    def test_control_characters_never_reach_terminal(self):
        self.assertEqual(terminal.safe_text('abc\x1b\x00\x9b\n'), 'abc\n')

    def test_register_validation_and_pcie_aperture(self):
        valid=('reg.map 0x60000000 0x4000', 'reg.maps', 'reg.read 0x60000000 32',
               'reg.write 0x60000000 32 0xffffffff', 'reg.rmw 0x60000000 32 3 2',
               'reg.dump 0x60000000 32 64','reg.unmap 15','shell','shell.size 120 40','wifi.status','wifi.start')
        for cmd in valid:self.assertEqual(terminal.wire_command(cmd),(cmd+'\n').encode())
        invalid=('reg.map 0x80000000 0x4000', 'reg.map 0x60000001 0x4000',
                 'reg.map 0x60000000 0x20000','reg.read 0x60000001 32',
                 'reg.read 0x60000000 128','reg.write 0x60000000 8 256',
                 'reg.write 0x60000000 64 0x10000000000000000',
                 'reg.rmw 0x60000000 32 1 2','reg.dump 0x60000000 32 65',
                 'reg.dump 0xfffffffffffffff8 64 2','reg.unmap 16',
                 'reg.read 0x60000000 32\nshell','reg.read 0x60000000 32;reboot',
                 'shell.size 0 24','shell.size 0x80 24')
        for cmd in invalid:
            with self.subTest(cmd=cmd),self.assertRaises(ValueError):terminal.wire_command(cmd)

    def test_raw_shell_markers_split_at_every_byte(self):
        raw=b'\x1b[32mroot# \xe4\xbd\xa0\xe5\xa5\xbd\x00\xff'
        payload=b'kernel log\n'+terminal.ShellWire.BEGIN+raw+terminal.ShellWire.END+b'log again\n'
        for split in range(len(payload)+1):
            wire=terminal.ShellWire()
            events=wire.feed(payload[:split])+wire.feed(payload[split:])
            self.assertEqual(b''.join(data for kind,data in events if kind=='raw'),raw)
            self.assertEqual(b''.join(data for kind,data in events if kind=='log'),b'kernel log\nlog again\n')
            self.assertEqual([kind for kind,_ in events if kind in ('begin','end')],['begin','end'])
            self.assertFalse(wire.active)
            self.assertFalse(wire.pending)

    def test_shell_prompt_is_not_delayed_waiting_for_newline(self):
        wire=terminal.ShellWire()
        wire.feed(wire.BEGIN)
        self.assertEqual(wire.feed(b'root# '),[('raw',b'root# ')])
        self.assertEqual(wire.feed(b'\r'),[])
        self.assertEqual(wire.feed(b'X'),[('raw',b'\rX')])

    def test_reboot_requires_exact_command_and_actual_reset_ack(self):
        for command in ('reboot.fastboot','reboot.status'):
            self.assertEqual(terminal.wire_command(command),(command+'\n').encode())
        for command in ('reboot.fastboot now','reboot.fastboot\nping','reboot.fastboot;ping'):
            with self.assertRaises(ValueError):terminal.wire_command(command)
        with tempfile.TemporaryDirectory() as directory:
            capture=terminal.Capture(Path(directory))
            try:
                commands=terminal.Commands(capture,'fake')
                commands.accept('reboot.fastboot')
                commands.tick(SimpleNamespace(write=lambda data:len(data)))
                self.assertTrue(commands.reboot_sent)
                with self.assertRaises(ValueError):commands.accept('wifi.start')
                capture.feed(b'GIULIA REBOOT: accepted target=fastboot\n')
                self.assertFalse(terminal.reboot_acknowledged(capture))
                capture.feed(b'GIULIA REBOOT: resetting target=fastboot disks=shutdown-complete\n')
                self.assertTrue(terminal.reboot_acknowledged(capture))
                self.assertFalse(terminal.reboot_failed(capture))
                capture.feed(b'GIULIA REBOOT: PSCI SYSTEM_RESET returned=-1\n')
                self.assertTrue(terminal.reboot_failed(capture))
            finally:capture.close()

    def test_fastboot_wait_matches_requested_device(self):
        times=iter([0,.1,.2,.6,.7,.8])
        runner=Mock(side_effect=[SimpleNamespace(returncode=0,stdout='other\tfastboot\n'),
                                SimpleNamespace(returncode=0,stdout='wanted\tfastboot\n')])
        self.assertEqual(terminal.wait_fastboot('wanted',2,runner,lambda:next(times),lambda _:None),0)
        self.assertEqual(runner.call_count,2)
        with self.assertRaises(TimeoutError):
            terminal.wait_fastboot('wanted',1,runner,iter([0,2]).__next__,lambda _:None)

    def test_reboot_serial_flow_requires_disconnect_without_reset_failure(self):
        ack=b'GIULIA REBOOT: resetting target=fastboot disks=shutdown-complete\n'
        failure=b'GIULIA REBOOT: PSCI SYSTEM_RESET returned=-1\n'

        class Disconnected(OSError):
            pass

        class Session:
            def __init__(self, scenario):
                self.scenario=scenario
                self.incoming=[]
                self.writes=[]
                self.closed=False

            def __enter__(self):return self
            def __exit__(self,*args):self.closed=True

            @property
            def in_waiting(self):
                if self.writes and not self.incoming and self.scenario=='disconnect':
                    raise Disconnected('device left USB CDC')
                return len(self.incoming[0]) if self.incoming else 0

            def read(self,size):
                return self.incoming.pop(0) if size else b''

            def write(self,data):
                self.writes.append(data)
                self.incoming.append(ack)
                if self.scenario=='psci-return':self.incoming.append(failure)
                return len(data)

        for scenario in ('psci-return','stays-online','disconnect'):
            with self.subTest(scenario=scenario),tempfile.TemporaryDirectory() as directory:
                session=Session(scenario)
                serial_module=SimpleNamespace(Serial=Mock(return_value=session),SerialException=Disconnected)
                args=SimpleNamespace(list=False,port='COM_FAKE',command='reboot.fastboot',
                                     output=directory,seconds=1)
                clock=[0.0]
                def advance(_seconds):clock[0]+=.25
                with patch.object(terminal.time,'monotonic',side_effect=lambda:clock[0]), \
                     patch.object(terminal.time,'sleep',side_effect=advance),patch('builtins.print'):
                    if scenario=='psci-return':
                        with self.assertRaisesRegex(OSError,'Kernel reset returned'):
                            terminal.run(args,serial_module,[])
                        self.assertLess(clock[0],args.seconds)
                    elif scenario=='stays-online':
                        with self.assertRaisesRegex(TimeoutError,'reset and disconnect'):
                            terminal.run(args,serial_module,[])
                    else:
                        self.assertEqual(terminal.run(args,serial_module,[]),0)
                self.assertEqual(session.writes,[b'reboot.fastboot\n'])
                self.assertTrue(session.closed)
                captured=b''.join(p.read_bytes() for p in Path(directory).rglob('device-*.log'))
                self.assertIn(ack,captured)
                if scenario=='psci-return':self.assertIn(failure,captured)


if __name__ == '__main__':
    unittest.main()
