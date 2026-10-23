"""Audio metadata is reachable through explicit USB commands without device access."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
import debug_terminal as terminal
from ios16_terminal import audio_terminal_source, terminal_sources


class AudioTerminal(unittest.TestCase):
    def test_exact_metadata_commands(self):
        for command in ('audio.status', 'audio.map', 'audio.preflight', 'audio.start', 'audio.stop', 'audio.test'):
            self.assertEqual(terminal.wire_command(command), (command+'\n').encode())
            for suffix in (' ', ' now', ';ping', '\nping'):
                with self.subTest(command=command+suffix), self.assertRaises(ValueError):
                    terminal.wire_command(command+suffix)
        for command in ('audio', 'audio.play', 'audio.record', 'audio.write'):
            with self.subTest(command=command), self.assertRaises(ValueError):
                terminal.wire_command(command)

    def test_capture_topic_and_explicit_queries(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = terminal.Capture(Path(directory))
            try:
                commands = terminal.Commands(capture, 'fixture')
                self.assertFalse(commands.queue)
                for command in ('audio.start','audio.stop','audio.test'):
                    with self.assertRaises(ValueError): commands.accept('watch '+command+' 2')
                commands.accept('all')
                self.assertFalse(any(command.startswith('audio.') for command in commands.queue))
                collection = terminal.Commands(capture, 'fixture')
                collection.accept('collect')
                self.assertFalse(any(command.startswith('audio.') for command in collection.queue))
                commands.queue.clear()
                commands.accept('audio.map')
                self.assertEqual(list(commands.queue), ['audio.map'])
                capture.feed(b'GIULIA AUDIO: stage=board-mapping-only playback=0\n'
                             b'GIULIA GPU: unrelated\n')
                self.assertEqual(len(capture.filtered('audio')), 1)
                self.assertIn('HISTORICAL SESSION LOGS', commands.accept('logs audio'))
                capture.save('fixture')
                audio_log = (capture.root/'audio.txt').read_text(encoding='utf-8')
                self.assertIn('playback=0', audio_log)
                self.assertNotIn('GPU', audio_log)
            finally:
                capture.close()

    def test_embedded_in_every_terminal_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            usb = Path(directory)/'usb.c'
            block = Path(directory)/'block.c'
            usb.write_text('pinned USB source placeholder')
            profiles = ((False, False, False), (False, True, False),
                        (False, True, True), (True, True, False), (True, True, True))
            for wifi, gui, gpu in profiles:
                changes = []
                terminal_sources(usb, block, lambda *change: changes.append(change),
                                 wifi=wifi, gui=gui, gpu=gpu)
                source = next(new for _, old, new in changes if old == 'static void command_line(void)')
                self.assertEqual(source.count('static int gaudio_command('), 1)
                self.assertLess(source.index('static int gaudio_command('),
                                source.index('if(gaudio_command(cmd))'))
                self.assertIn('audio=audio.status,audio.map', source)
                help_source = next(new for _, old, new in changes if old == 'commands help status ping replay')
                self.assertIn('audio.status audio.map', help_source)
                if wifi:
                    self.assertIn('if(giulia_wifi_terminal(cmd) || gs_command(cmd)', source)

    def test_host_and_kernel_embedded_c_compile_and_run(self):
        compiler = os.environ.get('CC') or next(
            (path for name in ('clang', 'cc', 'gcc') if (path := shutil.which(name))), None)
        if not compiler:
            self.skipTest('Set CC or install a C compiler for the audio metadata bridge test')
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            source = audio_terminal_source()
            self.assertNotIn('#include <stdint.h>', source)
            self.assertNotIn('#include <stddef.h>', source)
            (folder/'audio-generated.h').write_text(source)
            for embedded in (False, True):
                with self.subTest(kernel_embedded=embedded):
                    binary = folder/('audio-embedded' if embedded else 'audio-host')
                    if os.name == 'nt':
                        binary = binary.with_suffix('.exe')
                    command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                               str(ROOT/'tests/ios16_audio_terminal.c'), '-I', str(folder),
                               '-o', str(binary)]
                    if embedded:
                        command.append('-DGAUDIO_KERNEL_EMBEDDED')
                    build = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
                    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
                    self.assertIn('Audio metadata terminal checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
