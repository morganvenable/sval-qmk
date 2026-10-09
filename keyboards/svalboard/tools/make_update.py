#!/usr/bin/env python3
"""Build, sign and check Svalboard updates (.svup) from QMK .uf2 files.

A .svup is the 108-byte manifest, its 64-byte Ed25519 signature, then the raw
image the updater stages and copies to flash offset 0. The manifest layout and
the image rules match keyboards/svalboard/updater/update_manifest.h and
update_image.c; the plan is docs/updater-plan.md (M1, M3).

One step, for development (signs with the TEST-ONLY key, key_id 0, unless
--key and --key-id say otherwise):

    make_update.py IN.uf2 -o OUT.svup --kb svalboard/trackball/pmw3389/right \\
        --keymap sval [--version-string S] [--fw-version N]

Two steps, for release CI (M3, D29): the build job, which never sees a key,
writes the raw image and an unsigned manifest; the signing job signs them.

    make_update.py unsigned IN.uf2 --image-out X.img --manifest-out X.manifest \\
        --kb svalboard/right --keymap sval --release
    make_update.py sign --image X.img --manifest X.manifest -o X.svup \\
        --key-env SVAL_UPDATE_SIGNING_KEY --expect-sha512 HEX \\
        [--expect-version vLaunch3]

'sign' signs release images only and checks everything first: the image is
the one the release lint checked (--expect-sha512, from
check_release_artifacts.py), its hash and structure, RELEASE set and
DIAGNOSTIC clear, a release keymap (sval, blank), and the image's own
build-info record (a release updater build with no test key, test hooks,
keytest or host bootloader, with the manifest's version, side, pointing
device and keymap). Its key must be one of the two release keys in
updater/update_release_keys.h, and it sets the manifest's key_id to that slot.

    make_update.py verify FILE.svup [--build release|test|plain]

checks a .svup offline the way a build of that kind would before erasing
anything: the signature against the keys that build has, the manifest rules,
the hash and the image structure.

fw_version and the version string default to the image's build-info record
(SVAL_FW_VERSION, SVAL_FW_VERSION_STRING), so a manifest always says what the
firmware reports. Refuses, with exit status 2: a UF2 that is malformed, for
another chip family, not starting at 0x10000000, with gaps or overlaps, or
ending beyond 0x10160000; an image whose boot2 CRC, initial SP or reset vector
is wrong; and key, flag and field combinations the firmware would refuse
anyway. Uses the 'cryptography' package for Ed25519 when it is installed and a
pure-Python RFC 8032 implementation otherwise; both give identical signatures.
"""
import argparse
import hashlib
import os
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEST_KEY_FILE = HERE / "sval_update_TEST_ONLY.key"
RELEASE_KEYS_H = HERE.parent / "updater" / "update_release_keys.h"
# Public keys that must never be production release keys: the M3 DRY RUN keys,
# and any key retired later. Kept outside the header, so a key swap cannot
# drop them by accident (release_keys()).
RETIRED_KEYS = HERE / "retired_release_keys.txt"

# ---- format (update_manifest.h) ----------------------------------------------------

MANIFEST_MAGIC = 0x50555653  # "SVUP"
MANIFEST_VERSION = 1
UPDATER_PROTO = 1
MANIFEST_FMT = "<IHBBBBHHBBIII16s64s"
MANIFEST_BYTES = 108
SIG_BYTES = 64
assert struct.calcsize(MANIFEST_FMT) == MANIFEST_BYTES

KEY_TEST, KEY_COUNT = 0, 3
RELEASE_KEY_IDS = (1, 2)
HAND = {"left": 0, "right": 1}
POINTING = {"none": 0, "trackpoint": 1, "pmw3360": 2, "pmw3389": 3, "azoteq": 4}
KEYMAP = {"sval": 1, "blank": 2}  # the release keymaps (D22)
FLAG_RELEASE, FLAG_DIAGNOSTIC = 0x0001, 0x0002
STORAGE_FORMAT = 2  # SVAL_UPDATE_STORAGE_FORMAT in keyboards/svalboard/config.h

