"""Bounded, recorded transactions for this phone's explicit GPU experiments."""
from pathlib import Path
import sys, time, re
import serial
from serial.tools import list_ports
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from debug_terminal import Capture,wire_command

class Live:
    def __init__(self,name):
        ports=[p.device for p in list_ports.comports() if p.vid==0x1209 and p.pid==1 and p.serial_number=='ACE5DBG001']
        if len(ports)!=1:raise RuntimeError('Need one identified Ace 5 CDC device')
        self.port=serial.Serial(ports[0],115200,timeout=.02,write_timeout=2)
        self.capture=Capture(ROOT/'out/ios16'/name)
    def __enter__(self):return self
    def __exit__(self,*args):
        self.capture.save(self.port.port);self.capture.close();self.port.close()
        print('CAPTURE '+str(self.capture.root),flush=True)
    def command(self,line,pattern,timeout=4):
        self.capture.events.append({'command':line,'time':time.time()})
        wire=wire_command(line)
        if self.port.write(wire)!=len(wire):raise RuntimeError('Partial write')
        text='';end=time.monotonic()+timeout
        while time.monotonic()<end:
            raw=self.port.read(min(max(self.port.in_waiting,1),16384))
            if raw:
                self.capture.feed(raw);text+=raw.decode(errors='replace')
                if re.search(r'panic\(cpu|GIULIA REG: invalid|GIULIA REG: unmapped address',text):
                    raise RuntimeError('Device rejected '+line)
                match=re.search(pattern,text)
                if match:return match
        raise RuntimeError('Timed out '+line)
    def read(self,address,width=32):
        return int(self.command(f'reg.read {address:#x} {width}',
            rf'GIULIA REG: read address={address:#x} width={width} value=(0x[0-9a-f]+|0)\r?\n')[1],0)
    def write(self,address,value,width=32,verify=True):
        self.command(f'reg.write {address:#x} {width} {value:#x}',
            rf'GIULIA REG: write-issued address={address:#x} width={width} value=(0x[0-9a-f]+|0) readback=not-performed')
        if verify:
            actual=self.read(address,width)
            if actual!=value:raise RuntimeError(f'Readback {address:#x}: {actual:#x}, expected {value:#x}')
    def map(self,base,size):
        return int(self.command(f'reg.map {base:#x} {size:#x}',
            rf'GIULIA REG: mapped slot=(\d+) base={base:#x} bytes={size:#x}')[1])
    def unmap(self,slot):
        self.command(f'reg.unmap {slot}',rf'GIULIA REG: unmapped slot={slot}\b')
