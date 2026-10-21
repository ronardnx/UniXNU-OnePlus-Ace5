// SPDX-License-Identifier: BSD-2-Clause-Patent
// PKG110 runtime DT: GCC 0x100000, GDSCs 0x177004/0x19e000,
// UFS 0x1d84000/0x3000, apps-smmu 0x15000000/0x100000, SID 0x60.
// Register survey only: no controller, queue, SMMU or disk writes.
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/debug.h>

static uintptr_t giuliaMap(uint64_t physical, uint64_t bytes,
                          OSSharedPtr<IOMemoryMap> &mapping)
{
  IODeviceMemory *memory = IODeviceMemory::withRange(physical, bytes);
  if (!memory) return 0;
  mapping = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release();
  return mapping ? static_cast<uintptr_t>(mapping->getVirtualAddress()) : 0;
}

static uint32_t giuliaRead(uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile const uint32_t *>(base + offset);
}

static void giuliaUfsProbe(void)
{
  unsigned requested = 0;
  PE_parse_boot_argn("giulia-ufs-probe", &requested, sizeof(requested));
  if (requested != 1) return;
  OSSharedPtr<IOMemoryMap> clocksMap, memoryMap, hostMap, smmuMap;
  uintptr_t clocks = giuliaMap(0x177000, 0x1000, clocksMap);
  uintptr_t memory = giuliaMap(0x19e000, 0x1000, memoryMap);
  if (!clocks || !memory) panic("GIULIA UFS: GCC mapping failed");
  uint32_t power = giuliaRead(clocks, 4), memPower = giuliaRead(memory, 0);
  uint32_t axi = giuliaRead(clocks, 0x18), ahb = giuliaRead(clocks, 0x24);
  if (!(power & 0x80000000U) || !(axi & 1) || !(ahb & 1))
    panic("GIULIA UFS: power/clocks unavailable\ngdsc=%08x mem=%08x axi=%08x ahb=%08x", power, memPower, axi, ahb);
  uintptr_t host = giuliaMap(0x1d84000, 0x3000, hostMap);
  uintptr_t smmu = giuliaMap(0x15000000, 0x1000, smmuMap);
  if (!host || !smmu) panic("GIULIA UFS: host/SMMU mapping failed");
  unsigned entries = giuliaRead(smmu, 0x20) & 0xff;
  if (entries > 128) entries = 128;
  uint32_t slot = 0xffffffffU, match = 0, route = 0;
  for (unsigned i = 0; i < entries; ++i) {
    uint32_t smr = giuliaRead(smmu, 0x800 + i * 4);
    uint32_t mask = (smr >> 16) & 0x7fff;
    if ((smr & 0x80000000U) && (((smr & 0x7fff) ^ 0x60U) & ~mask) == 0) {
      slot = i; match = smr; route = giuliaRead(smmu, 0xc00 + i * 4); break;
    }
  }
  panic("GIULIA UFS SM8650 register survey\n"
        "gdsc=%08x mem=%08x axi=%08x ahb=%08x\n"
        "cap=%08x ver=%08x hcs=%08x hce=%08x\n"
        "list=%08x:%08x db=%08x rsr=%08x\n"
        "smmu id0=%08x id1=%08x id2=%08x\n"
        "SID60 slot=%08x smr=%08x s2cr=%08x\n"
        "read-only survey; root not mounted",
        power, memPower, axi, ahb,
        giuliaRead(host, 0), giuliaRead(host, 8), giuliaRead(host, 0x30), giuliaRead(host, 0x34),
        giuliaRead(host, 0x54), giuliaRead(host, 0x50), giuliaRead(host, 0x58), giuliaRead(host, 0x60),
        giuliaRead(smmu, 0x20), giuliaRead(smmu, 0x24), giuliaRead(smmu, 0x28), slot, match, route);
}
