"""Offline oracle for core IO80211 request paths in the cached 20H392 image.

Reads the original dyld cache bytes; never builds a kernel, contacts a device,
or downloads firmware. Optional, like the ath12k source-cache oracles. Run with
Python 3 and an existing cache under out/ios16/dyld-symbols, or --cache-dir.
"""
from pathlib import Path
import argparse
import hashlib
import struct

ROOT = Path(__file__).resolve().parents[1]


class Cache:
    def __init__(self, folder):
        self.maps = []
        for path in folder.glob("dyld_shared_cache_arm64*"):
            if not path.is_file():
                continue
            with path.open("rb") as stream:
                header = stream.read(256)
                if len(header) < 24 or not header.startswith(b"dyld_v1"):
                    continue
                offset, count = struct.unpack_from("<II", header, 16)
                if not 0 < count <= 64 or offset + 32 * count > path.stat().st_size:
                    continue
                stream.seek(offset)
                for _ in range(count):
                    address, size, file_offset, _, _ = struct.unpack("<QQQII", stream.read(32))
                    self.maps.append((address, address + size, file_offset, path))
        if not self.maps:
            raise SystemExit("Missing local dyld cache; this oracle does not download one")

    def read(self, address, size):
        assert 0 <= size <= 65536
        for lower, upper, offset, path in self.maps:
            if lower <= address and size <= upper - address:
                with path.open("rb") as stream:
                    stream.seek(offset + address - lower)
                    result = stream.read(size)
                assert len(result) == size
                return result
        raise AssertionError(f"Unmapped target address {address:#x}")

    def expect(self, address, expected, label):
        assert self.read(address, len(bytes.fromhex(expected))) == bytes.fromhex(expected), label

    def jump(self, table, index, base):
        return base + 4 * struct.unpack("<H", self.read(table + 2 * index, 2))[0]


