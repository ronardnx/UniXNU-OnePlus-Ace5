"""The payload must contain both the common runtime and the CDC bridge."""
MARKERS = (b'NLH status abi=2 nonce=', b'NLH done token=',
           b'NLH disabled unixnu_hotload=1 development-arm64-required',
           b'GIULIA TERM: hotload=NLH2', b'NLH module slot=',
           b'NLH memory abi=2 executable=1',
           b'NLH core abi=1 domains=sched,vm,vfs resident_objects=1')


def check_hotload_kernel(kernel):
    if any(marker not in kernel for marker in MARKERS):
        raise ValueError('Hotload requires --hotload-source and a supported development ARM64 kernel')
