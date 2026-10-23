"""Exercise recorded terminal reply handling with a fake port, never a device."""
from pathlib import Path
import sys
import unittest
import serial
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from gpu_terminal_live import Live


class FakePort:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.writes = []
        self.reads = 0

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def write(self, wire):
        self.writes.append(wire)
        return len(wire)

    def read(self, count):
        self.reads += 1
        if not self.chunks:
            raise AssertionError("Refusal was ignored; command is waiting for a timeout")
        chunk = self.chunks.pop(0)
        if len(chunk) > count:
            raise AssertionError("Fixture chunk exceeds requested read size")
        return chunk


class FakeCapture:
    def __init__(self):
        self.events = []
        self.chunks = []

    def feed(self, chunk):
        self.chunks.append(chunk)


class LiveReplyTests(unittest.TestCase):
    def setUp(self):
        # Any accidental serial constructor call or device enumeration fails.
        for target in ("gpu_terminal_live.serial.Serial", "gpu_terminal_live.list_ports.comports"):
            guard = patch(target, side_effect=AssertionError("Offline test accessed a device"))
            guard.start()
            self.addCleanup(guard.stop)

    def fixture(self, chunks):
        live = Live.__new__(Live)
        live.port = FakePort(chunks)
        live.capture = FakeCapture()
        return live

    def test_map_table_full_split_reply_is_rejected_without_timeout(self):
        chunks = [b"GIULIA REG: map table ", b"full\r\n"]
        live = self.fixture(chunks)
        with self.assertRaisesRegex(RuntimeError, "Device rejected .*map table full"):
            live.map(0x3d30000, 0x10000)
        self.assertEqual(live.port.reads, 2)
        self.assertEqual(live.port.writes, [b"reg.map 0x3d30000 0x10000\n"])
        self.assertEqual(live.capture.chunks, chunks)
        self.assertEqual(live.capture.events[0]["command"], "reg.map 0x3d30000 0x10000")

    def test_refusal_messages_are_rejected_on_first_reply(self):
        for reason in (
            "overlapping mapping; use reg.maps",
            "unmapped or misaligned address",
            "unmapped address",
            "disabled; requires giulia_debug=1 giulia_mmio=1",
            "map allocation failed",
            "invalid arguments; map BASE BYTES",
        ):
            with self.subTest(reason=reason):
                chunk = ("GIULIA REG: " + reason + "\r\n").encode()
                live = self.fixture([chunk])
                with self.assertRaises(RuntimeError) as caught:
                    live.command("reg.read 0x3d00000 32", r"GIULIA REG: read address=")
                self.assertIn("Device rejected", str(caught.exception))
                self.assertIn(reason, str(caught.exception))
                self.assertNotIn("Timed out", str(caught.exception))
                self.assertEqual(live.port.reads, 1)
                self.assertEqual(live.capture.chunks, [chunk])

    def test_successful_mapping_with_split_reply_still_parses(self):
        chunks = [b"GIULIA REG: mapped slot=15 base=0x3d30000 ",
                  b"bytes=0x10000 accessibility=unchecked no_device_access_yet\r\n"]
        live = self.fixture(chunks)
        self.assertEqual(live.map(0x3d30000, 0x10000), 15)
        self.assertEqual(live.port.reads, 2)
        self.assertEqual(live.capture.chunks, chunks)

    def test_disconnect_prevents_cleanup_or_retry_device_io(self):
        live=self.fixture([b'unused'])
        def gone(count):raise serial.SerialException('CDC disconnected')
        live.port.read=gone
        with self.assertRaises(serial.SerialException):live.read(0x3d7e000)
        self.assertTrue(live.disconnected)
        sent=len(live.port.writes)
        with self.assertRaisesRegex(RuntimeError,'I/O stopped'):live.write(0x3d7e000,1)
        self.assertEqual(len(live.port.writes),sent)


if __name__ == "__main__":
    unittest.main()
