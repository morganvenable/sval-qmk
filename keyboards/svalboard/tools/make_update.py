#!/usr/bin/env python3
"""Build a signed Svalboard update (.svup) from a QMK .uf2.

A .svup is the 108-byte manifest, its 64-byte Ed25519 signature, then the raw
image the updater stages and copies to flash offset 0. The manifest layout and
the image rules match keyboards/svalboard/updater/update_manifest.h and
update_image.c; the plan is docs/updater-plan.md (M1).

Refuses, with exit status 2: a UF2 that is malformed, for another chip family,
not starting at 0x10000000, with gaps or overlaps, or ending beyond 0x10160000;
an image whose boot2 CRC, initial SP or reset vector is wrong; and key, flag
and field combinations the firmware would refuse anyway.

    make_update.py IN.uf2 -o OUT.svup --kb svalboard/trackball/pmw3389/right \\
        --keymap sval --version-string 1.2.3 --fw-version 10203

Signs with the TEST-ONLY key (key_id 0) unless --key and --key-id say
otherwise. Uses the 'cryptography' package for Ed25519 when it is installed and
a pure-Python RFC 8032 signer otherwise; both give identical signatures.
"""
import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEST_KEY_FILE = HERE / "sval_update_TEST_ONLY.key"

# ---- format (update_manifest.h) ----------------------------------------------------

MANIFEST_MAGIC = 0x50555653  # "SVUP"
MANIFEST_VERSION = 1
UPDATER_PROTO = 1
MANIFEST_FMT = "<IHBBBBHHBBIII16s64s"
MANIFEST_BYTES = 108
SIG_BYTES = 64
assert struct.calcsize(MANIFEST_FMT) == MANIFEST_BYTES

KEY_TEST, KEY_COUNT = 0, 3
HAND = {"left": 0, "right": 1}
POINTING = {"none": 0, "trackpoint": 1, "pmw3360": 2, "pmw3389": 3, "azoteq": 4}
KEYMAP = {"sval": 1, "blank": 2}
FLAG_RELEASE, FLAG_DIAGNOSTIC = 0x0001, 0x0002
STORAGE_FORMAT = 2  # SVAL_UPDATE_STORAGE_FORMAT in keyboards/svalboard/config.h

# ---- image rules (update_image.c, config.h) ---------------------------------------

XIP_BASE = 0x10000000
MAX_IMAGE = 0x160000  # SVAL_UPDATE_MAX_IMAGE; also the linker cap (flash_reservation.ld)
IMAGE_MIN, IMAGE_ALIGN = 0x200, 0x100
SP_MIN, SP_MAX = 0x20000000, 0x20042000
RP2040_FAMILY = 0xE48BFF56
UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_FLAG_NOT_MAIN_FLASH, UF2_FLAG_FAMILY = 0x00000001, 0x00002000


class Refused(Exception):
    pass


