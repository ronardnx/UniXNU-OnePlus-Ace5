"""Exercise the real ADT generator against the stock 20H392 wifid gate."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from ios16_platform import check_platform, read_nodes
from ios16_wifi import WLAN_PATH, check_wifi_platform, matching_job_nodes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--product-adt', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--launch-jobs', type=Path, required=True)
    args = parser.parse_args()
    job = json.loads(args.launch_jobs.read_text())['/System/Library/LaunchDaemons/com.apple.wifid.plist']
    assert job['_LimitLoadToDeviceTree'] == ['wlan', 'marconi-wifi']
    kernel = args.kernel.read_bytes()
    cases = 0
    with tempfile.TemporaryDirectory() as directory:
        base = [sys.executable, str(ROOT/'scripts/make-ios16-adt.py'), '--reference', str(args.reference)]
        gui = ['--gui', '--product-adt', str(args.product_adt), '--trust-catalog', '--wifi-mac', 'a6:01:95:d1:57:36']
        before = Path(directory)/'before.adt'
        after = Path(directory)/'after.adt'
        subprocess.run([*base, *gui, '--out', str(before)], check=True)
        subprocess.run([*base, *gui, '--wifi-bringup', '--out', str(after)], check=True)
        old = read_nodes(before.read_bytes())
        new = read_nodes(after.read_bytes())
        assert matching_job_nodes(job, old) == []
        assert matching_job_nodes(job, new) == [WLAN_PATH]
        assert check_wifi_platform(old) is None
        report = check_wifi_platform(new)
        assert report['scan_supported'] is False
        assert report['pcie_transport_implemented'] is False
        assert check_platform(before.read_bytes(), kernel) == check_platform(after.read_bytes(), kernel)
        cases += 1
        # Boot-critical content and the existing MAC survive the opt-in change.
        assert new.keys() - old.keys() == {WLAN_PATH}
        for path, values in old.items():
            previous = {k: v for k, v in values.items() if k != 'random-seed'}
            current = {k: v for k, v in new[path].items() if k != 'random-seed'}
            assert previous == current, path
        handles = [p['AAPL,phandle'] for p in new.values() if 'AAPL,phandle' in p]
        assert len(handles) == len(set(handles))
        cases += 1

        mutations = [
            lambda n: n[WLAN_PATH].update(reg=(0xb0000000).to_bytes(8, 'little')),
            lambda n: n[WLAN_PATH].update(compatible=b'AppleBCMWLAN\0'),
            lambda n: n[WLAN_PATH].update({'local-mac-address': b'\x02\0\0\0\0\x01'}),
            lambda n: n['/device-tree/chosen'].update({'mac-address-wifi0': bytes(6)}),
            lambda n: n['/device-tree/chosen'].update({'mac-address-wifi0': b'\xff'*6}),
            lambda n: n.update({'/device-tree/marconi-wifi': copy.deepcopy(n[WLAN_PATH])}),
            lambda n: n[WLAN_PATH].pop('giulia,bringup-stage'),
        ]
        for mutate in mutations:
            damaged = copy.deepcopy(new)
            mutate(damaged)
            try:
                check_wifi_platform(damaged)
            except ValueError:
                cases += 1
            else:
                raise AssertionError('Malformed Wi-Fi declaration was accepted')
        for bad in (['--wifi-bringup'],
                    ['--gui', '--product-adt', str(args.product_adt), '--wifi-bringup'],
                    [*gui[:-1], 'ff:ff:ff:ff:ff:ff', '--wifi-bringup']):
            destination = Path(directory)/'rejected.adt'
            result = subprocess.run([*base, *bad, '--out', str(destination)], capture_output=True)
            assert result.returncode != 0 and not destination.exists()
            cases += 1
    print(f'Wi-Fi platform: {cases} real-generator, stock-job gate and invalid-resource cases passed')
    print('Runtime wifid startup and RF scanning are not verified by these host tests')


if __name__ == '__main__':
    main()
