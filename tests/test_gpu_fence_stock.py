"""Adversarial offline ELF fixtures: alignment-looking bytes alone are not evidence."""
from copy import deepcopy
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
LOADER = importlib.util.spec_from_file_location("gpu_fence_stock", ROOT / "scripts/check-gpu-fence-stock.py")
CHECK = importlib.util.module_from_spec(LOADER)
LOADER.loader.exec_module(CHECK)
SPEC = json.loads((ROOT / "hardware/gpu-fence-stock.json").read_text(encoding="utf-8"))["module"]
BOOT_SPEC = json.loads((ROOT / "hardware/gpu-fence-stock.json").read_text(encoding="utf-8"))["boot"]


def elf_fixture():
    """Minimal relocatable ELF retaining the stock instruction/symbol coordinates."""
    spec = deepcopy(SPEC)
    text_offset = int(spec["elf"]["section_file_offset"], 0)
    symbol_value = int(spec["elf"]["symbol_value"], 0)
    text_size = symbol_value + spec["elf"]["symbol_bytes"]
    shstrings = b"\0.text\0.shstrtab\0.strtab\0.symtab\0.modinfo\0"
    strings = b"\0" + spec["elf"]["symbol"].encode() + b"\0"
    modinfo = b"vermagic=" + spec["vermagic"].encode() + b"\0"
    data = bytearray(text_offset + text_size)
    for check in spec["instruction_checks"]:
        struct.pack_into("<I", data, int(check["file_offset"], 0), int(check["word"], 0))
    sections = [(0,) * 10, (1, 1, 6, 0, text_offset, text_size, 0, 0, 4, 0)]
    for name, payload, kind, link, entsize in [
        (b".shstrtab", shstrings, 3, 0, 0),
        (b".strtab", strings, 3, 0, 0),
        (b".symtab", CHECK.SYMBOL.pack(0, 0, 0, 0, 0, 0)
         + CHECK.SYMBOL.pack(1, 0x12, 0, 1, symbol_value, spec["elf"]["symbol_bytes"]), 2, 3, 24),
        (b".modinfo", modinfo, 1, 0, 0),
    ]:
        sections.append((shstrings.index(name), kind, 0, 0, len(data), len(payload), link, 0, 1, entsize))
        data.extend(payload)
    shoff = len(data)
    data.extend(b"".join(CHECK.SECTION_HEADER.pack(*s) for s in sections))
    data[:64] = CHECK.ELF_HEADER.pack(b"\x7fELF\x02\x01\x01" + bytes(9), 1, 183, 1,
                                     0, 0, shoff, 0, 64, 0, 0, 64, len(sections), 2)
    return data, spec, sections, shoff


def pin_fixture(data, spec):
    # Only test-local trust: the command line never accepts an alternate pin.
    spec["bytes"] = len(data)
    spec["sha256"] = hashlib.sha256(data).hexdigest()


def boot_fixture(page_shift="12", duplicate_page_line=False):
    spec = deepcopy(BOOT_SPEC)
    lines = [b""] * 440
    for entry in spec["config_evidence"]:
        lines[entry["line"] - 1] = entry["text"].encode()
    lines[309] = b"CONFIG_ARM64_PAGE_SHIFT=" + page_shift.encode()
    if duplicate_page_line:
        lines.append(lines[433])
    config = b"\n".join(lines) + b"\n"
    compressed = gzip.compress(config, mtime=0)
    start = 512
    end = start + 8 + len(compressed)
    kernel = bytearray(end + 8)
    kernel[56:60] = b"ARM\x64"
    version = spec["version_prefix"].encode()
    kernel[64:64 + len(version)] = version
    kernel[start:end + 8] = b"IKCFG_ST" + compressed + b"IKCFG_ED"
    spec.update(kernel_bytes=len(kernel), kernel_sha256=hashlib.sha256(kernel).hexdigest(),
                ikconfig_start_marker_kernel_offset=hex(start),
                ikconfig_end_marker_kernel_offset=hex(end),
                ikconfig_gzip_bytes=len(compressed), ikconfig_bytes=len(config),
                ikconfig_sha256=hashlib.sha256(config).hexdigest())
    data = bytearray(4096) + kernel
    data[:8] = b"ANDROID!"
    struct.pack_into("<I", data, 8, len(kernel))
    struct.pack_into("<I", data, 40, 4)
    pin_fixture(data, spec)
    return data, spec