class StockKernel(Cache):
    def __init__(self, path):
        data = path.read_bytes()
        # Original kernelcache.release.iphone10b payload from the pinned
        # targets/iphone16.json IM4P. This is extraction, never a kernel build.
        assert hashlib.sha256(data).hexdigest() == "1c1e2d36a0c0daef07e4c6d2ac544eed08ab8e17a045ce8f95195a099bdadcc4"
        assert data[:4] == b"\xca\xfe\xba\xbe" and struct.unpack_from(">I", data, 4)[0] == 1
        base = struct.unpack_from(">IIIII", data, 8)[2]
        assert data[base:base + 4] == b"\xcf\xfa\xed\xfe"
        self.maps = []
        offset = base + 32
        for _ in range(struct.unpack_from("<I", data, base + 16)[0]):
            command, size = struct.unpack_from("<II", data, offset)
            assert size >= 8 and size <= len(data) - offset
            if command == 0x19:
                _, address, _, file_offset, length = struct.unpack_from("<16sQQQQ", data, offset + 8)
                assert base + file_offset + length <= len(data)
                self.maps.append((address, address + length, base + file_offset, path))
            offset += size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, default=ROOT / "out/ios16/dyld-symbols")
    parser.add_argument("--stock-kernel", type=Path, help="Optional locally extracted original 20H392 kernel payload")
    args = parser.parse_args()
    cache = Cache(args.cache_dir)
    # GET103 requests a complete BSS, with the same IE buffer as scan results;
    # its consumer is __addScanResultToList, not a scalar or a user pointer.
    assert cache.jump(0x1D3EF565E, 103 + 1, 0x1D3EC4038) == 0x1D3EC6328
    cache.expect(0x1D3EC632C, "96918052", "GET103 length = 0x48c")
    cache.expect(0x1D3EC6340, "08808052e8b70179561b00b9", "GET103 IE capacity = 0x400")
    cache.expect(0x1D3EC6350, "a14302d1e00314aa50120094", "GET103 calls GET wrapper")
    cache.expect(0x1D3EC638C, "ebd9ff97", "GET103 calls __addScanResultToList")
    cache.expect(0x1D3ED5E2C, "e30301aae10c8052020080d24c91ff17", "CopyCurrentNetwork selects 103")
    # Plain Disassociate has no payload; Ext supplies reason at payload +4.
    # Ext deliberately initializes the otherwise unused first word to 0xaa.
    assert cache.jump(0x1D3EF589C, 22 - 1, 0x1D3ECDAC8) == 0x1D3ECE8E0
    cache.expect(0x1D3EC1768, "c102805202008052030080d2040080529a300014", "scalar Disassociate")
    cache.expect(0x1D3ECE8F0, "40e5050fe03300fd", "Ext unused first word is not version 1")
    cache.expect(0x1D3ECE90C, "f5830191a2027eb2610080521a8d689408018052884b20b9952b10f96efcff17",
                 "Ext reason +4, eight-byte payload, SET wrapper")
    # Bind manually emits an empty-payload DRIVER_VERSION query, while the
    # association event polls result 21 through the scalar RawGet path.
    cache.expect(0x1D3ED4F28, "68058052e86b3db9", "Bind selects DRIVER_VERSION 43")
    cache.expect(0x1D3ED4F5C, "08208052e80700f9", "Bind output = 256")
    cache.expect(0x1D3EDBB54, "a8028052e86300b9e1430191e00314aae383ff97", "association event polls 21")
    # Bind writes the 13 capability bytes to handle +0x50. Both explicit SAE
    # auth override and automatic AKM selection test handle +0x59 bit 3:
    # capabilities[9] & 8, capability index 75. Missing it rejects the request
    # before any SET20; the success path selects auth_upper = 0x1000.
    cache.expect(0x1D3ED5034, "f80314aa1f0f05f8", "Bind capability destination +0x50")
    cache.expect(0x1D3ED5050, "88018052f81b00f9a9018052e82300b9e92b00b9e1430091e00314aaa19eff97",
                 "Bind GET12 copies exactly 13 bytes into the handle")
    cache.expect(0x1D3EC017C, "e83f40f908654139c83e1837", "explicit SAE gate: byte 9, bit 3")
    cache.expect(0x1D3EC0194, "c10100d021781091eb010014", "SAE rejection selects unsupported message")
    message = b"%s: AUTH override set to WPA3 SAE but SAE is not supported\n\0"
    assert cache.read(0x1D3EFA41E, len(message)) == message
    cache.expect(0x1D3EC0948, "60008052dec6689467fdff17", "SAE rejection branches to Unsupported")
    cache.expect(0x1D3EBFEEC, "dce781128b050014", "SAE rejection returns Unsupported")
    cache.expect(0x1D3EC095C, "ff6300b9f92f00f908008252e8430479", "SAE accepted: SET20 auth = 0x1000")
    cache.expect(0x1D3EC07EC, "e93f40f92965413929011d125f010071ea079f1a490d490a11000014",
                 "automatic SAE AKM selection checks the same capability")
    # Extended association status 371 is a scalar Apple association result,
    # consumed through the same 1..35 error table as the GET21 join waiter.
    # This deliberately does not equate raw IEEE/WMI status with that result.
    cache.expect(0x1D3EC4338, "7fc60571e0e7ff54", "GET371 selects scalar dispatch")
    cache.expect(0x1D3EC16E4, "212e8052830080521e0a0094a0000035e00740b913000094",
                 "extended result: request 371, four-byte result, common consumer")
    cache.expect(0x1D3ED6728, "07acff97", "GET21 uses the same association-result consumer")
    cache.expect(0x1D3EC1744, "080400511f890071a8000054c90100902941039120d968b8c0035fd6",
                 "common consumer maps association results 1..35")
    results = struct.unpack("<35i", cache.read(0x1D3EF90D0, 35 * 4))
    assert (results[0], results[1], results[8], results[21]) == (0, -3906, -3905, -3925)
    # Powersave 5, rate 8, interrupt mitigation 18 and MCS 57 use req_val rather
    # than desktop versioned structs. The consumer preserves the integer;
    # these instructions establish framing, not hardware mode enumeration.
    for number in (5, 8, 18, 57):
        assert cache.jump(0x1D3EF565E, number + 1, 0x1D3EC4038) == 0x1D3EC4038
    cache.expect(0x1D3EC4038, "ff120071c10f0054a14302d1e00314aa141b0094", "scalar GET expects four-byte output")
    cache.expect(0x1D3EC40EC, "16008052481740b9a80200b9", "scalar result comes from req_val")
    cache.expect(0x1D3ECB034, "ff0200f1c41a407ae8179f1a890080522901969ae90700f9",
                 "zero-payload scalar GET still requests four output bytes")
    cache.expect(0x1D3EBA5A0, "a252009121018052f0dd6894", "scalar CopyValue wraps req_val as CFNumber Int type 9")
    cache.expect(0x1D58F1D68, "90abd8d01092099100021fd6", "number-creation stub targets CFNumberCreate")
    # These actual iOS CoreWiFi consumers confirm the numeric APIs used by
    # GET8/57. They do not establish rate units or a legacy MCS sentinel, so
    # those semantics must not be guessed from similarly named desktop types.
    cache.expect(0x1ABD7C344, "001040f9e323009101018052020080d2d3aef794", "CWFApple80211 txRate requests GET8")
    cache.expect(0x1ABD7BC6C, "001040f9e323009121078052020080d289b0f794", "CWFApple80211 MCSIndex requests GET57")
    cache.expect(0x1AFB67EA0, "901a12f010a20d9100021fd6", "CoreWiFi numeric API stub targets Apple80211CopyValue")
    # SET18 accepts a four-byte integer at the public Set API, copies it
    # unchanged to req_val, and leaves the proxy payload empty. Event 7 also
    # forwards its body unchanged. Neither consumer resolves the historical
    # OFF/AUTO enum versus legacy OFF/ON request macros in auxiliary headers;
    # polling/IRQ state therefore cannot authorize a fabricated INT_MIT value.
    assert cache.jump(0x1D3EF589C, 18 - 1, 0x1D3ECDAC8) == 0x1D3ECDAC8
    cache.expect(0x1D3ECDAD0, "ff120071e16a0154a80240b9884720b9a14302d1e00314aa84d6ff97",
                 "SET18 requires four bytes and copies the integer to req_val without enum translation")
    assert cache.read(0x1D3EF5A4E + (7 - 2), 1) == b"\0"
    cache.expect(0x1D3ED7CD4, "a8a200919f020071e303889a661644a9", "INT_MIT_CHANGED passes the original event body")
    # The actual BSS consumer classifies integer Mbps, not on-air 500-kbps
    # rate octets: OFDM {6,9,12,18,24,36,48,54}, CCK {1,2,5,11}. It also
    # wraps each stored rate word directly as SInt32 without unit conversion.
    cache.expect(0x1D3EBD26C, "0c4882d28c20a0f20c02c0f22c08e0f2cd0481520e08a0d24e00c0f28f0b2a8bef2540b9ffd90071",
                 "BSS rate classification uses OFDM/CCK integer-Mbps sets")
    cache.expect(0x1D3EBD5C0, "888b403988020034080080d2880b088b02910091e0031aaa61008052e3d16894803900b4",
                 "BSS rate words are passed unchanged to SInt32 number creation")
    if args.stock_kernel:
        kernel = StockKernel(args.stock_kernel)
        # The real GET8 Family wrapper clears its versioned scratch record,
        # sends request 8, then returns scratch +8 as the four-byte scalar.
        # BCMWLAN's connected producer divides its kbps cache by 1000 using
        # the unsigned reciprocal multiply; fractional Mbps are truncated.
        kernel.expect(0xFFFFFFF006741814, "ff7f00a9ff0b00f9", "GET8 Family clears 24-byte scratch")
        kernel.expect(0xFFFFFFF006741840, "21398d520106b87202018052e30308aaa84a009480010034",
                      "GET8 Family direct request and success branch")
        kernel.expect(0xFFFFFFF006741874, "02018052e30309aa00013fd660000035e80b40b9680200b9",
                      "GET8 Family returns scratch +8 as scalar")
        assert kernel.jump(0xFFFFFFF0054766E0, 8 - 1, 0xFFFFFFF00690096C) == 0xFFFFFFF00690177C
        kernel.expect(0xFFFFFFF00690177C, "880240f902fd43f90dfdff17", "Core GET8 calls vtable slot 0x7f8")
        kernel.expect(0xFFFFFFF0068658B0, "883800b008210891080000f9", "BCMWLAN constructor installs vtable 0xfffffff006f76208")
        assert struct.unpack("<Q", kernel.read(0xFFFFFFF006F76208 + 0x7F8, 8))[0] == 0xFFFFFFF0068C371C
        kernel.expect(0xFFFFFFF0068C371C, "084c40f9089549f9080940f9091542f9290100b400008052085148b869ba8952490ca272087d099b08fd66d3280800b9c0035fd6608084524010bc72c0035fd6",
                      "connected RATE producer returns integer kbps /1000; disconnected returns NotReady")
        for kbps in (0, 1000, 5500, 11000, 54000, 0xFFFFFFFF):
            assert (kbps * 0x10624DD3) >> 38 == kbps // 1000
        # GET57 is also a scalar on the user side. The successful legacy
        # ratespec branch leaves scratch +4 unchanged, so Family's initial
        # zero reaches the caller as compatibility default, not valid HT MCS0
        # and not a fabricated UINT32_MAX sentinel.
        assert kernel.jump(0xFFFFFFF0054766E0, 57 - 1, 0xFFFFFFF00690096C) == 0xFFFFFFF0069018C8
        assert struct.unpack("<Q", kernel.read(0xFFFFFFF006F76208 + 0x970, 8))[0] == 0xFFFFFFF0068F4494
        for address, raw, label in (
            (0xFFFFFFF006742F54, "ff0300f9", "GET57 Family clears scratch"),
            (0xFFFFFFF006742F78, "e403009121398d520106b87222078052e30308aad9440094800100340d000014", "GET57 Family direct request and success branch"),
            (0xFFFFFFF006742FB0, "22078052e30309aa00013fd660000035e80740b9680200b9", "GET57 Family returns scratch +4 as scalar"),
            (0xFFFFFFF0069018C8, "880240f902b944f9bafcff17", "Core GET57 calls vtable slot 0x970"),
            (0xFFFFFFF006900BD0, "e00314aae10313aafd7b51a9f44f50a9f6574fa9f85f4ea9fc6f4da9ff83049140001fd6", "Core forwards the original output pointer"),
            (0xFFFFFFF0068F4530, "f30300aa685c80520800bc721f00086b0418407ac1010054e82f40b9090508120a20a0523f010a6b000400540a40a0523f010a6b800000540a60a0523f010a6b81030054080d001219000014", "successful legacy ratespec skips the output store"),
            (0xFFFFFFF0068F45D8, "081d0012880600b9a8835df8", "only HT/VHT/HE write the index"),
            (0xFFFFFFF0068F45F8, "e00313aafd7b47a9f44f46a9f65745a9ff030291", "legacy returns the original successful status"),
        ):
            kernel.expect(address, raw, label)
        # Main AppleBCMWLANCore GET/SET producer, not the virtual-interface
        # helper. Both request-18 entries select the default Unsupported
        # return. It provides no INT_MIT mode that polling could impersonate.
        assert kernel.jump(0xFFFFFFF0054766E0, 18 - 1, 0xFFFFFFF00690096C) == 0xFFFFFFF006901FC0
        assert kernel.jump(0xFFFFFFF0054765F0, 18 - 1, 0xFFFFFFF0069009E0) == 0xFFFFFFF006901FC0
        kernel.expect(0xFFFFFFF006900844, "d50c8052", "original GET defaults to Unsupported 0x66")
        kernel.expect(0xFFFFFFF00690087C, "d50c8052", "original SET defaults to Unsupported 0x66")
        kernel.expect(0xFFFFFFF006901FD8, "e00315aafd7b51a9f44f50a9f6574fa9f85f4ea9fc6f4da9ff830491c0035fd6",
                      "original request 18 returns Unsupported without a mode producer")
        print("20H392 stock producer oracle passed: integer-Mbps RATE, legacy MCS compatibility default and unsupported INT_MIT")
    # GetInfoCopy queries these seven status fields only after power is ON.
    # Each unsuccessful query is skipped at 0x1d3eba0a8..0x1d3eba0cc; their
    # presence alone does not prove optional status/telemetry is required.
    assert struct.unpack("<7I", cache.read(0x1D3EF5B4C, 28)) == (18, 5, 13, 14, 15, 8, 57)
    print("20H392 IO80211 oracle passed: Bind, current-network BSS, disconnect, association/extended result, SAE gate and powersave/int-mit/rate/MCS scalar ABI")


if __name__ == "__main__":
    main()
