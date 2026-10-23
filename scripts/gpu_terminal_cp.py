"""Polling CP-only bringup experiment; no raster or Metal readiness claim."""
import re,struct
from gpu_terminal_live import ROOT

def run(live,transport,report):
    raise RuntimeError("Disabled after CP_INIT caused 900E; retained for offline investigation")
    source=(ROOT/'.cache/gpu-source/gen7_reg.h').read_text()
    regs={name:int(value,0) for name,value in re.findall(r'^#define\s+(GEN7_\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\b',source,re.M)}
    def addr(name):return 0x3d00000+4*regs[name]
    def wr(name,value):live.write(addr(name),value,verify=False)
    def rd(name):return live.read(addr(name))
    def pkt(op,n):return 0x70000000|n|((1^(n.bit_count()&1))<<15)|(op<<16)|((1^(op.bit_count()&1))<<23)
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
    # CP traffic and instruction fetch privileges match the vendor gen7_start.
    for name,val in (('GEN7_CP_APRIV_CNTL',0x6f),('GEN7_CP_BV_APRIV_CNTL',0xf),
            ('GEN7_CP_LPAC_APRIV_CNTL',0xf),('GEN7_CP_AQE_APRIV_CNTL',1)):
        wr(name,val)
    for i in range(4):wr('GEN7_GBIF_QSB_SIDE'+str(i),0x71620)
    wr('GEN7_RBBM_GBIF_CLIENT_QOS_CNTL',0x2120212)
    wr('GEN7_GBIF_CX_CONFIG',(rd('GEN7_GBIF_CX_CONFIG')&~0xe0000000)|0x20000000)
    wr('GEN7_UCHE_GBIF_GX_CONFIG',0x10240e0)
    wr('GEN7_GPU_GMU_AO_GPU_CX_BUSY_MASK',0xff000000)
    # Keep GX hardware clocks explicitly on during this bounded CP-only trial.
    wr('GEN7_RBBM_CLOCK_CNTL_GLOBAL',1)
    wr('GEN7_RBBM_CGC_GLOBAL_LOAD_CMD',0)
    record=transport.allocate(4096,4096,0x33f,0)
    transport.dev(4,record['offset'],record['bytes'])
    commands=[pkt(0x17,1),1<<27,pkt(0x48,7),0x94b,3,0x20000000,2,record['gpu'],0,1<<31]
    try:
        reply=transport.send(131,commands,seconds=2)
        report['cp_init_reply']=reply
        assert reply['value']==0 and reply['status'] in (None,0)
        assert rd("GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS")&(1<<23)==0
        fence=transport.allocate(4096,4096,0x33f,2)
        transport.write(fence['offset'],[0])
        reply=transport.send(131,[pkt(0x3d,3),fence['gpu'],0,0xa750cafe],seconds=2)
        report['cp_mem_write_reply']=reply
        report['cp_mem_write_value']=transport.read(fence['offset'])[0]
        assert reply['value']==0 and report['cp_mem_write_value']==0xa750cafe
        report['gpu_memory_write_verified']=True
    finally:
        report['cp_diagnostics']={name:hex(rd(name)) for name in
            ('GEN7_RBBM_STATUS','GEN7_RBBM_STATUS3','GEN7_CP_SQE_CNTL','GEN7_CP_HW_FAULT','GEN7_RBBM_INT_0_STATUS')}
        report['gpu_translation_fault']=hex(live.read(0x3dc1058))
        report['gpu_translation_fault_address']=hex(live.read(0x3dc1060,64))
