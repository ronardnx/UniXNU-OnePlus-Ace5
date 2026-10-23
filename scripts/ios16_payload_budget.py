"""Shared FAT capacity and prepared Darwin22 loader bounds for local packaging."""
import struct


MIB = 1024 * 1024
SECTOR_BYTES = 512
PAYLOAD_MAX_BYTES = 224 * MIB
KERNEL_MAX_BYTES = 128 * MIB
KERNEL_MAX_SPAN = 254 * MIB
HANDOFF_ALIGNMENT = 16384


def require(condition, message):
    if not condition:
        raise ValueError(message)


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def fat_geometry(image):
    require(512 <= len(image) <= PAYLOAD_MAX_BYTES and len(image) % 512 == 0,
            'FAT payload exceeds the bounded 224 MiB initrd budget or is unaligned')
    require(image[510:512] == b'\x55\xaa' and image[82:90] == b'FAT32   ' and
            image[71:82] == b'GIULIAESP  ', 'Expected the GIULIAESP FAT32 payload')
    sector_bytes, cluster_sectors, reserved, fats, root_entries, small_total = struct.unpack_from('<HBHBHH', image, 11)
    fat16 = struct.unpack_from('<H', image, 22)[0]
    total_sectors, fat_sectors = struct.unpack_from('<II', image, 32)
    require(sector_bytes == SECTOR_BYTES and total_sectors * sector_bytes == len(image),
            'FAT total sectors/sector size differ from the image length')
    require(cluster_sectors in (1, 2, 4, 8, 16, 32, 64, 128) and
            reserved > 0 and fats in (1, 2) and fat_sectors > 0 and
            root_entries == 0 and small_total == 0 and fat16 == 0,
            'Invalid FAT32 allocation geometry')
    overhead = reserved + fats * fat_sectors
    require(overhead < total_sectors, 'FAT tables consume the entire payload')
    clusters = (total_sectors - overhead) // cluster_sectors
    require(65525 <= clusters <= 0x0ffffff5 and fat_sectors * sector_bytes // 4 >= clusters + 2,
            'FAT32 cluster count or FAT entry capacity is invalid')
    return {'sector_bytes': sector_bytes, 'total_sectors': total_sectors,
            'cluster_sectors': cluster_sectors, 'cluster_bytes': sector_bytes * cluster_sectors,
            'reserved_sectors': reserved, 'fat_count': fats, 'fat_sectors': fat_sectors,
            'data_clusters': clusters, 'data_bytes': clusters * cluster_sectors * sector_bytes}


def file_budget(files, payload_bytes, geometry=None):
    require(all(isinstance(size, int) and size >= 0 for size in files.values()),
            'Invalid staged payload file size')
    # Four directory clusters: FAT root, EFI, UniXNU and Payloads. The fixed
    # packaging filenames (including their long-name entries) fit one each.
    minimum = sum(align(size, SECTOR_BYTES) for size in files.values()) + 7 * SECTOR_BYTES
    require(minimum <= payload_bytes,
            'Staged files already exceed the payload capacity before FAT overhead; increase --payload-mib')
    result = {'files': files, 'file_bytes': sum(files.values()),
              'minimum_payload_bytes': minimum, 'directory_clusters': 4}
    if geometry is not None:
        require(geometry['total_sectors'] * geometry['sector_bytes'] == payload_bytes,
                'Requested payload capacity differs from formatted FAT geometry')
        allocated = sum(align(size, geometry['cluster_bytes']) for size in files.values())
        allocated += 4 * geometry['cluster_bytes']
        require(allocated <= geometry['data_bytes'],
                'Staged files and directories do not fit the formatted FAT32 data area; increase --payload-mib')
        result.update(allocated_data_bytes=allocated,
                      available_data_bytes=geometry['data_bytes'],
                      free_data_bytes=geometry['data_bytes'] - allocated)
    return result


