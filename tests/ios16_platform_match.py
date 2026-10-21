"""Check real kernel personalities against the generated Ace 5 ADT."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from ios16_platform import check_platform, read_nodes


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--kernel', type=Path, required=True)
    a = p.parse_args()
    sys.path.insert(0, str(a.reference.resolve()/'tools'))
    import adt
    with tempfile.TemporaryDirectory() as td:
        path=Path(td)/'giulia.adt'
        subprocess.run([sys.executable,str(ROOT/'scripts/make-ios16-adt.py'),
                        '--reference',str(a.reference),'--out',str(path)],check=True)
        blob=path.read_bytes()
    kernel=a.kernel.read_bytes()
    result=check_platform(blob,kernel)
    assert result['platform_match'] in ('xiaomi,sweet','oneplus,giulia')
    assert result['cpu_match']=='qcom,kryo'
    nodes=read_nodes(blob)
    chosen=nodes['/device-tree/chosen']
    assert chosen['dram-base']==chosen['dram-size']==bytes(8)
    assert nodes['/device-tree']['compatible'].split(b'\0')[:3]==[
        b'oneplus,giulia',b'qcom,sm8650',b'AppleARM']
    cases=1
    def rejected(data, image, message):
        nonlocal cases
        try:check_platform(data,image)
        except (ValueError, KeyError) as error:
            assert message in str(error), str(error)
        else:raise AssertionError('Invalid platform binding accepted')
        cases+=1
    def changed(path,key,value):
        tree,_=adt.parse(blob)
        current=tree
        for name in path:current=next(n for n in current.children if n.name==name)
        current.props=[(k,f,value if k==key else v) for k,f,v in current.props]
        return adt.serialize(tree)
    # Regression: the old ADT had the correct device identity, but lacked the
    # built-in platform driver's compatible name, causing IOPlatformExpert panic.
    old=changed([],'compatible',b'oneplus,giulia\0qcom,sm8650\0AppleARM\0')
    legacy=kernel.replace(b"'IONameMatch'     = 'oneplus,giulia'",b"'IONameMatch'     = 'xiaomi,sweet'")
    rejected(old,legacy,'ADT does not match built-in platform driver')
    rejected(changed(['cpus','cpu0'],'compatible',b'ARM,v8\0'),kernel,'CPU driver')
    rejected(changed([],'model',b'Redmi K70\0'),kernel,'actual Ace 5')
    rejected(changed([],'compatible',b'xiaomi,sweet\0oneplus,giulia\0qcom,sm8650\0'),kernel,'actual Ace 5')
    rejected(blob,kernel.replace(b'IOPlatformExpertDevice;',b'IOPlatformExpertMissing;'),'found 0')
    rejected(blob,kernel+kernel,'found 2')
    rejected(blob[:-1],kernel,'ADT')
    # A fixture of the native personality planned for the next kernel build:
    # the legacy alias is no longer necessary for the match.
    native=legacy.replace(b"'IONameMatch'     = 'xiaomi,sweet'",b"'IONameMatch'     = 'oneplus,giulia'")
    assert check_platform(old,native)['legacy_platform_alias'] is False
    cases+=1
    print(f'Platform binding: {cases} real-kernel and malformed/mismatch regression cases passed')


if __name__=='__main__':
    main()