# ---- build info (update_keys.h) ---------------------------------------------------

BUILD_INFO_MAGIC = b"SVBI\x02"  # magic, then info_ver 2
BUILD_INFO_FMT = "<4sBBBBI16sBBBB"  # ..., version, hand, pointing_id, keymap_id, reserved
BUILD_INFO_BYTES = 32
BI_HOST_TEST = 0xFF  # hand and pointing_id of a host-test build
assert struct.calcsize(BUILD_INFO_FMT) == BUILD_INFO_BYTES
BI_RELEASE, BI_TEST_KEY, BI_TEST_HOOKS, BI_KEYTEST, BI_HOST_BOOTLOADER, BI_KEYS_DRY_RUN = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20
BI_NOT_IN_RELEASE = {BI_TEST_KEY: "the TEST-ONLY key", BI_TEST_HOOKS: "test hooks", BI_KEYTEST: "SVAL_KEYTEST",
                     BI_HOST_BOOTLOADER: "SVAL_HOST_BOOTLOADER"}
VERSION_RE = re.compile(r"^[A-Za-z0-9._+-]{0,16}$")

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


def build_info(image):
    """The image's build-info record (update_keys.h) as a dict, or None if it has none
    (a build without the updater, or from before M3)."""
    hits = []
    at = image.find(BUILD_INFO_MAGIC)
    while at >= 0:
        hits.append(at)
        at = image.find(BUILD_INFO_MAGIC, at + 1)
    if not hits:
        if image.find(b"SVBI\x01") >= 0:
            raise Refused("the image has a version-1 build-info record (an M3 build from before 2026-10-09): rebuild it")
        return None
    if len(hits) > 1:
        raise Refused(f"the image has {len(hits)} build-info records (SVBI), expected one")
    if hits[0] + BUILD_INFO_BYTES > len(image):
        raise Refused("the build-info record runs past the end of the image")
    _, ver, flags, release_keys, proto, fw, version, hand, pointing_id, keymap_id, reserved = struct.unpack_from(
        BUILD_INFO_FMT, image, hits[0])
    text = version.rstrip(b"\0")
    if b"\0" in text or not VERSION_RE.match(text.decode("latin-1")):
        raise Refused(f"the build-info record's version string {version!r} is malformed")
    return dict(offset=hits[0], flags=flags, release_keys=release_keys, updater_proto=proto, fw_version=fw,
                version=text.decode("ascii"), hand=hand, pointing_id=pointing_id, keymap_id=keymap_id,
                reserved=reserved)


def build_info_identity_problems(info, m):
    """Where the manifest's hand, pointing device and keymap disagree with the image's
    build-info record (a record from a host-test build, 0xFF, says nothing)."""
    problems = []
    for field in ("hand", "pointing_id", "keymap_id"):
        if info[field] != BI_HOST_TEST and info[field] != m[field]:
            problems.append(f"the manifest says {field} {m[field]}, the image was built with {info[field]}")
    return problems


# ---- Ed25519 ---------------------------------------------------------------------------
# Pure-Python signer and verifier, following RFC 8032 section 5.1 (extended
# coordinates). Not constant time: fine for a build tool on its own machine.

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


def _decompress(b):
    """A point from its 32-byte encoding, or None if it is not one (RFC 8032 5.1.3)."""
    y = int.from_bytes(b, "little")
    sign, y = y >> 255, y & ((1 << 255) - 1)
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    if x2 == 0:
        if sign:
            return None
        return (0, y, 1, 0)
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return None
    if x & 1 != sign:
        x = _P - x
    return (x, y, 1, x * y % _P)


def _same(a, b):
    return (a[0] * b[2] - b[0] * a[2]) % _P == 0 and (a[1] * b[2] - b[1] * a[2]) % _P == 0


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


