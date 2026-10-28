"""Board ID station: give each new Svalboard main board its permanent board record.

Plug in one blank board at a time. It shows up in BOOTSEL (RPI-RP2). The
station reads it, loads the ID-writer into its RAM, checks the GP3 strap
against the batch, appends the record and reads it back, then (optionally)
flashes the firmware and logs a CSV line. Format: docs/board-record.md.

Once per batch:
    py board_id.py batch
Then, for every board:
    py board_id.py run
    py board_id.py run --firmware sval.uf2
Other commands:
    py board_id.py read            show the record of the board in BOOTSEL
    py board_id.py check FW.uf2    would this UF2 touch the record?

Safety: only boards in BOOTSEL (USB 2E8A:0003) are touched, one at a time.
With exactly one present, the station reads its flash unique ID with picotool
and pins every later picotool call to it (--ser matches the flash ID; the USB
serial Windows sees in BOOTSEL can be a generic one). picotool is never given
-f/-F, and the deny list is checked against the flash ID. A board that has a record already is refused unless --force,
which appends a newer record and keeps the old one. Nothing here erases flash.
"""
import argparse
import csv
import datetime
import json
import os
import random
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import board_record as br  # noqa: E402
import uf2  # noqa: E402

TOOL_VERSION = "board_id/1"
HOME = os.path.expanduser("~")
DEFAULT_DIR = os.path.join(HOME, "Svalboard", "board-id")
DEFAULT_PICOTOOL = os.path.join(HOME, ".pico-sdk", "picotool", "2.1.1", "picotool", "picotool.exe")
WRITER_UF2 = os.environ.get("BOARD_ID_WRITER_UF2") or os.path.join(HERE, "writer.uf2")
FIRMWARE_MAX = 0x160000  # flash_reservation.ld
RECORD_XIP = 0x10000000 + br.BASE
CSV_FIELDS = ["time_utc", "result", "uid", "bootsel_serial", "model", "revision", "lot", "strap", "strap_counts", "page",
              "records", "firmware", "station", "detail"]


def load_models():
    with open(os.path.join(HERE, "models.json"), encoding="utf-8") as f:
        return json.load(f)


# ---- boards ----------------------------------------------------------------------------

class Boards:
    """BOOTSEL boards on this PC, and picotool pinned to one of them."""

    def __init__(self, picotool):
        self.picotool_exe = picotool

    def list(self):
        """Serials of RP2040 boards in BOOTSEL (USB 2E8A:0003)."""
        cmd = ("Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\\VID_2E8A&PID_0003\\*' }"
               " | ForEach-Object { $_.InstanceId }")
        out = subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout
        found = []
        for line in out.splitlines():
            m = re.fullmatch(r"USB\\VID_2E8A&PID_0003\\([^\\&]+)", line.strip(), re.I)
            if m:
                found.append(m.group(1).upper())
        return sorted(set(found))

    def flash_id(self, pnp_serial):
        """The flash unique ID of the one board in BOOTSEL, or None. picotool's
        --ser matches this ID, not the USB serial Windows lists."""
        if self.list() != [pnp_serial]:
            return None  # only ever asked with exactly one board present
        try:
            p = subprocess.run([self.picotool_exe, "info", "-d"], capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            return None
        m = re.search(r"flash id:\s*0x([0-9A-Fa-f]{16})", p.stdout)
        return m.group(1).upper() if m else None

    def picotool(self, serial, *args, timeout=60):
        cmd = [self.picotool_exe, *args, "--ser", serial]
        if any(a in ("-f", "-F", "--force", "--force-no-reboot") for a in args):
            raise RuntimeError("picotool is never forced")
        t0 = time.monotonic()
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, "picotool timed out"
        out = (p.stdout + p.stderr).strip()
        if os.environ.get("BOARD_ID_VERBOSE"):
            said = " | ".join(l.strip() for l in re.split(r"[
]+", out) if l.strip() and "%" not in l)
            print(f"  [picotool {' '.join(args)} --ser {serial}] exit {p.returncode}, {time.monotonic() - t0:.2f} s: {said}")
        return p.returncode, out


class FakeBoards(Boards):
    """Dry runs: boards simulated by fake_picotool.py in a folder."""

    def __init__(self, folder):
        super().__init__(None)
        self.folder = folder

    def list(self):
        path = os.path.join(self.folder, "devices.txt")
        if not os.path.exists(path):
            return []
        with open(path, encoding="utf-8") as f:
            return sorted({line.strip().upper() for line in f if line.strip()})

    def flash_id(self, pnp_serial):
        """pnp_ids.txt maps a USB serial to a flash ID ("PNP UID" lines); else they are the same."""
        path = os.path.join(self.folder, "pnp_ids.txt")
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) == 2 and parts[0].upper() == pnp_serial:
                        return parts[1].upper()
        return pnp_serial

    def picotool(self, serial, *args, timeout=60):
        cmd = [sys.executable, "-I", os.path.join(HERE, "fake_picotool.py"), self.folder, *args, "--ser", serial]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout + p.stderr).strip()