class FenceStockEvidenceTests(unittest.TestCase):
    def inspect(self, data, spec):
        pin_fixture(data, spec)
        return CHECK.inspect_module(bytes(data), spec)

    def test_fixture_has_alignment_evidence_but_no_runtime_claim(self):
        data, spec, _, _ = elf_fixture()
        result = self.inspect(data, spec)
        self.assertEqual(result["allocator_page_bytes"], 4096)
        self.assertNotIn("vm_ready", result)
        self.assertNotIn("runtime_provider_ready", result)

    def test_unknown_stock_hash_is_rejected_even_with_valid_layout(self):
        data, spec, _, _ = elf_fixture()
        spec["bytes"] = len(data)
        with self.assertRaisesRegex(CHECK.EvidenceError, "SHA-256"):
            CHECK.inspect_module(bytes(data), spec)

    def test_alignment_bytes_outside_function_are_not_evidence(self):
        data, spec, sections, _ = elf_fixture()
        sym_offset = sections[4][4] + CHECK.SYMBOL.size
        struct.pack_into("<Q", data, sym_offset + 8, 0)
        with self.assertRaisesRegex(CHECK.EvidenceError, "symbol range"):
            self.inspect(data, spec)

    def test_data_symbol_cannot_impersonate_allocator_function(self):
        data, spec, sections, _ = elf_fixture()
        data[sections[4][4] + CHECK.SYMBOL.size + 4] = 0x11
        with self.assertRaisesRegex(CHECK.EvidenceError, "must be a function"):
            self.inspect(data, spec)

    def test_nonexecutable_text_is_rejected(self):
        data, spec, _, shoff = elf_fixture()
        struct.pack_into("<Q", data, shoff + CHECK.SECTION_HEADER.size + 8, 2)
        with self.assertRaisesRegex(CHECK.EvidenceError, "executable"):
            self.inspect(data, spec)

    def test_changed_page_rounding_is_rejected_despite_mask_elsewhere(self):
        data, spec, _, _ = elf_fixture()
        check = spec["instruction_checks"][-1]
        offset = int(check["file_offset"], 0)
        original = data[offset:offset + 4]
        data[offset:offset + 4] = bytes(4)
        data[offset + 8:offset + 12] = original
        with self.assertRaisesRegex(CHECK.EvidenceError, "allocator instruction differs"):
            self.inspect(data, spec)

    def test_instruction_cannot_be_taken_from_adjacent_function(self):
        data, spec, _, _ = elf_fixture()
        spec["instruction_checks"][-1]["text_offset"] = hex(
            int(spec["elf"]["symbol_value"], 0) + spec["elf"]["symbol_bytes"])
        with self.assertRaisesRegex(CHECK.EvidenceError, "outside allocator function"):
            self.inspect(data, spec)

    def test_ambiguous_allocator_symbols_are_rejected(self):
        data, spec, sections, _ = elf_fixture()
        sym_offset = sections[4][4]
        data[sym_offset:sym_offset + 24] = data[sym_offset + 24:sym_offset + 48]
        with self.assertRaisesRegex(CHECK.EvidenceError, "ambiguous allocator symbol"):
            self.inspect(data, spec)

    def test_truncated_section_and_out_of_bounds_text_fail_closed(self):
        data, spec, _, shoff = elf_fixture()
        with self.assertRaisesRegex(CHECK.EvidenceError, "section table.*bounds"):
            self.inspect(data[:-1], spec)
        struct.pack_into("<Q", data, shoff + CHECK.SECTION_HEADER.size + 32, len(data))
        with self.assertRaisesRegex(CHECK.EvidenceError, "text section.*bounds"):
            self.inspect(data, spec)

    def test_wrong_architecture_or_byte_order_is_rejected(self):
        for offset, value, message in [(5, 2, "ELF64 little endian"), (18, 62, "AArch64")]:
            with self.subTest(offset=offset):
                data, spec, _, _ = elf_fixture()
                data[offset] = value
                with self.assertRaisesRegex(CHECK.EvidenceError, message):
                    self.inspect(data, spec)

    def test_vermagic_must_come_from_modinfo(self):
        data, spec, sections, _ = elf_fixture()
        offset = sections[5][4]
        original = data[offset:offset + sections[5][5]]
        data[64:64 + len(original)] = original
        data[offset] = ord("x")
        with self.assertRaisesRegex(CHECK.EvidenceError, "vermagic"):
            self.inspect(data, spec)


