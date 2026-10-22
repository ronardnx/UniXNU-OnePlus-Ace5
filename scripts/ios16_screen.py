"""Bounded GSC1 stream decoder, PNG writer, and localhost live screen viewer."""
import binascii
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import struct
import threading
import webbrowser
import zlib

MAGIC = b'\0GSC1\r\n\x1a'
HEADER = struct.Struct('<8sIIHHIIIIII')
MAX_BYTES = 316 * 695 * 2
MAX_CHUNK = 1024
SHELL_BEGIN = b'\r\nGIULIA SHELL BEGIN\r\n'
SHELL_END = b'\r\nGIULIA SHELL END\r\n'


class ScreenWire:
    """Remove screen packets before UTF-8/PTY parsing, preserving log bytes.

    Headers and payloads have separate CRCs. A malformed header enters bounded
    resynchronization: bytes are discarded until another verified screen header,
    rather than treating possible pixel bytes as terminal output.
    """
    def __init__(self):
        self.pending = bytearray()
        self.frame = None
        self.identity = None
        self.errors = 0
        self.quarantine = False
        self.shell = False
        self.stops = 0
        self.last_event = None

    def feed(self, data):
        self.last_event = None
        self.pending.extend(data)
        logs = bytearray()
        frames = []
        while self.pending:
            if self.shell:
                end = self.pending.find(SHELL_END)
                if end >= 0:
                    logs.extend(self.pending[:end + len(SHELL_END)])
                    del self.pending[:end + len(SHELL_END)]
                    self.shell = False
                    continue
                keep = 0
                for n in range(1, min(len(SHELL_END), len(self.pending) + 1)):
                    if self.pending[-n:] == SHELL_END[:n]:
                        keep = n
                ready = len(self.pending) - keep
                logs.extend(self.pending[:ready])
                del self.pending[:ready]
                break
            at = self.pending.find(MAGIC)
            shell_at = self.pending.find(SHELL_BEGIN) if not self.quarantine else -1
            if shell_at >= 0 and (at < 0 or shell_at < at):
                logs.extend(self.pending[:shell_at + len(SHELL_BEGIN)])
                del self.pending[:shell_at + len(SHELL_BEGIN)]
                self.shell = True
                continue
            if at < 0:
                keep = 0
                for marker in (MAGIC, SHELL_BEGIN) if not self.quarantine else (MAGIC,):
                    for n in range(1, min(len(marker), len(self.pending) + 1)):
                        if self.pending[-n:] == marker[:n]:
                            keep = max(keep, n)
                ready = len(self.pending) - keep
                if not self.quarantine:
                    logs.extend(self.pending[:ready])
                del self.pending[:ready]
                break
            if at:
                if not self.quarantine:
                    logs.extend(self.pending[:at])
                del self.pending[:at]
            if len(self.pending) < HEADER.size:
                break
            _, frame_id, flags, width, height, total, offset, size, crc, frame_crc, header_crc = HEADER.unpack_from(self.pending)
            control = flags == 2 and not any((width, height, total, offset, size, crc, frame_crc))
            valid = (frame_id and (control or (flags in (0, 1) and 1 <= width <= 316 and
                     1 <= height <= 695 and total == width * height * 2 and
                     total <= MAX_BYTES and 1 <= size <= MAX_CHUNK and
                     offset <= total and size <= total - offset)) and
                     binascii.crc32(self.pending[8:40]) == header_crc)
            if not valid:
                self.errors += 1
                self.frame = self.identity = None
                self.quarantine = True
                del self.pending[0]
                continue
            if len(self.pending) < HEADER.size + size:
                break
            payload = bytes(self.pending[HEADER.size:HEADER.size + size])
            del self.pending[:HEADER.size + size]
            self.quarantine = False
            if control:
                self.frame = self.identity = None
                self.stops += 1
                self.last_event = 'stop'
                continue
            if binascii.crc32(payload) != crc:
                self.errors += 1
                self.frame = self.identity = None
                self.quarantine = True
                continue
            identity = (frame_id, flags, width, height, total, frame_crc)
            if offset == 0:
                self.identity, self.frame = identity, bytearray()
            if self.identity != identity or self.frame is None or offset != len(self.frame):
                self.errors += 1
                self.frame = self.identity = None
                continue
            self.frame.extend(payload)
            if len(self.frame) == total:
                if binascii.crc32(self.frame) == frame_crc:
                    frames.append((frame_id, flags, width, height, bytes(self.frame)))
                    self.last_event = 'frame'
                else:
                    self.errors += 1
                self.frame = self.identity = None
        return bytes(logs), frames