def pure_verify(pub, msg, sig):
    """[S]B == R + [k]A, with S < L and both points canonical (as Monocypher)."""
    if len(pub) != 32 or len(sig) != 64:
        return False
    big_a, big_r = _decompress(pub), _decompress(sig[:32])
    s = int.from_bytes(sig[32:], "little")
    if big_a is None or big_r is None or s >= _L:
        return False
    return _same(_mul(s, _G), _add(big_r, _mul(_hint(sig[:32], pub, msg), big_a)))


def _openssl():
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
        from cryptography.hazmat.primitives import serialization
        from cryptography.exceptions import InvalidSignature
        return Ed25519PrivateKey, serialization, Ed25519PublicKey, InvalidSignature
    except ImportError:
        return None


def _lib(signer):
    lib = _openssl() if signer in ("auto", "openssl") else None
    if signer == "openssl" and not lib:
        raise Refused("--signer openssl needs the 'cryptography' package")
    return lib


def public_key(seed, signer="auto"):
    lib = _lib(signer)
    if lib:
        return lib[0].from_private_bytes(seed).public_key().public_bytes(lib[1].Encoding.Raw, lib[1].PublicFormat.Raw)
    return pure_public(seed)


def sign(seed, msg, signer="auto"):
    lib = _lib(signer)
    if lib:
        return lib[0].from_private_bytes(seed).sign(msg)
    return pure_sign(seed, msg)


def verify(pub, msg, sig, signer="auto"):
    lib = _lib(signer)
    if lib:
        try:
            lib[2].from_public_bytes(pub).verify(sig, msg)
            return True
        except (lib[3], ValueError):
            return False
    return pure_verify(pub, msg, sig)


def parse_key(text, where):
    lines = [ln.strip() for ln in text.splitlines()]
    hexes = [ln for ln in lines if ln and not ln.startswith("#")]
    if len(hexes) != 1 or not re.fullmatch(r"[0-9a-fA-F]{64}", hexes[0]):
        raise Refused(f"{where}: expected one line of 64 hex digits (a 32-byte Ed25519 seed)")
    return bytes.fromhex(hexes[0])


def read_key(path):
    return parse_key(Path(path).read_text(), path)


def test_public_key():
    return pure_public(read_key(TEST_KEY_FILE))


def retired_keys(path=RETIRED_KEYS):
    """{public key: label} from retired_release_keys.txt: one key per line, 64 hex
    digits, then the rest of the line as its label; # starts a comment."""
    out = {}
    for n, line in enumerate(Path(path).read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        hexkey, _, label = line.partition(" ")
        if not re.fullmatch(r"[0-9a-fA-F]{64}", hexkey):
            raise Refused(f"{path}:{n}: expected 64 hex digits, then a label")
        out[bytes.fromhex(hexkey)] = label.strip() or hexkey
    return out


def release_keys(path=RELEASE_KEYS_H, retired=RETIRED_KEYS):
    """({1: pub, 2: pub}, dry_run) from updater/update_release_keys.h.

    Refuses a header that says it holds production keys (DRY_RUN 0) while either
    slot holds a retired key, such as a DRY RUN key a half-done swap left
    behind. Every release tool reads the keys through here, so release CI's
    version check, the ELF lint and the signer all refuse it."""
    text = Path(path).read_text()
    keys = {}
    for key_id in RELEASE_KEY_IDS:
        m = re.search(rf"#define\s+SVAL_UPDATE_RELEASE_KEY_{key_id}\b[^{{]*\{{([^}}]*)\}}", text)
        raw = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})\b", m.group(1))) if m else b""
        if len(raw) != 32:
            raise Refused(f"{path}: SVAL_UPDATE_RELEASE_KEY_{key_id} is not 32 bytes")
        keys[key_id] = raw
    dry = re.search(r"#define\s+SVAL_UPDATE_RELEASE_KEYS_DRY_RUN\s+([01])\b", text)
    if not dry:
        raise Refused(f"{path}: no SVAL_UPDATE_RELEASE_KEYS_DRY_RUN 0 or 1")
    if keys[1] == keys[2]:
        raise Refused(f"{path}: the two release keys are the same")
    dry_run = dry.group(1) == "1"
    if not dry_run:
        old = retired_keys(retired)
        bad = [f"key_id {k} is the retired key {old[v]}" for k, v in keys.items() if v in old]
        if bad:
            raise Refused(f"{path}: SVAL_UPDATE_RELEASE_KEYS_DRY_RUN is 0 but " + "; ".join(bad) +
                          f" ({Path(retired).name}): replace both keys (docs/updater.md, 'Release signing')")
    return keys, dry_run


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


