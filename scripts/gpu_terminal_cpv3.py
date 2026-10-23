"""Polling CP-only bringup experiment; no raster or Metal readiness claim."""
import re,struct
from gpu_terminal_live import ROOT

def run(live,transport,report):
    raise RuntimeError("Disabled: corrected banks did not prevent NOP 900E; no replay")
    assert report.get("context_banks")=={"gmu":2,"gpu":0,"gpu_reserved":[0,1]}
    source=(ROOT/'.cache/gpu-source/gen7_reg.h').read_text()
    regs={name:int(value,0) for name,value in re.findall(r'^#define\s+(GEN7_\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\b',source,re.M)}
    def addr(name):return 0x3d00000+4*regs[name]
    def wr(name,value):live.write(addr(name),value,verify=False)
    def rd(name):return live.read(addr(name))
    def pkt(op,n):return 0x70000000|n|((1^(n.bit_count()&1))<<15)|(op<<16)|((1^(op.bit_count()&1))<<23)
    from gpu_terminal_gen7_config import Gen7Config
    record=Gen7Config(live,transport,report).configure()
    for asset,name in ((1,'gen70900_sqe.fw'),(2,'gen70900_aqe.fw')):
        data=(ROOT/'out/ios16/gpu-stock-firmware'/name).read_bytes()
        a=transport.allocate(len(data)+4096,4096,0x33f,0)
        transport.dev(7,asset,a['offset']+4092)
        assert transport.read(a['offset']+4096,4)==list(struct.unpack_from('<4I',data,4))
        report[name]={'gpu':hex(a['gpu']+4096),'bytes':len(data)-4,'asset_verified':True}
        if asset==1:
            wr('GEN7_CP_SQE_INSTR_BASE_LO',a['gpu']+4096);wr('GEN7_CP_SQE_INSTR_BASE_HI',0)
        else:
            wr('GEN7_CP_AQE_INSTR_BASE_LO_0',a['gpu']+4096);wr('GEN7_CP_AQE_INSTR_BASE_HI_0',0)
    commands=[pkt(0x17,1),1<<27,pkt(0x48,7),0x94b,3,0x20000000,2,record['gpu'],0,1<<31]
    try:
        reply=transport.send(131,[pkt(0x10,1),0],seconds=2)
        report['cp_nop_reply']=reply
        assert reply['value']==0 and reply['status'] in (None,0)
        report['cp_nop_ack']=True
        return  # NOP-only audit of the corrected bank assignment.
        reply=transport.send(131,commands,seconds=2)
        report['cp_init_reply']=reply
        assert reply['value']==0 and reply['status'] in (None,0)
        assert rd("GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS")&(1<<23)==0
        report['cp_init_accepted']=True
        report['secvid_trust_control']=rd('GEN7_RBBM_SECVID_TRUST_CNTL')
        if report['secvid_trust_control']!=0:
            raise RuntimeError('CP initialized but secure-mode exit requires the stock ZAP authentication path')
        fence=transport.allocate(4096,4096,0x33f,2)
        transport.write(fence['offset'],[0])
        reply=transport.send(131,[pkt(0x3d,3),fence['gpu'],0,0xa750cafe],seconds=2)
        report['cp_mem_write_reply']=reply
        report['cp_mem_write_value']=transport.read(fence['offset'])[0]
        assert reply['value']==0 and report['cp_mem_write_value']==0xa750cafe
        report['gpu_memory_write_verified']=True
    finally:
      try:
        report['cp_diagnostics']={name:hex(rd(name)) for name in
            ('GEN7_RBBM_STATUS','GEN7_RBBM_STATUS3','GEN7_CP_SQE_CNTL','GEN7_CP_HW_FAULT','GEN7_RBBM_INT_0_STATUS')}
        report['gpu_translation_fault']=hex(live.read(0x3dc0058))
        report['gpu_translation_fault_address']=hex(live.read(0x3dc0060,64))
      except Exception as error:report['cp_diagnostic_error']=str(error)
