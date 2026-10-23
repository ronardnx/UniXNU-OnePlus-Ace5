"""One bounded stock-GMU/HFI polling experiment on the verified HFI boot.

Uses kernel-owned pinned RAM and GMU bank 2/SID 5. Programs the cold
SM8650 PLL/RCG defaults before loading the original firmware. No voltage
changes, GPU stream attachment or host IRQ enable. Restores held reset, FAULT
routing, disabled translation, saved config and terminal clock gates.
"""
from gpu_terminal_live import Live,ROOT
from gpu_terminal_native import NativeHfi
from gpu_terminal_diagnostics import snapshot
import hashlib,json,struct,re,time

import argparse
parser=argparse.ArgumentParser(description='Cold SM8650 GMU clock, SMMU SID5 and stock HFI smoke test. No GPU submission.')
parser.add_argument('--execute',action='store_true')
parser.add_argument('--uefi-clocks',action='store_true',help='Verify the automatic UEFI clocks instead of programming them')
args=parser.parse_args()
if not args.execute:parser.error('Use --execute to perform the bounded hardware experiment')
fw=(ROOT/'out/ios16/gpu-stock-firmware/gmu_gen70900.bin').read_bytes()
assert hashlib.sha256(fw).hexdigest()=='9685f62c42543befcaee68d25355e16bf55cdb0d573bddcc9f6633f59eec3f72'
root_pa=0  # Obtained freshly from the running service below.
blocks=[];off=0
while off<len(fw):
    address,n,kind,value=struct.unpack_from('<4I',fw,off);off+=16
    if n and (address<0x4000 or 0x10004000<=address<0x10008000):
        pa=0x3d6d000+address if address<0x4000 else 0x3d71000+address-0x10004000
        blocks.append((pa,fw[off:off+n]))
    off+=n
assert sum(len(data) for _,data in blocks)==540
report={'firmware_sha256':hashlib.sha256(fw).hexdigest(),'root_pa':hex(root_pa),
    'stage_requested':'GMU-HFI-tables','aqe_requested':False,
    'stock_fw_ready':False,'hfi_ready':False,'query_ack':False,
    'polling_only':True,'clock_rates_changed':False,'secure_cx_call_executed':False,
    'new_bandwidth_vote':False,'metal_ready':False}
