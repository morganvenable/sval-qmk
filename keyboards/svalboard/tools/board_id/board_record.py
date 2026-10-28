"""The board record, the ID-writer's params and its report, in Python.

Mirrors keyboards/svalboard/board_record.h and tools/board_id/writer/writer.h;
tests/sval_board_id checks the two agree byte for byte.
"""
import struct
import zlib

BASE, SIZE, PAGE = 0xF00000, 0x10000, 256
PAGES = SIZE // PAGE
MAGIC, VERSION = 0x52425653, 1
HEADER, CRC = 20, 4
KIND_IDENTITY = 1

TAG_MODEL, TAG_REVISION, TAG_LOT, TAG_STATION = 1, 2, 3, 4
TAG_STRAP, TAG_TESTS, TAG_NOTES, TAG_EXPECTED = 5, 6, 7, 8
TEXT_TAGS = {TAG_MODEL: "model", TAG_REVISION: "revision", TAG_LOT: "lot", TAG_STATION: "station", TAG_NOTES: "notes"}

STRAP_LOW, STRAP_HIGH, STRAP_FLOATING, STRAP_UNSTABLE, STRAP_NOT_READ = 0, 1, 2, 3, 0xFF
STRAP_NAMES = {STRAP_LOW: "low", STRAP_HIGH: "high", STRAP_FLOATING: "floating", STRAP_UNSTABLE: "unstable", STRAP_NOT_READ: "not read"}
STRAP_BY_NAME = {v: k for k, v in STRAP_NAMES.items()}

PARAMS_ADDR, PARAMS_MAGIC, PARAMS_BYTES, TLV_MAX = 0x20000100, 0x50425653, 256, 232
FLAG_FORCE, FLAG_DRY, EXPECT_ANY = 1, 2, 0xFF
REPORT_ADDR, REPORT_MAGIC, REPORT_BYTES = 0x20010000, 0x4D425653, 40

STATUS = {0: "WRITTEN", 1: "HAS_RECORD", 2: "STRAP_MISMATCH", 3: "STRAP_UNSTABLE", 4: "NOT_16MB", 5: "NO_UID", 6: "FULL",
          7: "VERIFY_FAILED", 8: "BAD_PARAMS", 9: "DRY_OK", 10: "TOO_LONG"}


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def tlv(fields):
    """Encode [(tag, bytes|str)] as TLV bytes."""
    out = b""
    for tag, value in fields:
        if isinstance(value, str):
            value = value.encode("utf-8")
        if not 0 <= len(value) <= 255:
            raise ValueError(f"field {tag} is {len(value)} bytes, at most 255")
        out += bytes([tag, len(value)]) + value
    return out


def station_tlv(model, revision, lot, station, expect=None, notes=""):
    fields = [(TAG_MODEL, model), (TAG_REVISION, revision), (TAG_LOT, lot), (TAG_STATION, station)]
    if expect is not None:
        fields.append((TAG_EXPECTED, bytes([expect])))
    if notes:
        fields.append((TAG_NOTES, notes))
    return tlv(fields)


def parse_tlv(b):
    out, i = [], 0
    while i < len(b):
        if len(b) - i < 2 or len(b) - i - 2 < b[i + 1]:
            raise ValueError("malformed TLV")
        out.append((b[i], b[i + 2:i + 2 + b[i + 1]]))
        i += 2 + b[i + 1]
    return out


def build(uid, written, fields_tlv, strap=None):
    """One identity record, as board_record_build() writes it (without the 0xFF fill)."""
    body = b""
    if strap is not None:
        body += bytes([TAG_STRAP, 4]) + bytes(strap)
    body += fields_tlv
    length = HEADER + len(body) + CRC
    if length > PAGE:
        raise ValueError(f"record is {length} bytes, at most {PAGE}")
    head = struct.pack("<IBBH8sI", MAGIC, VERSION, KIND_IDENTITY, length, bytes(uid), written) + body
    return head + struct.pack("<I", crc32(head))


