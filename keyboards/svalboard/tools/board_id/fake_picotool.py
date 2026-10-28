"""A stand-in for picotool, for the station's dry runs and tests.

    fake_picotool.py FOLDER <picotool arguments> --ser SERIAL

FOLDER/devices.txt lists the USB serials of the boards "in BOOTSEL"
(FOLDER/pnp_ids.txt can map one to a different flash ID). Each board is
FOLDER/<SERIAL>/ with block.bin (the 64 KiB record block) and chip.bin (see
writer/hal_host.c). Loading the ID-writer with -x runs the writer's real logic,
built for the PC (FOLDER/writer_host or $BOARD_ID_WRITER_HOST), on that board.
Firmware loads are recorded in flash_writes.txt; a write into the record block
is applied the way the boot ROM would (sector erase, then program), so a test
can prove the station never lets one through. Every call is appended to
FOLDER/calls.txt.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import board_record as br  # noqa: E402
import uf2  # noqa: E402

XIP = 0x10000000


def fail(msg):
    print(msg)
    sys.exit(1)


def main(argv):
    folder, args = argv[1], argv[2:]
    with open(os.path.join(folder, "calls.txt"), "a", encoding="utf-8") as f:
        f.write(" ".join(args) + "\n")
    if any(a in ("-f", "-F", "--force", "--force-no-reboot") for a in args):
        fail("fake picotool: forced commands are not allowed")
    if "--ser" not in args:
        fail("fake picotool: every call must name --ser")
    i = args.index("--ser")
    serial = args[i + 1]
    args = args[:i] + args[i + 2:]
    with open(os.path.join(folder, "devices.txt"), encoding="utf-8") as f:
        present = {line.strip().upper() for line in f if line.strip()}
    ids = os.path.join(folder, "pnp_ids.txt")  # USB serial -> flash ID; picotool --ser matches the flash ID
    if os.path.exists(ids):
        with open(ids, encoding="utf-8") as f:
            pairs = [line.split() for line in f if len(line.split()) == 2]
        present = {dict((a.upper(), b.upper()) for a, b in pairs).get(p, p) for p in present}
    dev = os.path.join(folder, serial)
    if serial.upper() not in present or not os.path.isdir(dev):
        fail(f"No accessible RP-series devices in BOOTSEL mode were found with serial number {serial}.")
    block_path = os.path.join(dev, "block.bin")

    if args[:2] == ["save", "-r"]:
        lo, hi, out = int(args[2], 16), int(args[3], 16), args[4]
        if XIP + br.BASE <= lo and hi <= XIP + br.BASE + br.SIZE:
            with open(block_path, "rb") as f:
                data = f.read()[lo - XIP - br.BASE:hi - XIP - br.BASE]
        elif br.REPORT_ADDR <= lo < br.REPORT_ADDR + 0x1000:
            path = os.path.join(dev, "ram_report.bin")
            data = open(path, "rb").read() if os.path.exists(path) else b"\0" * 64
            data = data.ljust(hi - lo, b"\0")[:hi - lo]
        else:
            data = b"\xff" * (hi - lo)
        with open(out, "wb") as f:
            f.write(data)
        print(f"Saving file: [==============================]  100%  ({len(data)} bytes)")
        return 0

    if args[:1] == ["load"]:
        path = args[-1]
        blocks = uf2.blocks(open(path, "rb").read())
        ram = [(a, p) for a, p in blocks if a >= 0x20000000]
        flash = [(a, p) for a, p in blocks if XIP <= a < XIP + 0x1000000]
        if ram:
            if flash:
                fail("fake picotool: mixed RAM and flash UF2")
            image = {a: p for a, p in ram}
            params = b"".join(image[a] for a in sorted(image) if br.PARAMS_ADDR <= a < br.PARAMS_ADDR + br.PARAMS_BYTES)
            ppath, rpath = os.path.join(dev, "params.bin"), os.path.join(dev, "ram_report.bin")
            with open(ppath, "wb") as f:
                f.write(params)
            if "-x" in args:
                host = os.environ.get("BOARD_ID_WRITER_HOST") or os.path.join(folder, "writer_host")
                r = subprocess.run([host, dev, ppath, rpath], capture_output=True, text=True)
                if r.returncode:
                    fail(f"fake picotool: writer_host failed: {r.stderr}")
            print("Loading into RAM: [==============================]  100%")
            print("The device was rebooted to start the code.")
            return 0
        with open(os.path.join(dev, "flash_writes.txt"), "a", encoding="utf-8") as f:
            for a, p in flash:
                f.write(f"{a - XIP:#x} {len(p)}\n")
        block = bytearray(open(block_path, "rb").read())
        for a, p in flash:
            off = a - XIP - br.BASE
            if 0 <= off < br.SIZE:
                s = off - off % 4096
                block[s:s + 4096] = b"\xff" * 4096
        for a, p in flash:
            off = a - XIP - br.BASE
            if 0 <= off < br.SIZE:
                block[off:off + len(p)] = p
        with open(block_path, "wb") as f:
            f.write(block)
        print("Loading into Flash: [==============================]  100%")
        return 0

    if args[:1] == ["reboot"]:
        if "-u" not in args:
            present.discard(serial.upper())
            with open(os.path.join(folder, "devices.txt"), "w", encoding="utf-8") as f:
                f.write("".join(s + "\n" for s in sorted(present)))
        print("The device was rebooted.")
        return 0

    if args[:1] == ["info"]:
        print(f"Device Information\n type: RP2040\n serial: {serial}")
        return 0
    fail(f"fake picotool: unsupported command {' '.join(args)}")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