report['raw_cp_submitted']=False
saved=[];maps=[];modified=False;attached=False;diagnostic_ready=False
with Live('gpu-gmu-clock-smmu-smoke') as live:
    def capture_phase(phase,include_gx=False):
        entry=snapshot(live,report,live.capture.root/'result.json',phase,
            cx_ready=True,include_gx=include_gx,
            gx_held=include_gx and report.get('oob_ack',False))
        if entry['status']!='complete' or (include_gx and entry['gx']['status']!='complete'):
            raise RuntimeError('Incomplete diagnostic snapshot: '+phase)
    from gpu_terminal_clocks import prepare as prepare_clocks,verify_uefi
    if args.uefi_clocks:verify_uefi(live,report)
    else:
        prepare_clocks(live,report)
        report['clock_rates_changed']=True
    transport=NativeHfi(live,report)
    if not re.search(r'^GIULIA GPU DEV: resources_abi=1 memstore=1 secure_allocation_mappings=1$',transport.info,re.M):
        raise RuntimeError('Current kernel lacks dedicated memstore and per-allocation secure mappings')
    root_pa=transport.gmu_root
    report['root_pa']=hex(root_pa);report['gpu_root_pa']=hex(transport.gpu_root)
    report['context_banks']={'gmu':2,'gpu':0,'gpu_reserved':[0,1]};
    report['native_transport']=True;report['temporary_nmi_vector_modified']=False
    def rw(a,v,width=32,verify=True):live.write(a,v,width,verify)
    def config(a,v,verify=True):
        saved.append((a,live.read(a)));rw(a,v,verify=verify)
        if not verify:
            report.setdefault('unverified_config_readbacks',[]).append({'address':hex(a),'requested':hex(v),'observed':hex(live.read(a))})
    def reg(word):return 0x3d00000+4*word
    def poll(a,mask,expected,seconds=3):
        end=time.monotonic()+seconds;seen=[]
        while time.monotonic()<end:
            value=live.read(a)
            if not seen or seen[-1]!=value:seen.append(value)
            if value&mask==expected:return True,seen
        return False,seen
    try:
        for base,size in ((0x3d90000,0x10000),(0x170000,0x4000),(0x3da0000,0x8000),(0x3d7c000,0x4000)):
            maps.append(live.map(base,size))
        rw(0x171004,live.read(0x171004)&~2,verify=False)
        rw(0x171010,live.read(0x171010)&~2,verify=False)
        rw(0x3d97000,live.read(0x3d97000)|1,verify=False)
        # The bus gates were explicitly enabled and surveyed immediately before
        # this experiment; no RCG/PLL/source-rate registers are changed here.
        for a in (0x171004,0x171010,0x171018,0x3d97000):assert live.read(a)&0x80000001==1
        assert live.read(0x3d7e000)==1 and live.read(0x3d7e070)==0
        assert live.read(0x3da0020)==0x4c017e18 and live.read(0x3da0048)==0
        for base,size in ((0x3dc0000,0x10000),(0x3dd0000,0x8000),
            (0x3d6c000,0x4000),(0x3d70000,0x4000),(0x3d74000,0x4000),(0x3d50000,0x4000),
            (0xb284000,0x4000),(0x3d00000,0x10000),(0x3d8c000,0x4000)):
            maps.append(live.map(base,size))
        for i in range(24):
            assert live.read(0x3da0800+4*i)==0 and live.read(0x3da0c00+4*i)==0x20000
        for i in range(22):assert not live.read(0x3dc0000+0x1000*i)&1
        diagnostic_ready=True
        assert not live.read(0x3dc2058)&0xc00001fe
        report['hfi_status_before']=live.read(reg(0x1f980))
        live.command('gpu.hfi.pin',r'GIULIA GPU HFI: memory_pinned_until_reboot=1')
        modified=True
        from gpu_terminal_smmu import prepare_prr
        prepare_prr(live,transport,report,config)
        # Use the same native4K format already validated by actual CM3 DMA.
        rw(0x3da1808,1);rw(0x3da1008,0x1f300)
        rw(0x3dc2010,0x38011,verify=False);assert live.read(0x3dc2010)==0x38071
        rw(0x3dc2030,0x80802020)
        rw(0x3dc2020,(0x750<<48)|root_pa,64);rw(0x3dc2028,0x750<<48,64)
        rw(0x3dc2038,0xff44);rw(0x3dc203c,0)
        rw(0x3dc2058,0xffffffff,verify=False)
        rw(0x3dc2618,0,verify=False);rw(0x3dc27f0,0xffffffff,verify=False)
        assert live.read(0x3dc27f4)==0
        for pa,data in blocks:
            assert live.read(0x3d7e000)==1
            for i in range(0,len(data),4):rw(pa+i,struct.unpack_from('<I',data,i)[0])
        assert live.read(0x3d74ff0)==0x20060001
        # PDC and the Gen7.2-family RSC layout used by Gen7.9.1.
        config(0xb280000+4*0x1148,0);config(0xb280000+4*0x1140,0x80000001)
        rsc=[(4,1<<24),(0xb,1),(0xe,0),(0xd,0),(0x10,0),(0xf,0),
             (0x12,0x80000021),(0x11,0),(0x100,0),(8,0x4520),(9,0x4510),(10,0x4514)]
        rsc += [(0x154+i,v) for i,v in enumerate((0xeaaae5a0,0xe1a1ebab,0xa2e0a581,0xecac82e2,0x0020edad))]
        for word,value in rsc:
            # Hidden TCS address/data fields can retain hardware/firmware-owned
            # routing. Vendor writes have no readback contract. Record these
            # differences explicitly; this polling trial does not exercise sleep.
            config(0x3d50000+4*word,value,verify=not 0xd<=word<=0x12)
        for word,value in ((0x1f8f1,1),(0x1f8f0,1),(0x1f81c,0),(0x1f801,2),
            (0x1f985,0x60010000),(0x1f984,1),(0x23b11,0x80c808a0),
            (0x1f82d,0x4052),(0x3c45,0),(0x1f9d0,0x60040000),
            (0x1f9ce,0),(0x1f9cf,0x07090100),(0x1f9cd,0x6003000f),
            (0x1f40f,1),(0x1f400,1),(0x1f401,1),
            (0x1f993,0xffffffff),(0x23b06,0xffffffff),(0x1f986,0)):
            config(reg(word),value,verify=word!=0x23b06)
            if word==0x23b06:assert live.read(reg(word))&0x7f==0x7f
        rw(reg(0x1f991),0xffffffff,verify=False);rw(reg(0x23b04),0xffffffff,verify=False)
        report['counter_before']=live.read(reg(0x1f880))
        report['counter_after']=live.read(reg(0x1f880))
        assert report['counter_before']!=report['counter_after']
        rw(0x3dc2000,0x1027,verify=False);assert live.read(0x3dc2000)==0x27
        attached=True;rw(0x3da0800,0x80000005);rw(0x3da0c00,2)
        capture_phase('streams_prepared')
        print('Stock TCM/config verified; starting bounded GMU polling experiment',flush=True)
        rw(0x3d7e000,0)
        report['stock_fw_ready'],report['fw_states']=poll(0x3d7e070,0x1ff,0x100)
        print('Firmware ready: '+str(report['stock_fw_ready'])+' '+str(report['fw_states']),flush=True)
        if not report['stock_fw_ready']:raise RuntimeError('GMU firmware-ready timeout')
        if report['stock_fw_ready']:
            rw(reg(0x1f986),1)
            report['hfi_ready'],report['hfi_states']=poll(reg(0x1f980),1,1)
            if not report['hfi_ready']:raise RuntimeError('GMU HFI-ready timeout')
            if report['hfi_ready']:
                from gpu_terminal_stock_tables import tables
                perf,bw,metadata=tables()
                assert perf[7]==0x340201 and perf[14]==0x400202 and perf[16]==0x800303
                report['offline_table_sources']=metadata
                reply=transport.send(12,[125,0])
                assert reply['value']==1 and reply['status']==0
                report['query_ack']=True
                for msg,payload in ((15,perf),(3,bw),(11,[1,1,0]),(11,[16,1,0])):
                    reply=transport.send(msg,payload)
                    print('INIT ACK id='+str(reply['id'])+' value='+str(reply['value'])+' status='+str(reply['status']),flush=True)
                    assert reply['value']==0 and reply['status'] in (0,None)
                report['performance_and_bandwidth_tables_accepted']=True
                report['hwsched_feature_enabled']=True
        report.update(smmu_fsr=live.read(0x3dc2058),
            global_fault=live.read(0x3da0048),gmu_irq=live.read(reg(0x1f992)),ao_irq=live.read(reg(0x23b05)))
        report['smmu_fault_valid']=bool(report['smmu_fsr']&0xc00001fe)
        if report['smmu_fault_valid']:report['smmu_far']=live.read(0x3dc2060,64)
        if report['smmu_fault_valid'] or report['global_fault']:
            raise RuntimeError('SMMU fault after GMU/HFI transaction')
        live.command('gpu.hfi.log',r'GIULIA GPU HFI: log_word=63 value=')
        print(json.dumps(report,indent=2),flush=True)
    except Exception as error:
        import traceback
        report['error']=traceback.format_exc();print(report['error'],flush=True)
        try:
            if diagnostic_ready:capture_phase('failure_before_cleanup')
            report['smmu_fsr']=live.read(0x3dc2058)
            report['global_fault']=live.read(0x3da0048)
            report['gmu_irq']=live.read(reg(0x1f992))
        except Exception as diagnostic_error:report['diagnostic_error']=str(diagnostic_error)
    finally:
        try:
            if getattr(live,'disconnected',False) or getattr(live,'io_stopped',False):
                report['paused_after_transport_loss']=True
                raise RuntimeError('Device completion unconfirmed; no recovery/retry or further device I/O')
            if modified:
                rw(0x3d7e000,1)
                rw(0x3da0c00,0x20000);rw(0x3dc2000,0)
                rw(0x3dc2618,0,verify=False);rw(0x3dc27f0,0xffffffff,verify=False)
                assert poll(0x3dc27f4,1,0)[0]
                rw(0x3da0800,0);rw(0x3dc2020,0,64);rw(0x3dc2028,0,64)
                rw(0x3dc2058,0xffffffff,verify=False)
                report["native_transport_stopped"]=True
                for a,v in reversed(saved):rw(a,v,verify=a in (0x3da6008,0x3da600c))
                if report.get('prr_marker',{}).get('prepared'):
                    report['prr_marker']['original_registers_restored']=True
                assert live.read(0x3d7e000)==1 and live.read(0x3dc2000)==0
                assert live.read(0x3da0800)==0 and live.read(0x3da0c00)==0x20000
                report['cleanup']={'reset_held':True,'stream_faulted':True,'context_disabled':True,
                    'tlb_synced':True,'stock_tcm_retained':True,'owned_ram_pinned_until_reboot':True}
            # Restore the three bus gate changes made in this terminal turn.
            rw(0x3d97000,live.read(0x3d97000)&~1,verify=False)
            rw(0x171010,live.read(0x171010)|2,verify=False)
            rw(0x171004,live.read(0x171004)|2,verify=False)
            for slot in reversed(maps):live.unmap(slot)
        except Exception as error:
            report['cleanup_error']=str(error)
        (live.capture.root/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        print('RESULT '+str(live.capture.root/'result.json'),flush=True)

if report.get('error') or report.get('cleanup_error') or not report.get('native_transport_stopped'):
    raise SystemExit(1)
