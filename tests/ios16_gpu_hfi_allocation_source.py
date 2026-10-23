"""Verify explicitly supplied pinned gen7 source bytes, without network/device I/O.

The manifest records canonical raw hashes. A local source cache may have only
CRLF-to-LF conversion; that mode is identified separately and is never reported
as a match of the canonical raw SHA-256 or Git blob.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def verify(source: Path) -> list[tuple[str, str]]:
    pins = json.loads((ROOT / 'hardware/gpu-hfi-allocation-source.json').read_text(encoding='utf-8'))
    if pins['commit'] != '453d8875f815bf9dfad9035982861226a4823a6c':
        raise ValueError('Unexpected gen7 source revision')
    observed = []
    for entry in pins['files']:
        path = source / entry['file']
        if not path.is_file():
            raise ValueError('Missing explicitly supplied source: ' + str(path))
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest == entry['sha256']:
            blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            if len(data) != entry['bytes'] or blob != entry['git_blob']:
                raise ValueError('Raw source structure differs: ' + entry['file'])
            mode = 'canonical-raw'
        elif hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest() == entry['normalized_lf_sha256']:
            mode = 'newline-normalized-content-only'
        else:
            raise ValueError('Source content differs from pinned gen7 ABI: ' + entry['file'])
        observed.append((entry['file'], mode))
    return observed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True,
                        help='Directory containing the four explicitly supplied source files')
    args = parser.parse_args()
    for name, mode in verify(args.source):
        print(name + ': ' + mode)


if __name__ == '__main__':
    main()
