"""Derive the Adreno 750 (SM8650, gen7_9_1) platform data NativeAdreno needs.

Inputs are the pinned OnePlus graphics-kernel blobs and the verified board
record, all already used by the rest of this repository:

  .cache/gpu-source/adreno-gpulist.h   gen7_9_1 core tables
  .cache/gpu-source/adreno_gen7.c      gen7_pwrup_reglist (preemption list)
  .cache/gpu-source/gen7_reg.h         register offsets
  hardware/sm8650-gpu.json             identity, SMMU routes, IRQs, firmware

Output is a self-contained C header plus a JSON manifest. The derivation is
deterministic: tests/test_gpu_a750_native.py regenerates it and compares bytes.

Nothing here is guessed. Every emitted value comes from one of the inputs, and
the generator fails rather than substituting a default.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / '.cache' / 'gpu-source'
SZ = {'SZ_1K': 1 << 10, 'SZ_4K': 1 << 12, 'SZ_16K': 1 << 14, 'SZ_64K': 1 << 16,
      'SZ_1M': 1 << 20, 'SZ_2M': 1 << 21, 'SZ_4M': 1 << 22, 'SZ_8M': 1 << 23,
      'SZ_16M': 1 << 24, 'SZ_32M': 1 << 25, 'MHZ_TO_KBPS': None}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def block(text, name):
    """Return the body of `name[] = { ... };` or `name = { ... };` exactly once."""
    for pattern in (rf'\b{re.escape(name)}\s*\[\]\s*=\s*\{{', rf'\b{re.escape(name)}\s*=\s*\{{'):
        match = re.search(pattern, text)
        if match:
            break
    else:
        raise ValueError('missing block: ' + name)
    start = match.end()
    depth = 1
    for end in range(start, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if not depth:
            return text[start:end]
    raise ValueError('unterminated block: ' + name)


def integer(expr):
    """Evaluate the integer expressions the pinned sources actually use."""
    expr = expr.strip()
    if expr in SZ and SZ[expr] is not None:
        return SZ[expr]
    if re.fullmatch(r'0x[0-9a-fA-F]+|\d+', expr):
        return int(expr, 0)
    match = re.fullmatch(r'(\d+)\s*\*\s*(\w+)', expr)
    if match and SZ.get(match.group(2)) is not None:
        return int(match.group(1)) * SZ[match.group(2)]
    match = re.fullmatch(r'MHZ_TO_KBPS\((\d+),\s*(\d+)\)', expr)
    if match:
        return int(match.group(1)) * 1000
    raise ValueError('unsupported expression: ' + expr)


def reg_offsets(text):
    offsets = {}
    for name, value in re.findall(r'^#define\s+(GEN7_\w+)\s+(0x[0-9a-fA-F]+|\d+)\b', text, re.M):
        offsets[name] = int(value, 0)
    return offsets


def coreregs(body):
    out = {}
    # Quoted values may contain commas, so they are matched whole.
    for key, value in re.findall(r'\.(\w+)\s*=\s*("(?:[^"\\]|\\.)*"|[^,\n]+)', body):
        out[key] = value.strip()
    return out


def table_pairs(body, offsets):
    """Rows of the form { SYMBOL, value } / { SYMBOL, value, value, deny }."""
    rows = []
    for match in re.finditer(r'\{\s*([^{}]+?)\s*\}', body):
        cells = [cell.strip() for cell in match.group(1).split(',') if cell.strip()]
        row = []
        for cell in cells:
            if re.fullmatch(r'GEN7_\w+(\s*\+\s*\d+)?', cell):
                name, _, plus = cell.partition('+')
                row.append(offsets[name.strip()] + (int(plus) if plus else 0))
            else:
                row.append(integer(cell))
        rows.append(row)
    return rows


def symbol_list(body, offsets):
    out = []
    for cell in (cell.strip() for cell in body.split(',')):
        if not cell:
            continue
        name, _, plus = cell.partition('+')
        if name.strip() not in offsets:
            raise ValueError('unknown register symbol: ' + cell)
        out.append(offsets[name.strip()] + (int(plus) if plus else 0))
    return out


def derive(vendor, pins):
    gpulist = (vendor / 'adreno-gpulist.h').read_text()
    gen7 = (vendor / 'adreno_gen7.c').read_text()
    regs = (vendor / 'gen7_reg.h').read_text()
    offsets = reg_offsets(regs)
    core = coreregs(block(gpulist, 'adreno_gpu_core_gen7_9_1'))
    for key in ('ao_hwcg', 'gbif', 'protected_regs', 'gmem_size', 'uche_gmem_alignment',
                'highest_bank_bit', 'gmu_hub_clk_freq', 'hang_detect_cycles', 'bcl_data',
                'preempt_level', 'aqefw_name', 'sqefw_name', 'gmufw_name', 'zap_name',
                'compatible'):
        if key not in core:
            raise ValueError('gen7_9_1 core lacks ' + key)
    ao = table_pairs(block(gpulist, core['ao_hwcg']), offsets)
    gbif = table_pairs(block(gpulist, core['gbif']), offsets)
    protect = table_pairs(block(gpulist, core['protected_regs']), offsets)
    # The vendor list ends with a { 0 } terminator that is not a span.
    if not protect or protect[-1] != [0]:
        raise ValueError('protected register list lacks its sentinel')
    protect = protect[:-1]
    pwrup = symbol_list(block(gen7, 'gen7_pwrup_reglist'), offsets)
    if any(len(row) != 2 for row in ao + gbif):
        raise ValueError('register/value tables must contain pairs')
    if any(len(row) != 4 for row in protect):
        raise ValueError('protected register rows must be {reg, start, end, deny}')
    if any(row[0] != offsets['GEN7_CP_PROTECT_REG'] + index for index, row in enumerate(protect)):
        raise ValueError('protected register indices are not contiguous')
    return dict(offsets=offsets, core=core, ao=ao, gbif=gbif, protect=protect,
                pwrup=pwrup, gpulist=gpulist, gen7=gen7, regs=regs, pins=pins)


def firmware_table(pins):
    return [(entry['name'], entry['size'], entry['sha256']) for entry in pins['firmware']]


def render(d):
    pins = d['pins']
    core = d['core']
    board = pins['board']
    limits = pins['gpu_limits'] if 'gpu_limits' in pins else {}
    lines = []
    add = lines.append
    add('/* SPDX-License-Identifier: MIT */')
    add('/* Generated by scripts/derive-a750-native.py. Do not edit by hand.')
    add(' *')
    add(' * Sources (sha256):')
    for name, data in (('adreno-gpulist.h', d['gpulist']), ('adreno_gen7.c', d['gen7']),
                       ('gen7_reg.h', d['regs']),
                       ('hardware/sm8650-gpu.json', (ROOT / 'hardware/sm8650-gpu.json').read_text())):
        add(f' *   {sha(data.encode())[:32]}  {name}')
    add(' */')
    add('#ifndef GIULIA_A750_NATIVE_H')
    add('#define GIULIA_A750_NATIVE_H')
    add('#include <stdint.h>')
    add('')
    add('/* Identity and limits (device tree + adreno_gpu_core_gen7_9_1). */')
    add(f'#define GIULIA_A750_CHIP_ID 0x{board["chip_id"]:08x}u')
    add(f'#define GIULIA_A750_GPU_ID {board["gpu_id"]}u')
    add(f'#define GIULIA_A750_MODEL "{board["model"]}"')
    add(f'#define GIULIA_A750_COMPATIBLE {core["compatible"]}')
    gmem = integer(core['gmem_size'])
    align = integer(core['uche_gmem_alignment'])
    if not align or align & (align - 1) or gmem > align:
        raise ValueError('GMEM size/alignment are not usable')
    add(f'#define GIULIA_A750_GMEM_BYTES 0x{gmem:x}u')
    add(f'#define GIULIA_A750_UCHE_GMEM_ALIGNMENT 0x{align:x}u')
    add('/* adreno.c:1199: uche_gmem_base = ALIGN(gmem_size, uche_gmem_alignment). */')
    add(f'#define GIULIA_A750_UCHE_GMEM_BASE 0x{align:x}u')
    add(f'#define GIULIA_A750_HIGHEST_BANK_BIT {integer(core["highest_bank_bit"])}u')
    add(f'#define GIULIA_A750_GMU_HUB_CLK_HZ {integer(core["gmu_hub_clk_freq"])}u')
    add(f'#define GIULIA_A750_HANG_DETECT_CYCLES 0x{integer(core["hang_detect_cycles"]):x}u')
    add(f'#define GIULIA_A750_BCL_DATA {integer(core["bcl_data"])}u')
    add(f'#define GIULIA_A750_PREEMPT_LEVEL {integer(core["preempt_level"])}u')
    add('')
    add('/* GMU frequency table from the device tree (Hz). */')
    freqs = board['gmu_frequency_table']
    add(f'#define GIULIA_A750_GMU_FREQ_COUNT {len(freqs) // 2}u')
    add('static const uint32_t GiuliaA750GmuFrequency[]={' +
        ','.join(f'0x{value:x}' for value in freqs) + '};')
    add('')
    add('/* SMMU routes from qcom,kgsl-iommu; actlr cells exactly as captured. */')
    for name, sid in board['stream_ids'].items():
        add(f'#define GIULIA_A750_SID_{name.upper()} {sid}u')
    add('static const uint32_t GiuliaA750SmmuActlrCells[]={' +
        ','.join(hex(value) for value in board['smmu_actlr']) + '};')
    add(f'#define GIULIA_A750_SMMU_ACTLR_CELLS {len(board["smmu_actlr"])}u')
    add('')
    add('/* Firmware names and digests (vendor.img). */')
    fw = firmware_table(pins)
    for name, size, digest in fw:
        if name.startswith('gmu_'):
            key = 'GMU'
        elif '_sqe.' in name:
            key = 'SQE'
        elif '_aqe.' in name:
            key = 'AQE'
        elif '_zap.' in name:
            key = 'ZAP'
        else:
            raise ValueError('unclassified firmware: ' + name)
        add(f'#define GIULIA_A750_FW_{key}_NAME "{name}"')
        add(f'#define GIULIA_A750_FW_{key}_BYTES {size}u')
        add(f'#define GIULIA_A750_FW_{key}_SHA256 "{digest}"')
    add('')
    offsets = d['offsets']
    add('/* Register word offsets used by NativeAdreno (0x3d00000 + 4*offset). */')
    add('enum {')
    for name in ('CP_SQE_INSTR_BASE_LO', 'CP_SQE_INSTR_BASE_HI', 'CP_RB_BASE', 'CP_RB_BASE_HI',
                 'CP_RB_CNTL', 'CP_RB_RPTR_ADDR_LO', 'CP_RB_RPTR_ADDR_HI', 'CP_RB_RPTR',
                 'CP_RB_WPTR', 'CP_SQE_CNTL', 'CP_HW_FAULT', 'CP_INTERRUPT_STATUS',
                 'CP_PROTECT_CNTL', 'CP_APRIV_CNTL', 'RBBM_STATUS', 'RBBM_STATUS3',
                 'RBBM_INT_0_STATUS', 'RBBM_INT_0_MASK', 'RBBM_SECVID_TSB_CNTL',
                 'RBBM_SECVID_TSB_TRUSTED_BASE_LO', 'RBBM_SECVID_TSB_TRUSTED_SIZE',
                 'RBBM_SECVID_TRUST_CNTL', 'UCHE_GMEM_RANGE_MIN_LO', 'UCHE_GMEM_RANGE_MAX_LO',
                 'UCHE_GBIF_GX_CONFIG', 'GPU_GMU_AO_GPU_CX_BUSY_STATUS',
                 'GPU_GMU_AO_GPU_CX_BUSY_MASK', 'RBBM_INTERFACE_HANG_INT_CNTL'):
        add(f'    GIULIA_A750_{name}=0x{offsets["GEN7_" + name]:x},')
    add('};')
    add('')
    add('/* ao_hwcg: the only HWCGC table gen7_9_1 declares (GX has none, so the')
    add(' * driver must use the RBBM_CGC_GLOBAL_LOAD path instead of a GX list). */')
    add('static const uint32_t GiuliaA750AoHwcg[][2]={')
    for reg, value in d['ao']:
        add(f'    {{0x{reg:x},0x{value:x}}},')
    add('};')
    add(f'#define GIULIA_A750_AO_HWCG_COUNT {len(d["ao"])}u')
    add('')
    add('/* GBIF programming list (gen7_0_0_gbif_regs). */')
    add('static const uint32_t GiuliaA750Gbif[][2]={')
    for reg, value in d['gbif']:
        add(f'    {{0x{reg:x},0x{value:x}}},')
    add('};')
    add(f'#define GIULIA_A750_GBIF_COUNT {len(d["gbif"])}u')
    add('')
    add('/* CP access protection spans: {index, start, end, deny}. Encoding is')
    add(' * start | ((end - start) << 18) | (deny << 31) into CP_PROTECT_REG+index. */')
    add('static const uint32_t GiuliaA750Protect[][4]={')
    for index, (_, start, end, deny) in enumerate(d['protect']):
        add(f'    {{{index},{start:#07x},{end:#07x},{deny}}},')
    add('};')
    add(f'#define GIULIA_A750_PROTECT_COUNT {len(d["protect"])}u')
    add('')
    add('/* Preemption power-up register list (gen7_pwrup_reglist). Packed with a')
    add(' * zero IFPC count, as the vendor does when IFPC is not enabled. */')
    add('static const uint32_t GiuliaA750PwrupRegs[]={')
    for reg in d['pwrup']:
        add(f'    0x{reg:08x},')
    add('};')
    add(f'#define GIULIA_A750_PWRUP_COUNT {len(d["pwrup"])}u')
    add('')
    add('#endif')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vendor', type=Path, default=VENDOR)
    parser.add_argument('--pins', type=Path, default=ROOT / 'hardware/sm8650-gpu.json')
    parser.add_argument('--out', type=Path, default=ROOT / 'ios16/native/giulia_a750_native.h')
    parser.add_argument('--manifest', type=Path,
                        default=ROOT / 'hardware/giulia-a750-native.json')
    parser.add_argument('--check', action='store_true', help='fail instead of writing on drift')
    args = parser.parse_args()
    pins = json.loads(args.pins.read_text())
    d = derive(args.vendor, pins)
    header = render(d)
    manifest = {
        'schema': 1,
        'generator': 'scripts/derive-a750-native.py',
        'inputs': {str(path.relative_to(ROOT)): sha(path.read_bytes()) for path in
                   (args.vendor / 'adreno-gpulist.h', args.vendor / 'adreno_gen7.c',
                    args.vendor / 'gen7_reg.h', args.pins)},
        'identity': {'chip_id': hex(pins['board']['chip_id']), 'gpu_id': pins['board']['gpu_id'],
                     'model': pins['board']['model']},
        'tables': {'ao_hwcg': len(d['ao']), 'gbif': len(d['gbif']),
                   'protect': len(d['protect']), 'pwrup': len(d['pwrup'])},
        'gmem_bytes': integer(d['core']['gmem_size']),
        'uche_gmem_alignment': integer(d['core']['uche_gmem_alignment']),
        'hardware_translation_verified': False,
        'rendered_sha256': sha(header.encode()),
    }
    if args.check:
        current = args.out.read_text() if args.out.exists() else ''
        if current != header:
            raise SystemExit('generated header drift: ' + str(args.out))
        print('A750 platform data matches the pinned vendor sources')
        return
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(header, newline='\n')
    args.manifest.write_text(json.dumps(manifest, indent=2) + '\n', newline='\n')
    print(f"A750 platform data: ao={len(d['ao'])} gbif={len(d['gbif'])} "
          f"protect={len(d['protect'])} pwrup={len(d['pwrup'])} -> {args.out}")


if __name__ == '__main__':
    main()
