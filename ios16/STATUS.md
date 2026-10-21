# Ace 5 ios-sweet route

Requested base: https://github.com/dipa-studio/ios-sweet

The new firmware is iPhone X d22ap iOS 16.7.16 (20H392). This replaces the
previous iPadOS 17.7.11/APFS system-image route. It does not make a Redmi
Note 10 Pro or K70 boot image suitable for the Ace 5.

`ios16-root.yml` builds the base journaled HFSX filesystem from the pinned
Apple OS, SystemOS and AppOS images. It preserves numeric owners and hard
links, installs the upstream base cryptex/logd/keybag layout, and verifies
the filesystem before export. Device-specific GUI tweaks are not included.
Firmware caching uses the IPSW digest, independently of kernel revisions.
No custom workflow time limit is set.

Still required before claiming this image boots on Ace 5:

- Adapt the XNU 8796 profile and its 16 KiB page configuration.
- Match the loader boot-args ABI: the reference uses a 608-byte command
  line and 736-byte boot_args; the old DarwinBoot uses 1024/1152 bytes.
- Validate owned RAM against the actual Ace 5 FDT, not K70 memory ranges.
- Adapt UFS/SMMU to SM8650, the actual userdata extent, and this image's
  HFS volume identifier. UFS register reads worked; disk reads are unproven.
- Adapt the reference native USB CDC transport and initialize it early
  enough for bring-up diagnostics. No working post-boot COM port yet.

Rollback source: branch `checkpoint/unixnu-ufs-read` at `2d86154`.
Unfinished stage display work is preserved in git stash
`9757d0145bef52e47f192142299a7d5ab894b05e`.
The previously flashed userdata image has not been replaced by this switch.

## Current implementation

- `scripts/ios16-loader.py` patches a separate UEFI build for the 608-byte
  command line, 736-byte boot_args and 16 KiB alignment. It also shrinks the
  local copy buffer, so CopyMem cannot overwrite BootFlags/MemorySizeActual.
- `scripts/prepare-ios16.py` generates the non-GUI iOS profile from the
  pinned Vermeer integration. UFS uses the captured Ace 5 GPT bounds and
  first-two-block CRC. Its SID60 route is moved from preserved CB2 to CB70
  only after queue quiescence and checks that CB70 is disabled/unreferenced.
  Restoration switches SID60 back before restoring firmware descriptors.
- DMA buffers must be contiguous allocator-owned pages inside XNU's actual
  gPhysBase/gPhysSize, with controller and SMMU address-width checks.
- Native CDC ACM uses ACE5DBG001 and starts at UFS root selection before
  disk probing/mounting. It still depends on an initialized scheduler and
  firmware USB PHY/clocks plus an identity SID40 route; earlier arm_init
  failures and panic-time polling are not covered yet.
- The ADT requests a 2 GiB UEFI allocation. Allocation failure is reported;
  no K70 physical RAM address is assumed. Framebuffer is d5100000,
  1264x2780 with 5056-byte stride; GIC and timer match captured Ace 5 DT.

Local checks: boot_args offsets/size compiled; generated patches parse;
ASan/UBSan tests pass for SMMU isolation, DMA/window and write bounds.
The GPT parser also passes against the actual saved LUN0 header/table
(32 entries, 4096-byte table), and rejects a corrupted table.
Kernel/UEFI builds and hardware tests remain separate gates; no new image
has been temporarily booted and no working USB COM port is claimed.
