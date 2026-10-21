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