def _kernel_span(kernel):
    require(32 <= len(kernel) <= KERNEL_MAX_BYTES,
            'Kernel file exceeds the prepared DarwinBoot 128 MiB read limit or is truncated')
    require(struct.unpack_from('<4I', kernel) == (0xfeedfacf, 0x100000c, 0, 2),
            'DarwinBoot requires a thin ARM64 Mach-O executable')
    count, command_bytes = struct.unpack_from('<II', kernel, 16)
    require(command_bytes <= len(kernel) - 32 and count <= command_bytes // 8,
            'Invalid kernel Mach-O load-command bounds')
    at, end, segments, entry = 32, 32 + command_bytes, [], None
    for _ in range(count):
        require(at + 8 <= end, 'Truncated kernel Mach-O load command')
        command, length = struct.unpack_from('<II', kernel, at)
        require(length >= 8 and length % 8 == 0 and length <= end - at,
                'Invalid kernel Mach-O load-command size')
        if command == 0x19:
            require(length >= 72 and struct.unpack_from('<I', kernel, at + 64)[0] <= (length - 72) // 80,
                    'Invalid kernel segment/section table')
            address, size, offset, file_size = struct.unpack_from('<4Q', kernel, at + 24)
            protection = struct.unpack_from('<I', kernel, at + 60)[0]
            if size:
                require(len(segments) < 64 and size <= 256 * MIB and
                        address + size <= 2**64 - 1 and file_size <= size and
                        offset <= len(kernel) and file_size <= len(kernel) - offset,
                        'Kernel segment exceeds the real DarwinBoot load bounds')
                require(all(address >= stop or address + size <= start
                            for start, stop, _, _ in segments), 'Overlapping loaded kernel segments')
                segments.append((address, address + size, file_size, protection))
        elif command == 5:
            require(entry is None and length == 288 and
                    struct.unpack_from('<II', kernel, at + 8) == (6, 68),
                    'DarwinBoot requires one ARM64 LC_UNIXTHREAD entry')
            entry = struct.unpack_from('<Q', kernel, at + 272)[0]
        at += length
    require(at == end and segments and entry is not None and entry % 4 == 0 and any(
            start <= entry < start + file_size and protection & 4
            for start, _, file_size, protection in segments),
            'Kernel lacks a valid executable DarwinBoot entry')
    base = min(start for start, _, _, _ in segments)
    span = max(stop for _, stop, _, _ in segments) - base
    require(base % 4096 == 0 and base + 256 * MIB <= 2**64 - 1 and span <= KERNEL_MAX_SPAN,
            'Loaded kernel span exceeds the prepared DarwinBoot 254 MiB limit')
    return base, span


def _ramdisk_size(ramdisk, keep_packed):
    require(len(ramdisk) <= 256 * MIB, 'Ramdisk exceeds the DarwinBoot 256 MiB input limit')
    packed = ramdisk[:8] == b'DBRDISK1'
    require(not keep_packed or packed, 'PackedRAMDisk ADT property requires DBRDISK1 input')
    if not packed:
        return len(ramdisk)
    require(len(ramdisk) >= 32 and struct.unpack_from('<II', ramdisk, 8) == (1, 4096),
            'Invalid DBRDISK1 version/page size')
    logical, count = struct.unpack_from('<QQ', ramdisk, 16)
    require(0 < logical <= 1 << 40 and logical % 4096 == 0 and
            count <= 65536 and count <= (len(ramdisk) - 32) // 16,
            'Invalid DBRDISK1 logical size/extent count')
    cursor, previous_end = 32 + count * 16, 0
    for i in range(count):
        offset, length = struct.unpack_from('<QQ', ramdisk, 32 + i * 16)
        require(length > 0 and offset % 4096 == 0 and length % 4096 == 0 and
                offset >= previous_end and offset <= logical and length <= logical - offset and
                length <= len(ramdisk) - cursor, 'Invalid DBRDISK1 extent bounds')
        previous_end, cursor = offset + length, cursor + length
    require(cursor == len(ramdisk), 'DBRDISK1 data length mismatch')
    return len(ramdisk) if keep_packed else logical


