"""Generate Ace 5 Darwin 22 ADT; RAM is allocated by UEFI, not hardcoded."""
import argparse
import runpy
import struct
import sys
from pathlib import Path

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--reference',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--memory',type=lambda x:int(x,0),default=0x80000000)
    p.add_argument('--gui',action='store_true')
    p.add_argument('--product-adt',type=Path,help='Local unwrapped D22AP ADT from the verified IPSW')
    a=p.parse_args()
    if a.gui and not a.product_adt:p.error('GUI requires the verified local D22AP product ADT')
    if not 0x10000000<=a.memory<=0x200000000 or a.memory&0x1fffff:
        p.error('Memory request must match the DarwinBoot window bounds')
    sys.path.insert(0,str(a.reference/'tools'))
    import adt
    reference=runpy.run_path(str(a.reference/'tools/mkvermeeradt.py'))
    tree=reference['build'](ios=True,gui=a.gui,product_adt=a.product_adt)
    def props(node, values):
        node.props=[(k,f,v) for k,f,v in node.props if k not in values]
        node.props += [(k,0,v) for k,v in values.items() if v is not None]
    # The existing kernel registers the shared DT platform expert under this
    # legacy match key. Keep it last for old kernels; native Ace 5 builds match
    # oneplus,giulia. This alias does not supply hardware addresses or topology.
    props(tree,{'compatible':b'oneplus,giulia\0qcom,sm8650\0AppleARM\0xiaomi,sweet\0',
                'model':b'OnePlus Ace 5\0','target-type':b'giulia\0','manufacturer':b'OnePlus\0'})
    if a.gui:
        # UIKit product compatibility is distinct from the physical board.
        props(tree,{'compatible':b'D22AP\0iPhone10,3\0oneplus,giulia\0qcom,sm8650\0AppleARM\0xiaomi,sweet\0',
                    'model':b'iPhone10,3\0','target-type':b'D22\0','physical-model':b'OnePlus Ace 5\0'})
    chosen=next(n for n in tree.children if n.name=='chosen')
    # DarwinBoot fills these from its actual allocation before handing off.
    # Darwin 22 arm_init requires both, in addition to the boot_args fields.
    props(chosen,{'dram-base':bytes(8),'dram-size':bytes(8),
                  'darwinboot-memory-size':struct.pack('<Q',a.memory),
                  'firmware-version':b'giulia-darwin22-uefi\0'})
    if a.gui:
        props(chosen,{'unique-chip-id':struct.pack('<Q',0x4749554C49410001)})
    cpus=next(n for n in tree.children if n.name=='cpus')
    props(cpus.children[0],{'compatible':b'qcom,kryo\0ARM,v8\0'})
    fb=next(n for n in tree.children if n.name=='sweet-fbcon')
    props(fb,{'reg':struct.pack('<QQ',0xd5100000,5056*2780),
              'width':struct.pack('<I',1264),'height':struct.pack('<I',2780),'stride':struct.pack('<I',5056)})
    blob=adt.serialize(tree)
    check,used=adt.parse(blob)
    assert used==len(blob) and check.name=='device-tree'
    a.out.parent.mkdir(parents=True,exist_ok=True)
    a.out.write_bytes(blob)
    print(f'Ace 5 ADT: {len(blob)} bytes, one CPU; UEFI must allocate {a.memory:#x} contiguous bytes')

if __name__=='__main__': main()
