#!/usr/bin/env python3
"""Offline check of a native touchpad build (SVAL_NATIVE_PTP), no board needed.

usage: ptp_desc_check.py <firmware.elf> [--gap-mm 10]

Reads the descriptors straight out of the built ELF (symbols SvalPtpReport,
ConfigurationDescriptor and DeviceDescriptor) and checks the touchpad report
descriptor with ptp_hid.check(): page 0xFF00 blob, report sizes
35/2/257/2/2/2, 5 contacts, X/Y ranges, byte layout. It also prints the
interface and endpoint numbers and bcdDevice. Exit 1 on any failure.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ptp_hid  # noqa: E402


def elf_symbols(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    symbols = {}
    for s in sections:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, off)
            end = data.index(b"\0", strtab[4] + name)
            symbols[data[strtab[4] + name:end].decode()] = (value, size)
    return sections, symbols


def read_symbol(data, sections, symbols, name):
    if name not in symbols:
        raise KeyError(f"{name} not in the ELF (built without SVAL_NATIVE_PTP?)")
    addr, size = symbols[name]
    for s in sections:
        sh_type, sh_addr, sh_offset, sh_size = s[1], s[3], s[4], s[5]
        if sh_type == 1 and sh_addr <= addr < sh_addr + sh_size:  # SHT_PROGBITS
            off = sh_offset + addr - sh_addr
            return data[off:off + size]
    raise ValueError(f"{name} is not in a loaded section")


def interfaces(config):
    """(number, class, subclass, protocol, [endpoint addresses], hid report length)."""
    out, i, cur = [], 0, None
    while i + 2 <= len(config):
        length, kind = config[i], config[i + 1]
        if length < 2:
            break
        if kind == 4:  # interface
            cur = [config[i + 2], config[i + 5], config[i + 6], config[i + 7], [], None]
            out.append(cur)
        elif kind == 5 and cur:  # endpoint
            cur[4].append((config[i + 2], struct.unpack_from("<H", config, i + 4)[0], config[i + 6]))
        elif kind == 0x21 and cur:  # HID
            cur[5] = struct.unpack_from("<H", config, i + 7)[0]
        i += length
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf")
    ap.add_argument("--gap-mm", type=int, default=10)
    args = ap.parse_args()
    data = open(args.elf, "rb").read()
    try:
        sections, symbols = elf_symbols(data)
        desc = read_symbol(data, sections, symbols, "SvalPtpReport")
        config = read_symbol(data, sections, symbols, "ConfigurationDescriptor")
        device = read_symbol(data, sections, symbols, "DeviceDescriptor")
    except (KeyError, ValueError) as exc:
        print(f"{os.path.basename(args.elf)}: FAIL: {exc.args[0]}")
        return 1

    failures, summary = ptp_hid.check(desc, gap_mm=args.gap_mm)
    ifaces = interfaces(config)
    ptp = [f for f in ifaces if f[5] == len(desc)]
    if len(ptp) != 1:
        failures.append(f"{len(ptp)} interfaces carry the touchpad descriptor")
    elif ptp[0][0] != ifaces[-1][0]:
        failures.append("the touchpad interface is not the last one")
    total, num_ifaces = struct.unpack_from("<HB", config, 2)
    if config[1] != 2 or total != len(config):
        failures.append(f"configuration wTotalLength {total} != {len(config)} bytes")
    if num_ifaces != len(ifaces) or [f[0] for f in ifaces] != list(range(len(ifaces))):
        failures.append(f"bNumInterfaces {num_ifaces} does not match interfaces {[f[0] for f in ifaces]}")
    addrs = [a for f in ifaces for a, _, _ in f[4]]
    if len(addrs) != len(set(addrs)) or any((a & 0x7F) > 15 for a in addrs):
        failures.append(f"endpoint addresses reused or above 15: {addrs}")
    raw = [f for f in ifaces if f[1] == 3 and f[5] and f[0] == 1]
    if not raw:
        failures.append("interface 1 is not HID (raw HID must stay interface 1)")
    bcd = struct.unpack_from("<H", device, 12)[0]

    print(f"{os.path.basename(args.elf)}: touchpad descriptor {summary.get('length')} bytes, "
          f"X 0..{summary.get('xmax')}, Y 0..{summary.get('ymax')}, blob page {summary.get('cert_page')}")
    for num, cls, sub, proto, eps, hid_len in ifaces:
        mark = "  <- touchpad" if ptp and num == ptp[0][0] else ""
        ep_text = ", ".join(f"EP {a & 0x7F}{' IN' if a & 0x80 else ' OUT'} {size} B {interval} ms" for a, size, interval in eps)
        print(f"  interface {num}: class {cls:#04x}/{sub}/{proto} {ep_text}{mark}")
    print(f"  bcdDevice {bcd:#06x}")
    for f in failures:
        print("FAIL:", f)
    print("OK" if not failures else f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