def crc32_mpeg2(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def boot2_valid(page0):
    return len(page0) >= 256 and crc32_mpeg2(page0[:252]) == struct.unpack_from("<I", page0, 252)[0]


def image_problems(image):
    """The device's structure checks (update_image_head_check). Empty when fine."""
    problems = []
    n = len(image)
    if n < IMAGE_MIN or n % IMAGE_ALIGN:
        problems.append(f"length {n:#x} is not a multiple of 256 of at least {IMAGE_MIN:#x}")
    if n > MAX_IMAGE:
        problems.append(f"length {n:#x} is above the maximum {MAX_IMAGE:#x}")
    if n < 0x108:
        return problems + ["too short for a vector table"]
    if not boot2_valid(image):
        problems.append("boot2 CRC (CRC-32/MPEG-2 over bytes 0-251) does not match the word at 252")
    sp, reset = struct.unpack_from("<II", image, 0x100)
    if not SP_MIN <= sp <= SP_MAX:
        problems.append(f"initial SP {sp:#010x} is outside [{SP_MIN:#x}, {SP_MAX:#x}]")
    if not reset & 1 or not XIP_BASE + 0x100 <= reset < XIP_BASE + n:
        problems.append(f"reset vector {reset:#010x} is not odd and inside the image")
    return problems


def uf2_to_image(data):
    """Raw image from a UF2, refusing anything but one contiguous run at XIP_BASE."""
    if not data or len(data) % 512:
        raise Refused(f"UF2 length {len(data)} is not a positive multiple of 512")
    count = len(data) // 512
    image = bytearray()
    for i in range(count):
        block = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, size, block_no, num_blocks, family = struct.unpack_from("<8I", block, 0)
        (m_end,) = struct.unpack_from("<I", block, 508)
        if (m0, m1, m_end) != (UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END):
            raise Refused(f"block {i}: bad UF2 magic")
        if flags & UF2_FLAG_NOT_MAIN_FLASH:
            raise Refused(f"block {i}: marked not-main-flash")
        if not flags & UF2_FLAG_FAMILY or family != RP2040_FAMILY:
            raise Refused(f"block {i}: family {family:#010x} is not RP2040 ({RP2040_FAMILY:#010x})")
        if size != 256:
            raise Refused(f"block {i}: payload {size} bytes, expected 256")
        if block_no != i or num_blocks != count:
            raise Refused(f"block {i}: numbered {block_no} of {num_blocks}, expected {i} of {count}")
        expected = XIP_BASE + 256 * i
        if addr != expected:
            what = "starts" if i == 0 else "has a gap or overlap"
            raise Refused(f"block {i}: address {addr:#010x}, expected {expected:#010x} (image {what})")
        image += block[32:32 + 256]
    if XIP_BASE + len(image) > XIP_BASE + MAX_IMAGE:
        raise Refused(f"image ends at {XIP_BASE + len(image):#010x}, beyond {XIP_BASE + MAX_IMAGE:#010x}")
    return bytes(image)


# ---- Ed25519 ---------------------------------------------------------------------------
# Pure-Python signer, following RFC 8032 section 5.1 (extended coordinates).
# Not constant time: fine for a build tool signing on its own machine.

_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _add(a, b):
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    pa = (y1 - x1) * (y2 - x2) % _P
    pb = (y1 + x1) * (y2 + x2) % _P
    pc = 2 * t1 * t2 * _D % _P
    pd = 2 * z1 * z2 % _P
    e, f, g, h = pb - pa, pd - pc, pd + pc, pb + pa
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(s, pt):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, pt)
        pt = _add(pt, pt)
        s >>= 1
    return q


def _compress(pt):
    x, y, z, _ = pt
    zi = pow(z, _P - 2, _P)
    x, y = x * zi % _P, y * zi % _P
    return (y | (x & 1) << 255).to_bytes(32, "little")


def _recover_x(y, sign):
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if x & 1 != sign:
        x = _P - x
    return x


_GY = 4 * pow(5, _P - 2, _P) % _P
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % _P)


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a = (a & ((1 << 254) - 8)) | (1 << 254)
    return a, h[32:]


def _hint(*parts):
    return int.from_bytes(hashlib.sha512(b"".join(parts)).digest(), "little") % _L


def pure_public(seed):
    return _compress(_mul(_expand(seed)[0], _G))


def pure_sign(seed, msg):
    a, prefix = _expand(seed)
    pub = _compress(_mul(a, _G))
    r = _hint(prefix, msg)
    big_r = _compress(_mul(r, _G))
    s = (r + _hint(big_r, pub, msg) * a) % _L
    return big_r + s.to_bytes(32, "little")


def _openssl():
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives import serialization
        return Ed25519PrivateKey, serialization
    except ImportError:
        return None


def public_key(seed, signer="auto"):
    lib = _openssl() if signer in ("auto", "openssl") else None
    if signer == "openssl" and not lib:
        raise Refused("--signer openssl needs the 'cryptography' package")
    if lib:
        key, ser = lib
        return key.from_private_bytes(seed).public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    return pure_public(seed)


def sign(seed, msg, signer="auto"):
    lib = _openssl() if signer in ("auto", "openssl") else None
    if signer == "openssl" and not lib:
        raise Refused("--signer openssl needs the 'cryptography' package")
    if lib:
        return lib[0].from_private_bytes(seed).sign(msg)
    return pure_sign(seed, msg)