class FenceStockBootEvidenceTests(unittest.TestCase):
    def test_embedded_config_proves_page_size_without_runtime_readiness(self):
        data, spec = boot_fixture()
        result = CHECK.inspect_boot(bytes(data), spec)
        self.assertEqual(result["kernel_page_bytes"], 4096)
        self.assertNotIn("vm_ready", result)

    def test_different_page_shift_cannot_be_reported_as_4k(self):
        data, spec = boot_fixture(page_shift="14")
        with self.assertRaisesRegex(CHECK.EvidenceError, "page configuration differs"):
            CHECK.inspect_boot(bytes(data), spec)

    def test_duplicate_page_configuration_is_not_unambiguous_evidence(self):
        data, spec = boot_fixture(duplicate_page_line=True)
        with self.assertRaisesRegex(CHECK.EvidenceError, "page configuration differs"):
            CHECK.inspect_boot(bytes(data), spec)

    def test_matching_outer_image_hash_cannot_hide_substituted_kernel(self):
        data, spec = boot_fixture()
        data[4096 + 128] ^= 1
        pin_fixture(data, spec)
        with self.assertRaisesRegex(CHECK.EvidenceError, "kernel SHA-256 differs"):
            CHECK.inspect_boot(bytes(data), spec)

    def test_ikconfig_magic_at_unrelated_offset_is_not_evidence(self):
        data, spec = boot_fixture()
        data[4096 + 512] = 0
        data[4096 + 200:4096 + 208] = b"IKCFG_ST"
        spec["kernel_sha256"] = hashlib.sha256(data[4096:]).hexdigest()
        pin_fixture(data, spec)
        with self.assertRaisesRegex(CHECK.EvidenceError, "IKCONFIG markers differ"):
            CHECK.inspect_boot(bytes(data), spec)

    def test_invalid_compressed_config_fails_closed(self):
        data, spec = boot_fixture()
        start = 4096 + int(spec["ikconfig_start_marker_kernel_offset"], 0) + 8
        data[start] = 0
        spec["kernel_sha256"] = hashlib.sha256(data[4096:]).hexdigest()
        pin_fixture(data, spec)
        with self.assertRaisesRegex(CHECK.EvidenceError, "invalid IKCONFIG gzip"):
            CHECK.inspect_boot(bytes(data), spec)

    def test_kernel_bounds_and_boot_header_version_are_checked(self):
        data, spec = boot_fixture()
        struct.pack_into("<I", data, 40, 3)
        pin_fixture(data, spec)
        with self.assertRaisesRegex(CHECK.EvidenceError, "header version"):
            CHECK.inspect_boot(bytes(data), spec)
        struct.pack_into("<I", data, 40, 4)
        spec["kernel_bytes"] += 1
        struct.pack_into("<I", data, 8, spec["kernel_bytes"])
        pin_fixture(data, spec)
        with self.assertRaisesRegex(CHECK.EvidenceError, "boot kernel.*bounds"):
            CHECK.inspect_boot(bytes(data), spec)


class OptionalStockIntegrationTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("GPU_FENCE_STOCK_MODULE"), "explicit stock module path not supplied")
    def test_explicit_stock_module(self):
        data = CHECK.read_regular_file(Path(os.environ["GPU_FENCE_STOCK_MODULE"]))
        self.assertEqual(CHECK.inspect_module(data, SPEC)["allocator_page_bytes"], 4096)

    @unittest.skipUnless(os.environ.get("GPU_FENCE_STOCK_BOOT"), "explicit stock boot path not supplied")
    def test_explicit_stock_boot(self):
        data = CHECK.read_regular_file(Path(os.environ["GPU_FENCE_STOCK_BOOT"]))
        self.assertEqual(CHECK.inspect_boot(data, BOOT_SPEC)["kernel_page_bytes"], 4096)


if __name__ == "__main__":
    unittest.main()
