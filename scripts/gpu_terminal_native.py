"""Native kernel transport for owned buffers and unmodified stock HFI."""
import re,time
class NativeHfi:
    def __init__(self,live,report):
        self.l=live;self.report=report;self.token=0;self.sequence=0x900;self.pending=[]
        self.allocations={}
        self.info=self.dev(0)
        m=re.search(r'abi=1 gmu_root=(0x[0-9a-f]+) gpu_root=(0x[0-9a-f]+)',self.info)
        if not m:raise RuntimeError('Native GPU development ABI is unavailable')
        self.gmu_root=int(m[1],16);self.gpu_root=int(m[2],16)
    def dev(self,op,a=0,b=0,c=0,d=0):
        self.token+=1
        vals=(self.token,op,a,b,c,d)
        line='gpu.dev '+' '.join(hex(x) for x in vals)
        assert len(line)<=63
        m=self.l.command(line,rf'GIULIA GPU DEV: done token={self.token} op={op} code=(\d+)',timeout=8)
        if int(m[1]):raise RuntimeError('Native GPU command failed: '+line+' '+m[0])
        return m.string
    def allocate(self,bytes,align,flags,kind):
        text=self.dev(1,bytes,align,flags,kind)
        m=re.search(r'allocation=(\d+) offset=(0x[0-9a-f]+|0) bytes=(0x[0-9a-f]+) flags=(0x[0-9a-f]+) kind=(\d+) gpu=(0x[0-9a-f]+) gmu=(0x[0-9a-f]+)',text)
        if not m:raise RuntimeError('Missing allocation record')
        names=('handle','offset','bytes','flags','kind','gpu','gmu')
        return dict(zip(names,(int(x,0) for x in m.groups())))
    def read(self,offset,count=1):
        text=self.dev(2,offset,count)
        words=[int(v,0) for _,v in re.findall(r'word=(\d+) value=(0x[0-9a-f]+|0)',text)]
        assert len(words)==count
        return words
    def write(self,offset,words):
        for i,v in enumerate(words):self.dev(3,offset+4*i,v)
    def packet(self,msg_id,payload,msg_type=0):
        self.sequence+=1;n=len(payload)+1
        assert n<=255 and self.sequence<4096
        header=(self.sequence<<20)|(msg_type<<16)|(n<<8)|msg_id
        self.dev(10)
        for i,v in enumerate([header,*payload]):self.dev(5,i,v)
        self.dev(6,n)
        self.l.write(0x3d7e650,1,verify=False)
        return header
    def poll(self):
        text=self.l.command('gpu.hfi.poll',r'GIULIA GPU HFI: poll queue=2 code=1',timeout=8).string
        parts=re.split(r'GIULIA GPU HFI: received queue=(\d+) words=(\d+) header=[^\r\n]+',text)
        for i in range(1,len(parts),3):
            q,n=int(parts[i]),int(parts[i+1])
            words=[int(v,0) for _,v in re.findall(r'GIULIA GPU HFI: word=(\d+) value=(0x[0-9a-f]+|0)',parts[i+2])]
            assert len(words)==n
            self.report.setdefault('hfi_packets',[]).append({'queue':q,'words':[hex(v) for v in words]})
            if q==1:self.pending.append(words)
        self.l.write(0x3d7e644,1,verify=False)
    def request(self,words):
        msg=words[0]&255
        if msg==20 and len(words)==11 and words[1]==1:
            desc=words[2:];flags,kind,handle,bytes,align=desc[2],desc[3],desc[5],desc[7],desc[8]
            if kind in (11,12,13,19,20):raise RuntimeError('Firmware requires a specific MMIO mapping, not a RAM substitute')
            if handle in self.allocations:raise RuntimeError('Duplicate allocation request needs investigation')
            va_align=align&255;size_align=(align>>8)&255
            if va_align>22 or size_align>22 or align>>16:raise RuntimeError('Unsupported alignment')
            alignment=max(4096,1<<va_align);rounded=(bytes+(1<<size_align)-1)&~((1<<size_align)-1)
            a=self.allocate(rounded,alignment,flags,kind)
            self.dev(4,a['offset'],a['bytes'])
            assert self.read(a['offset'])==[0]
            desc[0]=a['gpu'];desc[1]=0;desc[4]=handle;desc[6]=a['gmu'];desc[7]=a['bytes']
            self.allocations[handle]=a
            self.report.setdefault('memory_allocations',[]).append(dict(a,gmu_handle=handle))
            self.packet(20,[words[0],*desc],1)
            print('Allocated kind='+str(kind)+' bytes='+str(a['bytes']),flush=True)
            return
        if msg==110 and len(words)==3:
            self.packet(110,[words[0],words[1],words[2],0,0],1)
            self.report['optional_counter_declined']=True;return
        raise RuntimeError('Unhandled firmware request '+str([hex(x) for x in words]))
    def send(self,msg_id,payload,seconds=10):
        header=self.packet(msg_id,payload);deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if self.l.read(0x3d7e648)&0x8000:raise RuntimeError('CM3 fault')
            if not self.pending:self.poll()
            if not self.pending:continue
            words=self.pending.pop(0)
            if len(words)>=3 and (words[0]>>16)&15==1 and words[1]==header:
                reply={'id':msg_id,'value':words[2],'status':words[3] if len(words)>3 else None,'header':hex(header)}
                self.report.setdefault('transactions',[]).append(reply)
                return reply
            self.request(words)
            deadline=time.monotonic()+seconds
            if len(self.allocations)>64:raise RuntimeError('Bounded allocation limit exceeded')
        raise RuntimeError('HFI response timeout')