def rgb565_png(width, height, pixels):
    if not 1 <= width <= 316 or not 1 <= height <= 695 or len(pixels) != width * height * 2:
        raise ValueError('Invalid screen dimensions/pixel length')
    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)
        for x in range(width):
            n = 2 * (y * width + x)
            pixel = pixels[n] | pixels[n + 1] << 8
            r, g, b = (pixel >> 11) & 31, (pixel >> 5) & 63, pixel & 31
            scanlines.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    def chunk(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', binascii.crc32(name + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(scanlines, 3)) + chunk(b'IEND', b''))


class ScreenStore:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.lock = threading.Lock()
        self.png = None
        self.info = {'frames': 0, 'state': 'waiting for a real framebuffer frame'}

    def _publish_info_locked(self):
        self.directory.mkdir(exist_ok=True)
        temporary = self.directory / 'screen.json.tmp'
        temporary.write_text(json.dumps(self.info, indent=2), encoding='utf-8')
        temporary.replace(self.directory / 'screen.json')

    def set_state(self, state):
        with self.lock:
            self.info['state'] = state
            self._publish_info_locked()

    def receive(self, frame):
        frame_id, snapshot, width, height, pixels = frame
        png = rgb565_png(width, height, pixels)
        self.directory.mkdir(exist_ok=True)
        latest = self.directory / 'latest.png'
        temporary = self.directory / 'latest.tmp'
        temporary.write_bytes(png)
        temporary.replace(latest)
        if snapshot:
            (self.directory / f'screen-{frame_id:08x}.png').write_bytes(png)
        with self.lock:
            self.png = png
            self.info = {'frames': self.info['frames'] + 1, 'frame_id': frame_id,
                         'width': width, 'height': height, 'snapshot': bool(snapshot),
                         'received_utc': datetime.now(timezone.utc).isoformat(),
                         'state': 'snapshot received' if snapshot else 'live frame received', 'latest_png': str(latest.resolve()),
                         'source': 'actual panel framebuffer, 4x point sampling, RGB565'}
            self._publish_info_locked()
        return latest


VIEWER_HTML = b'''<!doctype html><meta charset="utf-8"><title>Ace 5 live screen</title>
<style>body{background:#16191d;color:#eee;font:15px system-ui;margin:20px}img{display:block;
max-height:85vh;max-width:95vw;background:#111;margin:12px 0;border:1px solid #555}
a{color:#99caff}#status{white-space:pre-wrap}</style>
<h2>Ace 5 live screen</h2><div id="status">Waiting for a real framebuffer frame...</div>
<img id="screen" hidden alt="Real phone framebuffer"><a href="/frame.png" download="ace5-screen.png">Save latest PNG</a>
<script>let last=0;async function update(){try{let r=await fetch('/status',{cache:'no-store'});
let s=await r.json();document.querySelector('#status').textContent=s.state+(s.received_utc?' | '+s.received_utc:'');
if(s.frames&&s.frames!==last){last=s.frames;let i=document.querySelector('#screen');i.src='/frame.png?v='+last;i.hidden=false;}}
catch(e){document.querySelector('#status').textContent='Viewer disconnected';}setTimeout(update,300)}update();</script>'''


class ScreenViewer:
    def __init__(self, store):
        self.store = store
        class Handler(BaseHTTPRequestHandler):
            def do_GET(handler):
                path = handler.path.split('?', 1)[0]
                with store.lock:
                    if path == '/':
                        payload, mime = VIEWER_HTML, 'text/html; charset=utf-8'
                    elif path == '/status':
                        payload, mime = json.dumps(store.info).encode(), 'application/json'
                    elif path == '/frame.png' and store.png is not None:
                        payload, mime = store.png, 'image/png'
                    else:
                        handler.send_error(404)
                        return
                handler.send_response(200)
                handler.send_header('Content-Type', mime)
                handler.send_header('Content-Length', str(len(payload)))
                handler.send_header('Cache-Control', 'no-store')
                handler.end_headers()
                handler.wfile.write(payload)
            def log_message(handler, *_args):
                pass
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_port}/'

    def open(self):
        webbrowser.open(self.url)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