# ---- one board -------------------------------------------------------------------------

def read_range(boards, serial, lo, hi, tmp):
    path = os.path.join(tmp, f"r{lo:08x}.bin")
    if os.path.exists(path):
        os.remove(path)
    rc, out = boards.picotool(serial, "save", "-r", f"{lo:x}", f"{hi:x}", path)
    if rc or not os.path.exists(path):
        return None, out
    with open(path, "rb") as f:
        return f.read()[:hi - lo], out


def read_block(boards, serial, tmp):
    data, out = read_range(boards, serial, RECORD_XIP, RECORD_XIP + br.SIZE, tmp)
    if data is None or len(data) != br.SIZE:
        return None, out
    return br.scan(data), out


def wait_for(boards, serial, present, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if (serial in boards.list()) == present:
            return True
        time.sleep(0.3)
    return False


def describe(rec):
    if not rec:
        return "none"
    when = datetime.datetime.fromtimestamp(rec["written"], datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    return (f"{rec.get('model', '?')} {rec.get('revision', '')} lot {rec.get('lot', '?')}, uid {rec['uid']},"
            f" strap {rec.get('strap', '?')}, written {when} by {rec.get('station', '?')}")


def process(boards, pnp, batch, opts, log=print):
    """Identify one BOOTSEL board (pnp: the USB serial it enumerates with). Returns the CSV row (dict)."""
    models = load_models()
    row = {"time_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "bootsel_serial": pnp,
           "model": batch["model"], "revision": batch["revision"], "lot": batch["lot"], "station": batch["station"], "uid": "",
           "strap": "", "strap_counts": "", "page": "", "records": "", "firmware": "", "detail": ""}

    def done(result, detail=""):
        row["result"], row["detail"] = result, detail
        return row

    deny = models.get("deny", {})
    if pnp in deny:
        return done("DENIED", f"{pnp} is on the deny list ({deny[pnp]}); not touched")
    serial = boards.flash_id(pnp)  # every picotool call below is pinned to it
    if not serial:
        return done("FAIL_PICOTOOL", "could not read the board's flash ID (is exactly one board in BOOTSEL?)")
    row["uid"] = serial
    if serial in deny:
        return done("DENIED", f"{serial} is on the deny list ({deny[serial]}); nothing written")
    tmp = tempfile.mkdtemp(prefix="board_id_")
    try:
        # 1. What it holds already.
        scan, out = read_block(boards, serial, tmp)
        if scan is None:
            return done("FAIL_PICOTOOL", f"could not read the record block: {out}")
        row["records"] = scan["identities"]
        if scan["identities"] and not opts.force:
            cur = scan["current"]
            row["uid"] = cur["uid"]
            return done("REFUSED_HAS_RECORD", f"already identified: {describe(cur)}")

        # 2. The ID-writer, with this batch's fields.
        expect = br.EXPECT_ANY if opts.strap_any else br.STRAP_BY_NAME[batch["strap"]]
        fields = br.station_tlv(batch["model"], batch["revision"], batch["lot"], batch["station"],
                                None if opts.strap_any else expect, batch.get("notes", ""))
        nonce = random.getrandbits(32)
        flags = (br.FLAG_FORCE if opts.force else 0) | (br.FLAG_DRY if opts.dry else 0)
        written = int(time.time())
        with open(WRITER_UF2, "rb") as f:
            image = uf2.patch(f.read(), br.PARAMS_ADDR, br.params(nonce, written, fields, expect, flags))
        patched = os.path.join(tmp, "writer.uf2")
        with open(patched, "wb") as f:
            f.write(image)
        rc, out = boards.picotool(serial, "load", "-x", patched)
        # The writer reboots the board into BOOTSEL when it is done, so picotool
        # may report the board going away; the report decides.
        time.sleep(opts.settle)
        if not wait_for(boards, pnp, True, 20):
            return done("FAIL_NO_REPORT", f"the board did not come back in BOOTSEL after the writer (picotool: {out})")

        # 3. The writer's report.
        rep = None
        for _ in range(5):
            data, _out = read_range(boards, serial, br.REPORT_ADDR, br.REPORT_ADDR + 256, tmp)
            rep = br.parse_report(data) if data else None
            if rep and rep["nonce"] == nonce:
                break
            rep = None
            time.sleep(0.5)
        after, _out = read_block(boards, serial, tmp)
        if rep is None:
            new = after and after["identities"] > scan["identities"]
            if new:
                row["uid"], row["page"] = after["current"]["uid"], after["newest"]
            return done("FAIL_NO_REPORT", "no report from the writer" + (
                f"; a new record is in flash: {describe(after['current'])}" if new else "; nothing was written") + f" (picotool: {out})")
        if rep["uid"] != serial and rep["status"] not in ("NO_UID", "NOT_16MB", "BAD_PARAMS"):
            return done("FAIL_VERIFY", f"the writer read flash ID {rep['uid']}, picotool {serial}")
        row["uid"], row["strap"] = rep["uid"], rep["strap"]
        row["strap_counts"] = "/".join(str(c) for c in rep["strap_counts"])
        row["page"] = rep["page"]
        status = rep["status"]
        if status == "DRY_OK":
            return done("PASS_DRY", f"dry run: would write page {rep['page']} ({rep['record_len']} bytes); strap {rep['strap']}")
        if status == "HAS_RECORD":
            return done("REFUSED_HAS_RECORD", f"the writer found a record: {describe(after and after['current'])}")
        if status == "STRAP_MISMATCH":
            return done("FAIL_STRAP_MISMATCH", f"GP3 reads {rep['strap']} ({row['strap_counts']} of 64 high under pull-up/down/none);"
                        f" a {batch['model']} reads {batch['strap']}. Nothing written. Check the board, or the batch.")
        if status != "WRITTEN":
            return done("FAIL_" + status, f"writer status {status}; flash capacity code {rep['jedec_capacity']:#04x}")

        # 4. Read the record back independently.
        if after is None or after["newest"] != rep["page"]:
            return done("FAIL_VERIFY", "the record did not read back from flash")
        cur = after["current"]
        want = {"model": batch["model"], "revision": batch["revision"], "lot": batch["lot"], "station": batch["station"]}
        bad = [k for k, v in want.items() if cur.get(k) != v]
        if cur["uid"] != rep["uid"] or bad:
            return done("FAIL_VERIFY", f"the record read back differs: {bad or 'uid'}")
        row["records"] = after["identities"]
        notes = []

        # 5. Firmware, optionally.
        if opts.firmware:
            rc, out = boards.picotool(serial, "load", "-v", opts.firmware, timeout=180)
            if rc:
                return done("FAIL_FIRMWARE", f"record written; firmware load failed: {out}")
            row["firmware"] = os.path.basename(opts.firmware)
            rc, out = boards.picotool(serial, "reboot", "-a")
            if rc:
                notes.append(f"reboot: {out}")
        return done("PASS", "; ".join([describe(cur)] + notes))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---- commands --------------------------------------------------------------------------

GREEN, RED, YELLOW, BOLD, END = "\033[42;30m", "\033[41;97m", "\033[43;30m", "\033[1m", "\033[0m"


def banner(row):
    r = row["result"]
    color = GREEN if r.startswith("PASS") else YELLOW if r.startswith(("REFUSED", "DENIED")) else RED
    word = "PASS" if r.startswith("PASS") else "REFUSED" if r.startswith(("REFUSED", "DENIED")) else "FAIL"
    print(f"\n{color}{BOLD}   {word}   {END}  {r}  {row['uid'] or row['bootsel_serial']}")
    print(f"   {row['detail']}\n")


def log_row(folder, row):
    path = os.path.join(folder, "board_id_log.csv")
    new = not os.path.exists(path)
    with open(path, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        if new:
            w.writeheader()
        w.writerow({k: row.get(k, "") for k in CSV_FIELDS})


def check_firmware(path):
    """None if the UF2 is a firmware image that stays clear of the record; else why not."""
    with open(path, "rb") as f:
        data = f.read()
    try:
        lo, hi = uf2.flash_extent(data)
    except uf2.UF2Error as e:
        return str(e)
    if hi > FIRMWARE_MAX:
        return f"it writes flash up to {hi:#x}, past the firmware area's end {FIRMWARE_MAX:#x}"
    if lo < br.BASE + br.SIZE and br.BASE < hi:
        return "it writes the board record block"
    return None


def ask(prompt, default="", choices=None):
    while True:
        hint = f" [{default}]" if default else ""
        value = input(f"{prompt}{hint}: ").strip() or default
        if choices and value.isdigit() and 1 <= int(value) <= len(choices):
            return choices[int(value) - 1]
        if value and (not choices or value in choices):
            return value


def cmd_batch(args):
    models = load_models()["models"]
    names = [m["model"] for m in models]
    model = args.model
    if not model:
        for i, n in enumerate(names, 1):
            print(f"  {i}. {n}")
        model = ask("Model", names[0], names)
    if model not in names:
        sys.exit(f"unknown model {model!r}; models.json lists {', '.join(names)}")
    m = models[names.index(model)]
    revision = args.revision if args.revision is not None else ask("PCB revision (as on the silkscreen)", (m["revisions"] or [""])[0])
    lot = args.lot if args.lot is not None else ask("Batch or lot")
    station = args.station or socket.gethostname()
    batch = {"model": model, "strap": m["strap"], "revision": revision, "lot": lot, "station": f"{station} {TOOL_VERSION}",
             "notes": args.notes or "", "created": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")}
    os.makedirs(args.dir, exist_ok=True)
    with open(os.path.join(args.dir, "batch.json"), "w", encoding="utf-8") as f:
        json.dump(batch, f, indent=2)
    print(f"Batch saved: {model} {revision}, lot {lot}; GP3 should read {m['strap']}.")


def load_batch(folder):
    path = os.path.join(folder, "batch.json")
    if not os.path.exists(path):
        sys.exit("No batch yet. Start one with: py board_id.py batch")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def make_boards(args):
    if args.fake:
        return FakeBoards(args.fake)
    if not os.path.exists(args.picotool):
        sys.exit(f"picotool not found at {args.picotool}; give --picotool")
    return Boards(args.picotool)


def one_board(boards):
    devs = boards.list()
    if len(devs) != 1:
        sys.exit(f"{len(devs)} boards in BOOTSEL; plug in exactly one")
    return devs[0]


def cmd_run(args):
    if args.firmware:
        why = check_firmware(args.firmware)
        if why:
            sys.exit(f"Refusing the firmware {args.firmware}: {why}")
    batch = load_batch(args.dir)
    boards = make_boards(args)
    os.system("")  # ANSI colors in a Windows console
    print(f"Batch: {batch['model']} {batch['revision']}, lot {batch['lot']}; GP3 should read {batch['strap']}"
          + (" (not checked)" if args.strap_any else "") + (f"; firmware {args.firmware}" if args.firmware else "")
          + ("; DRY RUN, nothing written" if args.dry else "") + ("; FORCE: append to boards with a record" if args.force else ""))
    handled, waiting, count = set(), "", 0
    while True:
        devs = boards.list()
        handled &= set(devs)
        fresh = [d for d in devs if d not in handled]
        msg = ("Plug in the next board in BOOTSEL." if not devs else
               f"{len(devs)} boards in BOOTSEL: leave only one plugged in." if len(devs) > 1 else
               "Done with this board: unplug it." if not fresh else "")
        if msg:
            if msg != waiting:
                print(msg)
                waiting = msg
            if args.once and not devs and count:
                return 0
            if args.once and args.no_wait:
                return 1
            time.sleep(0.5)
            continue
        waiting = ""
        serial = fresh[0]
        print(f"Board {serial}: identifying...")
        row = process(boards, serial, batch, args)
        log_row(args.dir, row)
        banner(row)
        handled.add(serial)
        count += 1
        if args.once:
            return 0 if row["result"].startswith("PASS") else 1


def cmd_read(args):
    boards = make_boards(args)
    pnp = one_board(boards)
    serial = boards.flash_id(pnp)
    if not serial:
        sys.exit(f"could not read the flash ID of {pnp}")
    tmp = tempfile.mkdtemp(prefix="board_id_")
    try:
        scan, out = read_block(boards, serial, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if scan is None:
        sys.exit(f"could not read {serial}: {out}")
    print(f"Board {serial}: {scan['identities']} identity record(s), {scan['used']} page(s) used, {scan['damaged']} damaged")
    for page, rec in scan["records"]:
        mark = "current" if page == scan["newest"] else "older"
        print(f"  page {page} ({mark}): {describe(rec)}")
    return 0


def cmd_check(args):
    why = check_firmware(args.uf2)
    print(f"{args.uf2}: " + (f"REFUSED: {why}" if why else "OK: firmware area only; the board record is untouched"))
    return 1 if why else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", default=DEFAULT_DIR, help="batch file and CSV log folder")
    ap.add_argument("--picotool", default=DEFAULT_PICOTOOL)
    ap.add_argument("--fake", metavar="DIR", help="dry run against simulated boards (fake_picotool.py)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("batch", help="start a batch: model, revision, lot")
    b.add_argument("--model")
    b.add_argument("--revision")
    b.add_argument("--lot")
    b.add_argument("--station", help="station name (default: this PC's name)")
    b.add_argument("--notes")
    r = sub.add_parser("run", help="identify boards one after another")
    r.add_argument("--firmware", help="a firmware UF2 to flash after the record")
    r.add_argument("--force", action="store_true", help="append a newer record to a board that has one")
    r.add_argument("--dry", action="store_true", help="read and check only; write nothing")
    r.add_argument("--strap-any", action="store_true", help="record the strap without checking it")
    r.add_argument("--once", action="store_true", help="stop after one board")
    r.add_argument("--no-wait", action="store_true", help=argparse.SUPPRESS)
    r.add_argument("--settle", type=float, default=1.0, help=argparse.SUPPRESS)
    sub.add_parser("read", help="show the record of the board in BOOTSEL")
    c = sub.add_parser("check", help="check that a firmware UF2 stays clear of the record")
    c.add_argument("uf2")
    args = ap.parse_args(argv)
    return {"batch": cmd_batch, "run": cmd_run, "read": cmd_read, "check": cmd_check}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