def manifest_version(m):
    return m["version"].rstrip(b"\0").decode("latin-1")


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


def versions_for(image, args):
    """fw_version and version string for the manifest: the image's build-info record
    unless given. A given value that contradicts a set record is refused: the
    firmware reports the record's (INFO, split presence)."""
    info = build_info(image)
    fw, text = args.fw_version, args.version_string
    if info:
        if fw is not None and info["fw_version"] and fw != info["fw_version"]:
            raise Refused(f"--fw-version {fw} but the image reports {info['fw_version']} (SVAL_FW_VERSION)")
        if text is not None and info["version"] and text != info["version"]:
            raise Refused(f"--version-string {text!r} but the image reports {info['version']!r} (SVAL_FW_VERSION_STRING)")
        fw = info["fw_version"] if fw is None else fw
        text = info["version"] if text is None else text
    return (0 if fw is None else fw), ("" if text is None else text)


def manifest_from_args(image, args, key_id):
    """The manifest for IN and the common options (one-step and 'unsigned')."""
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
    if not 0 <= key_id < KEY_COUNT:
        raise Refused(f"--key-id must be 0..{KEY_COUNT - 1}")
    if key_id == KEY_TEST and args.release:
        raise Refused("the TEST-ONLY key never signs a RELEASE image")
    fw_version, version = versions_for(image, args)
    if not 0 <= fw_version <= 0xFFFFFFFF or not 0 <= args.epoch <= 0xFFFF or not 0 <= args.storage_format <= 0xFF:
        raise Refused("--fw-version, --epoch or --storage-format out of range")
    return build_manifest(image, key_id=key_id, hand=hand, pointing_id=pointing_id,
                          keymap_id=args.keymap_id if args.keymap_id is not None else KEYMAP.get(args.keymap, 0),
                          flags=flags, epoch=args.epoch, storage_format=args.storage_format,
                          fw_version=fw_version, version=version)


def read_image(args):
    data = Path(args.input).read_bytes()
    image = data if args.raw else uf2_to_image(data)
    if args.raw and (len(image) > MAX_IMAGE):
        raise Refused(f"image is {len(image):#x} bytes, above {MAX_IMAGE:#x}")
    return image


def check_structure(image, allow_bad):
    problems = image_problems(image)
    if problems:
        if not allow_bad:
            raise Refused("; ".join(problems))
        print("warning: building a structurally bad TEST image: " + "; ".join(problems), file=sys.stderr)


def describe(out, manifest, image):
    m = parse_manifest(manifest)
    return (f"{out}: {len(image)} B image, key_id {m['key_id']}, hand {m['hand']}, pointing {m['pointing_id']}, "
            f"flags {m['flags']:#x}, version {manifest_version(m)!r} ({m['fw_version']}), "
            f"manifest sha512 {hashlib.sha512(manifest).hexdigest()[:8]}, image sha512 {m['sha512'].hex()[:16]}")


# ---- one step: make --------------------------------------------------------------------


