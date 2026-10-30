#!/usr/bin/env python3
"""Apply multicore PSCI SMP binary patches directly to kernel.macho.

Enables all 8 cores on OnePlus Ace 5 / 13R (giulia, Snapdragon 8 Gen 3 SM8650)
by patching:
  1. _reset_vector MPIDR mask (preserves Affinity 1 for secondary cores).
  2. SweetCPU::start (registers secondary CPUs without single-core panic).
  3. SweetCPU::signalCPU (GICv3 SGI1 directed at destination core MPIDR).
  4. SweetCPU::startCPU (PSCI CPU_ON SMC 0xC4000003 trampoline).
"""

import hashlib
import json
from pathlib import Path

PATCHES = [
    # 1. _reset_vector MPIDR Affinity 1 mask (0xfffffe00074a6420)
    # Original: and x0, x15, #0xff   (e0 1d 40 92)
    # Patched:  and x0, x15, #0xffff (e0 3d 40 92)
    (
        0x4A2420,
        bytes.fromhex("e03d4092"),
        "reset_vector MPIDR mask (and x0, x15, #0xffff)",
    ),
    # 2. SweetCPU::start patch A: compute cpu pointer for any core and secondary CPU registration
    # Original: checked target == boot_cpu, reg == boot_cpu->phys_id, num_cpus == 1
    # Patched:  x24 = &cpus[target_cpu_id]; branch to IOCPU::start; secondary_cpu_entry registers service
    (
        0xA8E908,
        bytes.fromhex(
            "880e40f9090f80521820099b0b000014"  # ldr x8, [x20, #0x18]; mov w9, #0x78; madd x24, x0, x9, x8; b 0x2c
            "680240f908f142f9e00313aa01008052"  # ldr x8, [x19]; ldr x8, [x8, #0x5e0]; mov x0, x19; mov w1, #0
            "00013fd62000805268000014"          # blr x8; mov w0, #1; b 0x1a0 (to epilogue return true)
            "1f2003d51f2003d51f2003d5"          # nop; nop; nop
        ),
        "SweetCPU::start multicore topology lookup and secondary registration",
    ),
    # 3. SweetCPU::start patch B: branch secondary CPUs away from boot processor_start
    # Original: adrp x0, ...; add x0, ...; bl IOLog (cosmetic log message)
    # Patched:  ldr w8, [x24]; cbnz w8, secondary_cpu_entry; nop
    (
        0xA8EA18,
        bytes.fromhex(
            "080340b9"  # ldr w8, [x24]
            "e8f7ff35"  # cbnz w8, #0xfffffe0007a92918 (secondary_cpu_entry)
            "1f2003d5"  # nop
        ),
        "SweetCPU::start secondary CPU branch",
    ),
    # 4. SweetCPU::signalCPU: GICv3 ICC_SGI1R_EL1 targeting core MPIDR
    # Original: checked x1 == x0, panicking on foreign IPI
    # Patched:  read target cpu_id from [x1, #0x90], shift to Aff1, target list bit 0, SGI 1, write ICC_SGI1R_EL1
    (
        0xA8EC50,
        bytes.fromhex(
            "289040b9"  # ldr w8, [x1, #0x90]
            "08bd70d3"  # lsl x8, x8, #16
            "080140b2"  # orr x8, x8, #1
            "080168b2"  # orr x8, x8, #0x1000000
            "9f3a03d5"  # dsb ishst
            "a8cb18d5"  # msr icc_sgi1r_el1, x8
            "df3f03d5"  # isb
            "c0035fd6"  # ret
        ),
        "SweetCPU::signalCPU GICv3 cross-core SGI1 IPI",
    ),
    # 5. SweetCPU::startCPU trampoline: PSCI CPU_ON SMC #0 (0xC4000003)
    # Placed in NOP cave at 0xfffffe00074a2004 (file offset 0x49e004)
    (
        0x49E004,
        bytes.fromhex(
            "089040b9"  # ldr w8, [x0, #0x90] (target cpu_id)
            "89ebffd0"  # adrp x9, topology (0xfffffe0007214000)
            "29e142f9"  # ldr x9, [x9, #0x5c0] (topology->cpus)
            "0a0f8052"  # mov w10, #0x78
            "09250a9b"  # madd x9, x8, x10, x9
            "210540b9"  # ldr w1, [x9, #4] (target phys_id / MPIDR)
            "29e7ffd0"  # adrp x9, gPhysBase/gVirtBase (0xfffffe0007188000)
            "2a8140f9"  # ldr x10, [x9, #0x100] (gPhysBase)
            "2b8940f9"  # ldr x11, [x9, #0x110] (gVirtBase)
            "22000090"  # adrp x2, LowResetVectorBase (0xfffffe00074a6000)
            "42000091"  # add x2, x2, #0
            "42000bcb"  # sub x2, x2, x11
            "42000a8b"  # add x2, x2, x10 (x2 = pa(LowResetVectorBase))
            "600080d2"  # mov x0, #3
            "0080b8f2"  # movk x0, #0xc400, lsl #16 (PSCI CPU_ON 0xc4000003)
            "030080d2"  # mov x3, #0
            "9f3f03d5"  # dsb sy
            "df3f03d5"  # isb
            "030000d4"  # smc #0
            "1f0000f1"  # cmp x0, #0
            "60000054"  # b.eq +12 (to success)
            "a0008052"  # mov w0, #5 (KERN_FAILURE)
            "c0035fd6"  # ret
            "00008052"  # mov w0, #0 (KERN_SUCCESS)
            "c0035fd6"  # ret
        ),
        "SweetCPU::startCPU PSCI CPU_ON trampoline",
    ),
    # 6. SweetCPU::startCPU branch to trampoline
    # Original: mov w0, #0x2e; ret (KERN_NOT_SUPPORTED)
    # Patched:  b 0xfffffe00074a2004
    (
        0xA8ECE0,
        bytes.fromhex("c93ce817"),
        "SweetCPU::startCPU branch to PSCI trampoline",
    ),
]


def patch_kernel(kernel_path: Path, json_path: Path):
    with open(kernel_path, "r+b") as f:
        for offset, data, desc in PATCHES:
            f.seek(offset)
            f.write(data)
            print(f"Applied patch: {desc} at offset {offset:#x} ({len(data)} bytes)")
        f.flush()

    digest = hashlib.sha256(kernel_path.read_bytes()).hexdigest()
    if json_path.exists():
        data = json.loads(json_path.read_text())
        data["output_sha256"] = digest
        json_path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"Successfully patched kernel: {kernel_path}")
    print(f"New SHA256: {digest}")


def main():
    root = Path(__file__).resolve().parents[1]
    kernel_path = root / "kernel-artifact" / "profile" / "kernel.macho"
    json_path = root / "kernel-artifact" / "profile" / "kernel.json"
    if not kernel_path.exists():
        raise SystemExit(f"Kernel macho not found: {kernel_path}")
    patch_kernel(kernel_path, json_path)


if __name__ == "__main__":
    main()
