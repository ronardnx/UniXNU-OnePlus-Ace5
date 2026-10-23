"""Execute the exact on-device diagnostic bytes with bounded memory guards.

Requires the diagnostic binary from the documented clang invocation and
Unicorn; this does not build a kernel or access the phone.
"""
from pathlib import Path
import hashlib,struct,sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7

ROOT=Path(__file__).resolve().parents[1]
code=Path(sys.argv[1] if len(sys.argv)>1 else ROOT/'.cache/gmu-hfi-mailbox.bin').read_bytes()
assert hashlib.sha256(code).hexdigest()=='4f9058cf0cba12edbfaf2945b23be4e39b51f13efde9c90f231677c0f71ef910'
cases=[(0,0,0,True),(1,0x60010000,6,True),(2,0x60010000,48,True),
       (1,0x60021ffc,1,True),(2,0x60021ffc,1,True),
       (1,0x60000000,1,False),(2,0x60000000,1,False),
       (1,0x60022000,1,False),(2,0x60021ffc,2,False),
       (2,0xfffffffc,48,False),(1,0x60010001,1,False),
       (1,0x60010000,0,False),(2,0x60010000,49,False),
       (3,0x60010000,1,False),(0xffffffff,0x60010000,1,False)]
for op,address,count,accepted in cases:
    cpu=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
    for base,size in ((0,0x4000),(0x10004000,0x4000),(0x60010000,0x12000),(0xe000e000,0x1000)):
        cpu.mem_map(base,size)
    cpu.mem_write(0x3a40,code)
    cpu.mem_write(0xe000ed08,struct.pack('<I',0x1000))
    mailbox=0x10006ec0
    words=[0,op,address,count,0x55aa,0,0]+[0xa7500000+i for i in range(48)]
    cpu.mem_write(mailbox,struct.pack('<55I',*words))
    before=bytes([0x56])*0x12000;cpu.mem_write(0x60010000,before)
    cpu.reg_write(UC_ARM_REG_SP,0x10007c00);cpu.reg_write(UC_ARM_REG_LR,0x3f01)
    for r in (UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7):cpu.reg_write(r,0x12345678)
    external=[]
    def written(cpu,access,where,size,value,user):
        if not 0x10004000<=where<0x10008000:external.append((where,size,value))
    cpu.hook_add(UC_HOOK_MEM_WRITE,written)
    cpu.emu_start(0x3a41,0x3f00,count=2000)
    result=struct.unpack('<7I',cpu.mem_read(mailbox,28))
    assert result[0]==(0xa750600d if accepted else 0xa750bad0),(op,address,count,result)
    assert result[5:7]==(0x55aa,0x1000)
    assert cpu.reg_read(UC_ARM_REG_SP)==0x10007c00
    for r in (UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7):assert cpu.reg_read(r)==0x12345678
    expected=bytearray(before)
    if op==2 and accepted:
        expected[address-0x60010000:address-0x60010000+count*4]=struct.pack('<'+'I'*count,*words[7:7+count])
        assert len(external)==count and all(a==address+i*4 and size==4 for i,(a,size,_) in enumerate(external))
    else:assert not external
    assert cpu.mem_read(0x60010000,0x12000)==expected
print(f'{len(cases)} CM3 mailbox execution tests passed: bounds, wrap refusal, exact writes and register restoration')
