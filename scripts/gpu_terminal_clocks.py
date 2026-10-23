"""SM8650 cold GMU clocks adapted from UniXNU-sheng 251b291 GpuPowerBoard.h.

Uses SM8650 gpucc-pineapple.c PLL1 default (520 MHz), GMU /2 (260 MHz),
HUB GPLL0 /3 (200 MHz). Requires CX already powered and GMU reset held.
No voltage changes, GMU reset release or SMMU translation enable. Clock
configuration is retained until reboot. Rates are configured, not measured.
"""
import json,time


def verify_uefi(live, report):
    """Verify automatic preparation without changing hardware state."""
    slots=[]
    values={}
    try:
        for base,size in ((0x170000,0x4000),(0x3d90000,0x10000),(0x3d7c000,0x4000)):
            slots.append(live.map(base,size))
        for address,mask,expected in (
            (0x171004,0x80000001,1),(0x171010,0x80000001,1),
            (0x3d99168,0x80000000,0x80000000),(0x3d97000,0x80000001,1),
            (0x3d91000,0x80000005,0x80000005),(0x3d91010,0xffffffff,0x4444001b),
            (0x3d91014,0xffffffff,0x1555),(0x3d9931c,0xffffffff,0x303),
            (0x3d993f0,0xffffffff,0x505),(0x3d9913c,0x80000001,1),
            (0x3d7e000,0xffffffff,1),(0x3d7e070,0xffffffff,0)):
            value=live.read(address);values[hex(address)]=hex(value)
            if value&mask!=expected:raise RuntimeError(f'UEFI clock mismatch {address:#x}: {value:#x}')
        report['uefi_clocks_verified']=True
        report['uefi_clock_registers']=values
    finally:
        if not live.disconnected and not getattr(live,'io_stopped',False):
            for slot in reversed(slots):live.unmap(slot)


def prepare(live, report):
    state={'writes':[], 'configured_gmu_hz':260000000,
           'configured_hub_hz':200000000,'frequency_measured':False,'complete':False}
    report['clock_preparation']=state
    slots=[]
    def checkpoint():
        (live.capture.root/'clock-preparation.json').write_text(json.dumps(state,indent=2)+'\n')
    def write(address,value,mask=0xffffffff):
        state['writes'].append({'address':hex(address),'value':hex(value),'mask':hex(mask)})
        checkpoint()
        live.write(address,value,verify=False)
        observed=live.read(address)
        state['writes'][-1]['observed']=hex(observed)
        if (observed&mask)!=(value&mask):
            raise RuntimeError(f'Clock readback {address:#x}: {observed:#x}, requested {value:#x}, mask {mask:#x}')
    def poll(address,mask,expected):
        for _ in range(20):
            value=live.read(address)
            if value&mask==expected:return value
            time.sleep(.001)
        raise RuntimeError(f'Clock poll {address:#x}: {value:#x}')
    try:
        slots=[live.map(0x170000,0x4000),live.map(0x3d90000,0x10000),
               live.map(0x150000,0x4000)]
        if not live.read(0x171004)&1 or not live.read(0x3d99168)&0x80000000:
            raise RuntimeError('Requires inherited CFG access and powered CX')
        # Verify a cold XO source and unconfigured PLL before any mutation.
        for address,expected in ((0x3d91000,0x100),(0x3d91004,0),
                                 (0x3d99318,0),(0x3d9931c,0),(0x3d993ec,0),(0x3d993f0,0)):
            if live.read(address)!=expected:raise RuntimeError(f'Not cold at {address:#x}')
        for address in (0x171004,0x171010):
            write(address,(live.read(address)&~2)|1,3)
            poll(address,0x80000001,1)
        # AHB is HALT_DELAY; its enable bit can read zero on this firmware.
        # Require the on indication, then enable the independent SMMU vote.
        write(0x3d9911c,live.read(0x3d9911c)|1,0x80000000)
        write(0x3d97000,live.read(0x3d97000)|1,1)
        poll(0x3d97000,0x80000001,1)
        slots += [live.map(0x3d7c000,0x4000),live.map(0x3da0000,0x8000)]
        if live.read(0x3d7e000)!=1 or live.read(0x3d7e070)!=0:
            raise RuntimeError('GMU must be held in reset with no firmware-ready state')
        if live.read(0x3da0020)!=0x4c017e18 or live.read(0x3da0048)!=0:
            raise RuntimeError('Unexpected SMMU identity or global fault')
        write(0x152000,live.read(0x152000)|0x18000)
        for off,value in ((0x10,0x4444001b),(0x14,0x1555),(0x20,0x20485699),
                          (0x24,0x00182261),(0x28,0x82aa299c),(0x1c,5),
                          (0x30,3),(0x34,0x9000),(0x38,0x34)):
            write(0x3d91000+off,value)
        # MODE contains asynchronous status bits; verify control bits only.
        write(0x3d91000,live.read(0x3d91000)|4,7)
        write(0x3d91004,1)
        poll(0x3d91000,0x80000000,0x80000000)
        write(0x3d91018,live.read(0x3d91018)|7)
        write(0x3d91000,live.read(0x3d91000)|1,7)
        for address,sel,div in ((0x3d993ec,5,5),(0x3d99318,3,3)):
            live.write(address,live.read(address)|2,verify=False)
            poll(address,0x80000000,0)
            cfg=(live.read(address+4)&~(0x1f|7<<8|3<<12|1<<20))|sel<<8|div
            write(address+4,cfg)
            live.write(address,live.read(address)|1,verify=False)
            cmd=poll(address,1,0)
            live.write(address,cmd&~2,verify=False)
            if live.read(address+4)!=cfg:raise RuntimeError('RCG configuration changed')
        for address in (0x3d99004,0x3d9900c,0x3d9913c,0x3d99144,0x3d99148):
            # CXO_AON is an always-on branch; its enable readback is not a vote.
            write(address,live.read(address)|1,0x80000000 if address==0x3d99004 else 1)
        poll(0x3d9913c,0x80000001,1)
        state['complete']=True
    except Exception as error:
        state['error']=str(error)
        raise
    finally:
        checkpoint()
        if not live.disconnected and not getattr(live,'io_stopped',False):
            for slot in reversed(slots):live.unmap(slot)