def make(args):
    if not 0 <= args.key_id < KEY_COUNT:
        raise Refused(f"--key-id must be 0..{KEY_COUNT - 1}")
    image = read_image(args)
    seed = read_key(args.key)
    pub = public_key(seed, args.signer)
    is_test_key = pub == test_public_key()
    if args.key_id == KEY_TEST and not is_test_key:
        raise Refused("key_id 0 is the test key; the firmware would refuse this signature")
    if args.key_id != KEY_TEST and is_test_key:
        raise Refused("the TEST-ONLY key must not sign as a release key_id")
    manifest = manifest_from_args(image, args, args.key_id)
    check_structure(image, args.unsafe_allow_bad_image and args.key_id == KEY_TEST)
    if args.key_id in RELEASE_KEY_IDS and release_keys()[0][args.key_id] != pub:
        print(f"warning: this key is not release key {args.key_id} in {RELEASE_KEYS_H.name}: no firmware accepts it",
              file=sys.stderr)
    sig = sign(seed, manifest, args.signer)
    out = Path(args.output)
    out.write_bytes(manifest + sig + image)
    print(describe(out, manifest, image))


# ---- two steps: unsigned, then sign --------------------------------------------------------


def make_unsigned(args):
    """The build job's half (M3): raw image and unsigned manifest, no key."""
    image = read_image(args)
    manifest = manifest_from_args(image, args, args.key_id)
    check_structure(image, False)
    info = build_info(image)
    if info:
        problems = build_info_identity_problems(info, parse_manifest(manifest))
        if problems:
            raise Refused("; ".join(problems) + " (--kb or --keymap is not this image's)")
    Path(args.image_out).write_bytes(image)
    Path(args.manifest_out).write_bytes(manifest)
    print(describe(args.manifest_out, manifest, image) + " (unsigned)")


def release_image_problems(manifest, image, keys, dry_run):
    """Why a manifest and image must not be signed as a release, or [] (the signing job's checks)."""
    m = parse_manifest(manifest)
    problems = []
    if m["magic"] != MANIFEST_MAGIC or m["manifest_ver"] != MANIFEST_VERSION or m["reserved"]:
        problems.append("not a version-1 manifest (magic, version or reserved field)")
    if m["flags"] != FLAG_RELEASE:
        problems.append(f"flags {m['flags']:#x}: a release image has RELEASE set and DIAGNOSTIC (and all else) clear")
    if m["keymap_id"] not in KEYMAP.values():
        problems.append(f"keymap_id {m['keymap_id']} is not a release keymap ({', '.join(KEYMAP)})")
    if m["hand"] not in HAND.values() or m["pointing_id"] not in POINTING.values():
        problems.append(f"hand {m['hand']} or pointing_id {m['pointing_id']} is unknown")
    if m["updater_proto"] != UPDATER_PROTO or m["storage_format"] < STORAGE_FORMAT:
        problems.append(f"updater_proto {m['updater_proto']} or storage_format {m['storage_format']} is not this release's")
    if m["image_len"] != len(image):
        problems.append(f"image_len {m['image_len']} but the image is {len(image)} B")
    elif hashlib.sha512(image).digest() != m["sha512"]:
        problems.append("the image's SHA-512 does not match the manifest")
    problems += image_problems(image)
    try:
        info = build_info(image)
    except Refused as e:
        info, problems = None, problems + [str(e)]
    if info is None:
        problems.append("the image has no build-info record: not an updater build")
    else:
        if not info["flags"] & BI_RELEASE:
            problems.append("the image is not a release updater build (SVAL_UPDATE_RELEASE)")
        for bit, what in BI_NOT_IN_RELEASE.items():
            if info["flags"] & bit:
                problems.append(f"the image has {what}")
        if bool(info["flags"] & BI_KEYS_DRY_RUN) != dry_run:
            problems.append("the image's release keys are not the ones in update_release_keys.h (DRY RUN flag differs)")
        if info["release_keys"] != len(RELEASE_KEY_IDS):
            problems.append(f"the image has {info['release_keys']} release key slots, expected {len(RELEASE_KEY_IDS)}")
        if info["fw_version"] != m["fw_version"] or info["version"] != manifest_version(m):
            problems.append(f"the manifest says {manifest_version(m)!r} ({m['fw_version']}), the image reports "
                            f"{info['version']!r} ({info['fw_version']})")
        if BI_HOST_TEST in (info["hand"], info["pointing_id"]) or info["keymap_id"] not in KEYMAP.values():
            problems.append(f"the image's build-info record has hand {info['hand']}, pointing_id {info['pointing_id']}, "
                            f"keymap_id {info['keymap_id']}: not a release keymap build for one half")
        problems += build_info_identity_problems(info, m)
    for key_id, pub in keys.items():
        if image.count(pub) != 1:
            problems.append(f"release key {key_id} appears {image.count(pub)} times in the image, expected once")
    if test_public_key() in image:
        problems.append("the image contains the TEST-ONLY public key")
    return problems