def read_key(path):
    lines = [ln.strip() for ln in Path(path).read_text().splitlines()]
    hexes = [ln for ln in lines if ln and not ln.startswith("#")]
    if len(hexes) != 1 or len(hexes[0]) != 64:
        raise Refused(f"{path}: expected one line of 64 hex digits (a 32-byte Ed25519 seed)")
    return bytes.fromhex(hexes[0])


def test_public_key():
    return pure_public(read_key(TEST_KEY_FILE))


# ---- manifest ---------------------------------------------------------------------------


def build_manifest(image, *, key_id, hand, pointing_id, keymap_id, flags, epoch, storage_format, fw_version, version):
    vbytes = version.encode("ascii")
    if len(vbytes) > 16 or b"\0" in vbytes:
        raise Refused("--version-string must be at most 16 ASCII characters")
    return struct.pack(MANIFEST_FMT, MANIFEST_MAGIC, MANIFEST_VERSION, key_id, hand, pointing_id, keymap_id, flags, epoch,
                       storage_format, UPDATER_PROTO, len(image), fw_version, 0, vbytes.ljust(16, b"\0"),
                       hashlib.sha512(image).digest())


def parse_manifest(raw):
    f = struct.unpack(MANIFEST_FMT, raw[:MANIFEST_BYTES])
    names = ("magic", "manifest_ver", "key_id", "hand", "pointing_id", "keymap_id", "flags", "security_epoch",
             "storage_format", "updater_proto", "image_len", "fw_version", "reserved", "version", "sha512")
    return dict(zip(names, f))


def parse_svup(data):
    """(manifest bytes, signature, image) from a .svup, checking only its framing."""
    if len(data) < MANIFEST_BYTES + SIG_BYTES:
        raise Refused(".svup is too short")
    manifest, sig, image = data[:MANIFEST_BYTES], data[MANIFEST_BYTES:MANIFEST_BYTES + SIG_BYTES], data[MANIFEST_BYTES + SIG_BYTES:]
    if parse_manifest(manifest)["image_len"] != len(image):
        raise Refused(".svup image length does not match its manifest")
    return manifest, sig, image


def kb_fields(kb):
    """hand and pointing_id from a keyboard path such as svalboard/trackball/pmw3389/right."""
    parts = kb.strip("/").split("/")
    if not parts or parts[0] != "svalboard" or parts[-1] not in HAND:
        raise Refused(f"--kb {kb!r}: expected svalboard/.../left or svalboard/.../right")
    middle = parts[1:-1]
    table = {(): "none", ("trackpoint",): "trackpoint", ("trackball", "pmw3360"): "pmw3360",
             ("trackball", "pmw3389"): "pmw3389", ("azoteq",): "azoteq"}
    if tuple(middle) not in table:
        raise Refused(f"--kb {kb!r}: unknown variant")
    return HAND[parts[-1]], POINTING[table[tuple(middle)]]


def make(args):
    data = Path(args.input).read_bytes()
    image = data if args.raw else uf2_to_image(data)
    if args.raw and (len(image) > MAX_IMAGE):
        raise Refused(f"image is {len(image):#x} bytes, above {MAX_IMAGE:#x}")

    if args.kb:
        hand, pointing_id = kb_fields(args.kb)
    else:
        hand, pointing_id = None, None
    if args.hand is not None:
        hand = HAND[args.hand]
    if args.pointing is not None:
        pointing_id = POINTING[args.pointing]
    if hand is None or pointing_id is None:
        raise Refused("give --kb, or both --hand and --pointing")

    flags = (FLAG_RELEASE if args.release else 0) | (FLAG_DIAGNOSTIC if args.diagnostic else 0)
    if args.release and args.diagnostic:
        raise Refused("a RELEASE image cannot be DIAGNOSTIC")
    if args.release and args.keymap not in KEYMAP:
        raise Refused(f"RELEASE images are release keymaps only ({', '.join(KEYMAP)})")
    if not 0 <= args.key_id < KEY_COUNT:
        raise Refused(f"--key-id must be 0..{KEY_COUNT - 1}")

    seed = read_key(args.key)
    pub = public_key(seed, args.signer)
    is_test_key = pub == test_public_key()
    if args.key_id == KEY_TEST and not is_test_key:
        raise Refused("key_id 0 is the test key; the firmware would refuse this signature")
    if args.key_id != KEY_TEST and is_test_key:
        raise Refused("the TEST-ONLY key must not sign as a release key_id")
    if args.key_id == KEY_TEST and args.release:
        raise Refused("the TEST-ONLY key never signs a RELEASE image")

    problems = image_problems(image)
    if problems:
        if not (args.unsafe_allow_bad_image and args.key_id == KEY_TEST):
            raise Refused("; ".join(problems))
        print("warning: building a structurally bad TEST image: " + "; ".join(problems), file=sys.stderr)

    manifest = build_manifest(image, key_id=args.key_id, hand=hand, pointing_id=pointing_id,
                              keymap_id=args.keymap_id if args.keymap_id is not None else KEYMAP.get(args.keymap, 0),
                              flags=flags, epoch=args.epoch, storage_format=args.storage_format,
                              fw_version=args.fw_version, version=args.version_string)
    sig = sign(seed, manifest, args.signer)
    out = Path(args.output)
    out.write_bytes(manifest + sig + image)
    m = parse_manifest(manifest)
    print(f"{out}: {len(image)} B image, key_id {args.key_id}, hand {hand}, pointing {pointing_id}, "
          f"flags {flags:#x}, version {args.version_string!r} ({args.fw_version}), "
          f"manifest sha512 {hashlib.sha512(manifest).hexdigest()[:8]}, image sha512 {m['sha512'].hex()[:16]}")


