"""UF2 helpers for the board ID station (RP2040 only).

    python uf2.py ram IN.bin OUT.uf2     a RAM image at 0x20000000 (the ID-writer)
"""
import struct
import sys

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x00002000
FLAG_NOT_MAIN_FLASH = 0x00000001
RP2040_FAMILY = 0xE48BFF56
XIP_BASE = 0x10000000
RAM_BASE = 0x20000000
PAYLOAD = 256


class UF2Error(Exception):
    pass


def build(base, data):
    """UF2 bytes for data at base, 256 bytes per block."""
    data = data + b"\0" * (-len(data) % PAYLOAD)
    n = len(data) // PAYLOAD
    out = []
    for i in range(n):
        head = struct.pack("<8I", MAGIC0, MAGIC1, FLAG_FAMILY, base + i * PAYLOAD, PAYLOAD, i, n, RP2040_FAMILY)
        out.append(head + data[i * PAYLOAD:(i + 1) * PAYLOAD].ljust(476, b"\0") + struct.pack("<I", MAGIC_END))
    return b"".join(out)


def blocks(data):
    """[(addr, payload)] of an RP2040 UF2, refusing anything malformed."""
    if not data or len(data) % 512:
        raise UF2Error(f"length {len(data)} is not a positive multiple of 512")
    out = []
    for i in range(len(data) // 512):
        b = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, size, _no, _n, family = struct.unpack_from("<8I", b, 0)
        (m_end,) = struct.unpack_from("<I", b, 508)
        if (m0, m1, m_end) != (MAGIC0, MAGIC1, MAGIC_END):
            raise UF2Error(f"block {i}: bad magic")
        if flags & FLAG_NOT_MAIN_FLASH:
            continue
        if not flags & FLAG_FAMILY or family != RP2040_FAMILY:
            raise UF2Error(f"block {i}: not an RP2040 block")
        if size > 476:
            raise UF2Error(f"block {i}: payload {size} too large")
        out.append((addr, b[32:32 + size]))
    return out


def patch(data, addr, new):
    """The UF2 with len(new) bytes at addr replaced; the range must lie in its blocks."""
    data = bytearray(data)
    done = 0
    for i in range(len(data) // 512):
        off = i * 512
        _m0, _m1, _f, baddr, size = struct.unpack_from("<5I", data, off)
        lo, hi = max(addr, baddr), min(addr + len(new), baddr + size)
        if lo < hi:
            data[off + 32 + lo - baddr:off + 32 + hi - baddr] = new[lo - addr:hi - addr]
            done += hi - lo
    if done != len(new):
        raise UF2Error(f"{addr:#x}+{len(new)} is not all inside the image")
    return bytes(data)


def flash_extent(data):
    """(lowest, highest end) flash offsets a flash UF2 writes, refusing RAM or other targets."""
    lo, hi = None, None
    for addr, payload in blocks(data):
        if not XIP_BASE <= addr < XIP_BASE + 0x1000000:
            raise UF2Error(f"block at {addr:#010x} is not in flash")
        a, b = addr - XIP_BASE, addr - XIP_BASE + len(payload)
        lo = a if lo is None else min(lo, a)
        hi = b if hi is None else max(hi, b)
    if lo is None:
        raise UF2Error("no flash blocks")
    return lo, hi


def main(argv):
    if len(argv) == 4 and argv[1] == "ram":
        with open(argv[2], "rb") as f:
            image = f.read()
        with open(argv[3], "wb") as f:
            f.write(build(RAM_BASE, image))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