def sign_release(args):
    """The signing job's half (M3, D29): sign a release manifest, refusing anything else."""
    if args.key_env:
        text = os.environ.get(args.key_env, "")
        if not text.strip():
            raise Refused(f"${args.key_env} is empty")
        seed = parse_key(text, f"${args.key_env}")
    else:
        seed = read_key(args.key)
    pub = public_key(seed, args.signer)
    if pub == test_public_key():
        raise Refused("the TEST-ONLY key never signs a release")
    keys, dry_run = release_keys(args.keys_header)
    slot = [k for k, v in keys.items() if v == pub]
    if not slot:
        raise Refused(f"this key is neither release key in {RELEASE_KEYS_H.name}: no release build would accept it")
    manifest = bytearray(Path(args.manifest).read_bytes())
    image = Path(args.image).read_bytes()
    if len(manifest) != MANIFEST_BYTES:
        raise Refused(f"{args.manifest}: {len(manifest)} B, a manifest is {MANIFEST_BYTES}")
    problems = release_image_problems(bytes(manifest), image, keys, dry_run)
    m = parse_manifest(bytes(manifest))
    # The image the lint checked (release CI passes the SHA-512 the lint job
    # computed from the ELF, the .uf2 and the .img as a job output).
    want = args.expect_sha512.strip().lower()
    if not re.fullmatch(r"[0-9a-f]{128}", want):
        problems.append("--expect-sha512 is not 128 hex digits")
    elif hashlib.sha512(image).hexdigest() != want:
        problems.append(f"the image's SHA-512 is {hashlib.sha512(image).hexdigest()[:16]}..., "
                        f"not the linted image's {want[:16]}...")
    if args.expect_version is not None and manifest_version(m) != args.expect_version:
        problems.append(f"version {manifest_version(m)!r}, expected {args.expect_version!r}")
    if args.expect_fw_version is not None and m["fw_version"] != args.expect_fw_version:
        problems.append(f"fw_version {m['fw_version']}, expected {args.expect_fw_version}")
    if problems:
        raise Refused("will not sign: " + "; ".join(problems))
    manifest[6] = slot[0]  # key_id: the slot this key fills
    manifest = bytes(manifest)
    sig = sign(seed, manifest, args.signer)
    if not verify(pub, manifest, sig, args.signer):
        raise Refused("the signature does not verify")
    out = Path(args.output)
    out.write_bytes(manifest + sig + image)
    print(describe(out, manifest, image) + (" [DRY RUN key]" if dry_run else ""))


# ---- verify ----------------------------------------------------------------------------------