def genkey(path):
    p = Path(path)
    if p.exists():
        raise Refused(f"{p} exists; not overwriting a key")
    seed = os.urandom(32)
    p.write_text("# Ed25519 seed (32 bytes, hex). Keep private.\n" + seed.hex() + "\n")
    print(public_key(seed).hex())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input", nargs="?", help="QMK .uf2 (or raw .bin with --raw)")
    ap.add_argument("-o", "--output", help="output .svup")
    ap.add_argument("--raw", action="store_true", help="input is a raw image starting at 0x10000000")
    ap.add_argument("--kb", help="keyboard path, e.g. svalboard/trackball/pmw3389/right (sets hand and pointing)")
    ap.add_argument("--hand", choices=sorted(HAND))
    ap.add_argument("--pointing", choices=sorted(POINTING))
    ap.add_argument("--keymap", default="sval", help="keymap name (display only)")
    ap.add_argument("--keymap-id", type=int)
    ap.add_argument("--version-string", default="", help="display version, at most 16 ASCII characters")
    ap.add_argument("--fw-version", type=lambda s: int(s, 0), default=0, help="numeric version (u32)")
    ap.add_argument("--epoch", type=lambda s: int(s, 0), default=0, help="security epoch (u16)")
    ap.add_argument("--storage-format", type=int, default=STORAGE_FORMAT)
    ap.add_argument("--key", default=str(TEST_KEY_FILE), help="Ed25519 seed file (default: the TEST-ONLY key)")
    ap.add_argument("--key-id", type=int, default=KEY_TEST)
    ap.add_argument("--release", action="store_true", help="set flags.RELEASE (release keys only)")
    ap.add_argument("--diagnostic", action="store_true", help="set flags.DIAGNOSTIC (scanlab/keytest/host-bootloader images)")
    ap.add_argument("--signer", choices=("auto", "openssl", "pure"), default="auto")
    ap.add_argument("--unsafe-allow-bad-image", action="store_true",
                    help="TEST key only: sign an image that fails the structure checks (for rejection tests)")
    ap.add_argument("--genkey", metavar="PATH", help="write a new random seed file and print its public key")
    ap.add_argument("--print-public", action="store_true", help="print the --key file's public key as hex")
    args = ap.parse_args(argv)
    try:
        if args.genkey:
            genkey(args.genkey)
        elif args.print_public:
            print(public_key(read_key(args.key), args.signer).hex())
        else:
            if not args.input or not args.output:
                ap.error("input and --output are required")
            if not 0 <= args.fw_version <= 0xFFFFFFFF or not 0 <= args.epoch <= 0xFFFF or not 0 <= args.storage_format <= 0xFF:
                raise Refused("--fw-version, --epoch or --storage-format out of range")
            make(args)
    except Refused as e:
        print(f"refused: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