def check(page):
    """The record at the start of page as a dict, or None if it is not a valid record."""
    if len(page) < HEADER + CRC:
        return None
    magic, version, kind, length, uid, written = struct.unpack_from("<IBBH8sI", page)
    if magic != MAGIC or version != VERSION or not HEADER + CRC <= length <= PAGE or length > len(page):
        return None
    if struct.unpack_from("<I", page, length - CRC)[0] != crc32(page[:length - CRC]):
        return None
    try:
        fields = parse_tlv(page[HEADER:length - CRC])
    except ValueError:
        return None
    rec = {"kind": kind, "length": length, "uid": uid.hex().upper(), "written": written, "fields": fields}
    for tag, value in fields:
        if tag in TEXT_TAGS and TEXT_TAGS[tag] not in rec:
            rec[TEXT_TAGS[tag]] = value.decode("utf-8", "replace")
        elif tag == TAG_STRAP and len(value) == 4 and "strap" not in rec:
            rec["strap"] = STRAP_NAMES.get(value[0], f"code {value[0]}")
            rec["strap_counts"] = list(value[1:])
        elif tag == TAG_EXPECTED and len(value) == 1 and "expected" not in rec:
            rec["expected"] = STRAP_NAMES.get(value[0], f"code {value[0]}")
        elif tag == TAG_TESTS and len(value) == 8 and "tests" not in rec:
            rec["tests"] = struct.unpack("<II", value)
    return rec


def scan(block):
    """As board_record_scan(): {'records': [(page, rec)], 'newest': page|None, 'used', 'damaged', 'append'}."""
    if len(block) != SIZE:
        raise ValueError(f"block is {len(block)} bytes, not {SIZE}")
    records, used, damaged, newest = [], 0, 0, None
    for i in range(PAGES):
        page = block[i * PAGE:(i + 1) * PAGE]
        if page[:4] == b"\xff\xff\xff\xff":
            break
        used = i + 1
        rec = check(page)
        if rec is None:
            damaged += 1
            continue
        records.append((i, rec))
        if rec["kind"] == KIND_IDENTITY:
            newest = i
    append = next((i for i in range(used, PAGES) if block[i * PAGE:(i + 1) * PAGE] == b"\xff" * PAGE), None)
    identities = [r for _, r in records if r["kind"] == KIND_IDENTITY]
    return {"records": records, "identities": len(identities), "newest": newest, "used": used, "damaged": damaged, "append": append,
            "current": dict(records)[newest] if newest is not None else None}


def params(nonce, written, fields_tlv, expect=EXPECT_ANY, flags=0):
    if len(fields_tlv) > TLV_MAX:
        raise ValueError(f"fields are {len(fields_tlv)} bytes, at most {TLV_MAX}")
    head = struct.pack("<IBBBBIIHH", PARAMS_MAGIC, 1, flags, expect, 0, nonce, written, len(fields_tlv), 0)
    head += fields_tlv.ljust(TLV_MAX, b"\0")
    return head + struct.pack("<I", crc32(head))


def parse_report(b):
    """The writer's report as a dict, or None if it is missing or damaged."""
    if len(b) < REPORT_BYTES:
        return None
    b = b[:REPORT_BYTES]
    (magic, version, status, strap, jedec, nonce, uid, pu, pd, np_, samples, page, rec_len, identities, used, damaged, wver,
     crc) = struct.unpack("<IBBBBI8sBBBBhHHHHHI", b)
    if magic != REPORT_MAGIC or version != 1 or crc != crc32(b[:-4]):
        return None
    return {"status": STATUS.get(status, f"code {status}"), "status_code": status, "strap": STRAP_NAMES.get(strap, f"code {strap}"),
            "jedec_capacity": jedec, "nonce": nonce, "uid": uid.hex().upper(), "strap_counts": [pu, pd, np_], "samples": samples,
            "page": page, "record_len": rec_len, "identities": identities, "used": used, "damaged": damaged, "writer_version": wver}