def verify_svup(args):
    """A .svup checked as a build of the given kind would before it erases anything."""
    manifest, sig, image = parse_svup(Path(args.svup).read_bytes())
    m = parse_manifest(manifest)
    keys, dry_run = release_keys(args.keys_header)
    if args.build == "test":
        keys = {**keys, KEY_TEST: test_public_key()}
    release_build = args.build == "release"
    problems = []
    if m["magic"] != MANIFEST_MAGIC or m["manifest_ver"] != MANIFEST_VERSION or m["reserved"] or m["flags"] & ~(FLAG_RELEASE | FLAG_DIAGNOSTIC):
        problems.append("INVALID: not a version-1 manifest")
    if m["key_id"] == KEY_TEST and (m["flags"] & FLAG_RELEASE or release_build):
        problems.append("BAD_SIG: the test key on a RELEASE image or a release build")
    if release_build and m["flags"] & FLAG_DIAGNOSTIC:
        problems.append("BAD_IMAGE: a DIAGNOSTIC image on a release build")
    pub = keys.get(m["key_id"])
    if pub is None:
        problems.append(f"BAD_SIG: a {args.build} build has no key_id {m['key_id']}")
    elif not verify(pub, manifest, sig, args.signer):
        problems.append(f"BAD_SIG: the signature does not verify with key_id {m['key_id']}")
    if hashlib.sha512(image).digest() != m["sha512"]:
        problems.append("BAD_HASH: the image does not match the manifest's SHA-512")
    problems += ["BAD_IMAGE: " + p for p in image_problems(image)]
    info = build_info(image)
    what = (f"key_id {m['key_id']}{' (DRY RUN)' if m['key_id'] in RELEASE_KEY_IDS and dry_run else ''}, "
            f"hand {m['hand']}, pointing {m['pointing_id']}, flags {m['flags']:#x}, "
            f"version {manifest_version(m)!r} ({m['fw_version']}), image {len(image)} B, "
            f"build info {'none' if info is None else hex(info['flags'])}")
    if problems:
        raise Refused(f"{args.svup}: a {args.build} build refuses it ({what}): " + "; ".join(problems))
    print(f"{args.svup}: a {args.build} build accepts it ({what})")


# ---- command line ---------------------------------------------------------------------------