def loader_budget(kernel, adt, trust_bytes=0, ramdisk=b''):
    # Existing payload entry points already ship ios16_platform. Keep this
    # import local so the Android packer only needs this helper for FAT checks.
    from ios16_platform import read_nodes
    require(0 < len(adt) <= 16 * MIB and 0 <= trust_bytes <= 16 * MIB,
            'ADT/trust catalogue exceeds the DarwinBoot 16 MiB read limit')
    nodes = read_nodes(adt)
    for path, properties in nodes.items():
        require(path == '/device-tree/chosen' or not any(name in properties for name in
                ('dram-base', 'dram-size', 'darwinboot-memory-size')),
                'DarwinBoot DRAM/window properties must belong to /device-tree/chosen')
    chosen = nodes.get('/device-tree/chosen', {})
    memory_map = nodes.get('/device-tree/chosen/memory-map', {})
    declared = chosen.get('darwinboot-memory-size')
    if declared is None:
        owned, source = 256 * MIB, 'DarwinBoot WINDOW_SIZE fallback; ADT property absent'
    else:
        require(len(declared) == 8, 'Invalid ADT darwinboot-memory-size length')
        owned = struct.unpack('<Q', declared)[0]
        require(256 * MIB <= owned <= 8 * 1024 * MIB and owned % (2 * MIB) == 0,
                'ADT memory window exceeds DarwinBoot bounds or is not 2 MiB aligned')
        source = '/device-tree/chosen/darwinboot-memory-size'
    base, span = _kernel_span(kernel)
    require(base + owned <= 2**64 - 1, 'Kernel base plus owned window overflows')
    require(chosen.get('dram-base') == bytes(8) and chosen.get('dram-size') == bytes(8),
            'Prepared Darwin22 loader requires zero DRAM handoff placeholders')
    ramdisk_keys = [name for name in ('RAMDisk', 'PackedRAMDisk') if name in memory_map]
    require(len(ramdisk_keys) <= 1 and bool(ramdisk_keys) == bool(ramdisk),
            'Ramdisk input and ADT memory-map placeholder disagree')
    if ramdisk_keys:
        require(memory_map[ramdisk_keys[0]] == bytes(16), 'Ramdisk handoff must begin at zero')
    require(('TrustCache' in memory_map) == bool(trust_bytes),
            'Trust catalogue input and ADT memory-map placeholder disagree')
    if trust_bytes:
        require(memory_map['TrustCache'] == bytes(16), 'Trust catalogue handoff must begin at zero')
    ramdisk_size = _ramdisk_size(ramdisk, 'PackedRAMDisk' in memory_map)
    # AllocateWindow preserves Kernel.Base modulo 2 MiB, so absolute 16 KiB
    # alignment must retain that residue even for a 4 KiB-aligned image base.
    args_offset = align(base + span, HANDOFF_ALIGNMENT) - base
    tree_offset = args_offset + HANDOFF_ALIGNMENT
    ramdisk_offset = align(base + tree_offset + len(adt), HANDOFF_ALIGNMENT) - base
    trust_offset = ramdisk_offset + align(ramdisk_size, HANDOFF_ALIGNMENT)
    used = align(base + trust_offset + trust_bytes, HANDOFF_ALIGNMENT) - base
    require(used <= owned - 2 * MIB, 'Kernel/ADT/trust/ramdisk handoff exceeds the actual owned window')
    return {'kernel_file_bytes': len(kernel), 'kernel_file_limit': KERNEL_MAX_BYTES,
            'kernel_virtual_span_bytes': span, 'kernel_virtual_span_limit': KERNEL_MAX_SPAN,
            'alignment_bytes': HANDOFF_ALIGNMENT, 'boot_args_offset': args_offset,
            'adt_offset': tree_offset, 'adt_bytes': len(adt), 'ramdisk_offset': ramdisk_offset,
            'ramdisk_loaded_bytes': ramdisk_size, 'trust_offset': trust_offset,
            'trust_bytes': trust_bytes, 'used_bytes': used, 'reserved_tail_bytes': 2 * MIB,
            'owned_window_bytes': owned, 'owned_window_source': source,
            'remaining_window_bytes': owned - 2 * MIB - used,
            'allocation_verified': False}