def add_image_options(ap, input_required=True):
    ap.add_argument("input", nargs=None if input_required else "?", help="QMK .uf2 (or raw .bin with --raw)")
    ap.add_argument("--raw", action="store_true", help="input is a raw image starting at 0x10000000")
    ap.add_argument("--kb", help="keyboard path, e.g. svalboard/trackball/pmw3389/right (sets hand and pointing)")
    ap.add_argument("--hand", choices=sorted(HAND))
    ap.add_argument("--pointing", choices=sorted(POINTING))
    ap.add_argument("--keymap", default="sval", help="keymap name (display only)")
    ap.add_argument("--keymap-id", type=int)
    ap.add_argument("--version-string", help="display version, at most 16 ASCII characters "
                    "(default: the image's SVAL_FW_VERSION_STRING)")
    ap.add_argument("--fw-version", type=lambda s: int(s, 0), help="numeric version, u32 (default: the image's SVAL_FW_VERSION)")
    ap.add_argument("--epoch", type=lambda s: int(s, 0), default=0, help="security epoch (u16)")
    ap.add_argument("--storage-format", type=int, default=STORAGE_FORMAT)
    ap.add_argument("--release", action="store_true", help="set flags.RELEASE (release keys only)")
    ap.add_argument("--diagnostic", action="store_true", help="set flags.DIAGNOSTIC (scanlab/keytest/host-bootloader images)")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    signer_help = "Ed25519 implementation: 'cryptography' when installed (auto), or the pure-Python one"
    try:
        if argv and argv[0] == "unsigned":
            ap = argparse.ArgumentParser(prog="make_update.py unsigned", description=make_unsigned.__doc__)
            add_image_options(ap)
            ap.add_argument("--image-out", required=True, help="raw image to write")
            ap.add_argument("--manifest-out", required=True, help="unsigned 108-byte manifest to write")
            ap.add_argument("--key-id", type=int, default=1, help="key_id for the manifest (the signer sets its own)")
            make_unsigned(ap.parse_args(argv[1:]))
        elif argv and argv[0] == "sign":
            ap = argparse.ArgumentParser(prog="make_update.py sign", description=sign_release.__doc__)
            ap.add_argument("--image", required=True)
            ap.add_argument("--manifest", required=True)
            ap.add_argument("-o", "--output", required=True, help="output .svup")
            key = ap.add_mutually_exclusive_group(required=True)
            key.add_argument("--key", help="Ed25519 seed file")
            key.add_argument("--key-env", metavar="VAR", help="environment variable holding the seed (64 hex digits)")
            ap.add_argument("--expect-version", help="refuse unless the manifest's version string is this (the tag)")
            ap.add_argument("--expect-fw-version", type=int, help="refuse unless the manifest's fw_version is this")
            ap.add_argument("--expect-sha512", required=True, metavar="HEX",
                            help="refuse unless the image's SHA-512 is this: the image the release lint checked "
                                 "(check_release_artifacts.py prints it)")
            ap.add_argument("--signer", choices=("auto", "openssl", "pure"), default="auto", help=signer_help)
            ap.add_argument("--keys-header", default=str(RELEASE_KEYS_H), help="release keys header (tests only)")
            sign_release(ap.parse_args(argv[1:]))
        elif argv and argv[0] == "verify":
            ap = argparse.ArgumentParser(prog="make_update.py verify", description=verify_svup.__doc__)
            ap.add_argument("svup")
            ap.add_argument("--build", choices=("release", "test", "plain"), default="release",
                            help="release: SVAL_UPDATE_RELEASE (keys 1, 2); test: SVAL_UPDATE_TEST_KEY (keys 0, 1, 2); "
                                 "plain: SVAL_UPDATER alone (keys 1, 2)")
            ap.add_argument("--signer", choices=("auto", "openssl", "pure"), default="auto", help=signer_help)
            ap.add_argument("--keys-header", default=str(RELEASE_KEYS_H), help="release keys header (tests only)")
            verify_svup(ap.parse_args(argv[1:]))
        else:
            ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
            add_image_options(ap, input_required=False)
            ap.add_argument("-o", "--output", help="output .svup")
            ap.add_argument("--key", default=str(TEST_KEY_FILE), help="Ed25519 seed file (default: the TEST-ONLY key)")
            ap.add_argument("--key-id", type=int, default=KEY_TEST)
            ap.add_argument("--signer", choices=("auto", "openssl", "pure"), default="auto", help=signer_help)
            ap.add_argument("--unsafe-allow-bad-image", action="store_true",
                            help="TEST key only: sign an image that fails the structure checks (for rejection tests)")
            ap.add_argument("--genkey", metavar="PATH", help="write a new random seed file and print its public key")
            ap.add_argument("--print-public", action="store_true", help="print the --key file's public key as hex")
            ap.add_argument("--print-release-keys", action="store_true",
                            help="print the two release public keys from update_release_keys.h, one per line")
            args = ap.parse_args(argv)
            if args.genkey:
                genkey(args.genkey)
            elif args.print_public:
                print(public_key(read_key(args.key), args.signer).hex())
            elif args.print_release_keys:
                keys, _ = release_keys()
                print("\n".join(keys[k].hex() for k in RELEASE_KEY_IDS))
            else:
                if not args.input or not args.output:
                    ap.error("input and --output are required")
                make(args)
    except Refused as e:
        print(f"refused: {e}", file=sys.stderr)
        return 2
    return 0


def genkey(path):
    p = Path(path)
    if p.exists():
        raise Refused(f"{p} exists; not overwriting a key")
    seed = os.urandom(32)
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write("# Ed25519 seed (32 bytes, hex). Keep private.\n" + seed.hex() + "\n")
    print(public_key(seed).hex())


if __name__ == "__main__":
    sys.exit(main())
