#!/usr/bin/env python3
"""Host tests for keyboards/svalboard/tools/make_update.py.

    test_make_update.py OUT_DIR [REAL.uf2 ...]

Checks the refusals, the manifest it builds, CRC and Ed25519 vectors (the
pure-Python signer against RFC 8032 and, when installed, the 'cryptography'
package), and writes signed .svup files plus OUT_DIR/index.txt for the C test
(test_updater) to cross-check with Monocypher. REAL.uf2 files, if given, are
QMK builds named like svalboard_trackball_pmw3389_right_sval.uf2.
"""
import hashlib
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "keyboards/svalboard/tools/make_update.py"
spec = importlib.util.spec_from_file_location("make_update", TOOL)
mu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mu)

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


BOOT2 = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-f]{2})", (Path(__file__).parent / "boot2_page0.h").read_text().split("#define BOOT2_PAGE0_BYTES")[1]))
assert len(BOOT2) == 256


def image(length=0x400, sp=0x20041000, reset=0x100001F7, page0=BOOT2):
    body = bytearray(page0 + struct.pack("<II", sp, reset))
    body += bytes((i * 13 + 5) & 0xFF for i in range(length - len(body)))
    return bytes(body)


def uf2(img, family=mu.RP2040_FAMILY, flags=mu.UF2_FLAG_FAMILY, size=256, addr=None, number=None, magic=mu.UF2_MAGIC0):
    n = len(img) // 256
    out = bytearray()
    for i in range(n):
        a = mu.XIP_BASE + 256 * i if addr is None else addr(i)
        b, total = (i, n) if number is None else number(i, n)
        out += struct.pack("<8I", magic, mu.UF2_MAGIC1, flags, a, size, b, total, family)
        out += img[i * 256:(i + 1) * 256].ljust(476, b"\0")
        out += struct.pack("<I", mu.UF2_MAGIC_END)
    return bytes(out)


def run(*args):
    p = subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    reals = [Path(a) for a in sys.argv[2:]]
    index = []
    tmp = Path(tempfile.mkdtemp())

    # ---- vectors -------------------------------------------------------------------
    check(mu.crc32_mpeg2(b"123456789") == 0x0376E6E7, "CRC-32/MPEG-2 check value")
    check(mu.crc32_mpeg2(BOOT2[:252]) == 0xD58F0B07, "real page 0 CRC d58f0b07")
    check(mu.boot2_valid(BOOT2), "real page 0 valid")
    check(mu.crc32_mpeg2(bytes(252)) == 0x7065399A and not mu.boot2_valid(bytes(256)), "zero page 7065399a, invalid")
    check(mu.crc32_mpeg2(b"\xff" * 252) == 0x0B8FD31A and not mu.boot2_valid(b"\xff" * 256), "erased page 0b8fd31a, invalid")

    rfc8032 = [  # RFC 8032 section 7.1, tests 1-3: seed, public key, message, signature
        ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
        ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
        ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
    ]
    have_openssl = mu._openssl() is not None
    for seed, pub, msg, sig in rfc8032:
        seed, pub, msg, sig = map(bytes.fromhex, (seed, pub, msg, sig))
        check(mu.pure_public(seed) == pub, "pure signer: RFC 8032 public key")
        check(mu.pure_sign(seed, msg) == sig, "pure signer: RFC 8032 signature")
        if have_openssl:
            check(mu.sign(seed, msg, "openssl") == sig, "cryptography: RFC 8032 signature")
    test_seed = mu.read_key(mu.TEST_KEY_FILE)
    if have_openssl:
        for n in (0, 1, 108, 1000):
            m = bytes(range(256)) * 4
            check(mu.sign(test_seed, m[:n], "pure") == mu.sign(test_seed, m[:n], "openssl"), f"pure == cryptography, {n} B")
    else:
        print("note: 'cryptography' not installed; the pure signer is checked against RFC 8032 only")
    (out / "test_pubkey.hex").write_text(mu.test_public_key().hex() + "\n")
    # The release keys as make_update.py reads them from update_release_keys.h;
    # test_updater checks the compiler saw the same bytes.
    rel_keys, _ = mu.release_keys()
    (out / "release_pubkeys.hex").write_text("".join(rel_keys[k].hex() + "\n" for k in mu.RELEASE_KEY_IDS))
    check(mu.test_public_key() not in rel_keys.values(), "the test key is not a release key")

    # Verification (the pure verifier, and 'cryptography' when installed).
    for seed, pub, msg, sig in rfc8032:
        seed, pub, msg, sig = map(bytes.fromhex, (seed, pub, msg, sig))
        check(mu.pure_verify(pub, msg, sig), "pure verifier: RFC 8032 signature")
        bad = bytearray(sig)
        bad[0] ^= 1
        check(not mu.pure_verify(pub, msg, bytes(bad)), "pure verifier: flipped R")
        bad = bytearray(sig)
        bad[40] ^= 1
        check(not mu.pure_verify(pub, msg, bytes(bad)), "pure verifier: flipped S")
        check(not mu.pure_verify(pub, msg + b"x", sig), "pure verifier: another message")
        s_plus_l = (int.from_bytes(sig[32:], "little") + mu._L).to_bytes(32, "little")
        check(not mu.pure_verify(pub, msg, sig[:32] + s_plus_l), "pure verifier: S >= L")
        if have_openssl:
            check(mu.verify(pub, msg, sig, "openssl") and not mu.verify(pub, msg + b"x", sig, "openssl"), "cryptography verify")

    # ---- a good build --------------------------------------------------------------------
    img = image(0x1800)
    good = tmp / "good.uf2"
    good.write_bytes(uf2(img))
    common = ["--kb", "svalboard/trackball/pmw3389/right", "--version-string", "M1-test-A", "--fw-version", "0x010203"]

    def make(name, *args, expect="ok", src=good):
        rc, text = run(src, "-o", out / name, *args)
        check(rc == 0, f"{name}: built ({text.strip()})")
        if rc == 0:
            index.append((name, expect))
        return out / name

    p = make("ok_basic.svup", *common)
    data = p.read_bytes()
    manifest, sig, body = mu.parse_svup(data)
    f = mu.parse_manifest(manifest)
    check(body == img, "image bytes are the UF2 payload")
    check(f["magic"] == 0x50555653 and manifest[:4] == b"SVUP", "magic")
    check((f["manifest_ver"], f["key_id"], f["hand"], f["pointing_id"], f["keymap_id"]) == (1, 0, 1, 3, 1), f"fields {f}")
    check((f["flags"], f["security_epoch"], f["storage_format"], f["updater_proto"]) == (0, 0, 2, 1), f"fields {f}")
    check((f["image_len"], f["fw_version"], f["reserved"]) == (len(img), 0x010203, 0), f"fields {f}")
    check(f["version"] == b"M1-test-A".ljust(16, b"\0"), "version string")
    check(f["sha512"] == hashlib.sha512(img).digest(), "sha512")
    pure = make("ok_basic_pure.svup", *common, "--signer", "pure")
    check(pure.read_bytes() == data, "pure signer output is identical")

    make("ok_left_trackpoint.svup", "--kb", "svalboard/trackpoint/left", "--keymap", "blank", "--epoch", "7")
    make("ok_diagnostic.svup", *common, "--diagnostic", "--keymap", "scanlab")
    (tmp / "good.bin").write_bytes(img)
    make("ok_raw.svup", *common, "--raw", src=tmp / "good.bin")
    for kb, hand, pointing in (("svalboard/left", 0, 0), ("svalboard/azoteq/right", 1, 4), ("svalboard/trackball/pmw3360/left", 0, 2)):
        check(mu.kb_fields(kb) == (hand, pointing), f"kb_fields {kb}")

    # signed, but with flipped bytes: Monocypher must refuse these
    flipped = bytearray(data)
    flipped[108 + 5] ^= 0x01
    (out / "badsig_sigbyte.svup").write_bytes(flipped)
    index.append(("badsig_sigbyte.svup", "badsig"))
    flipped = bytearray(data)
    flipped[20] ^= 0x01  # fw_version
    (out / "badsig_manifest.svup").write_bytes(flipped)
    index.append(("badsig_manifest.svup", "badsig"))
    flipped = bytearray(data)
    flipped[6] = 1  # key_id 1: not in this build
    (out / "badsig_keyid.svup").write_bytes(flipped)
    index.append(("badsig_keyid.svup", "badsig"))

    # ---- refusals ------------------------------------------------------------------------
    def refuse(what, uf2_bytes=None, *args, needle=""):
        src = good
        if uf2_bytes is not None:
            src = tmp / "case.uf2"
            src.write_bytes(uf2_bytes)
        target = tmp / "refused.svup"
        if target.exists():
            target.unlink()
        rc, text = run(src, "-o", target, *(args or common))
        check(rc == 2 and "refused" in text and needle.lower() in text.lower() and not target.exists(), f"refuses {what}: rc {rc}: {text.strip()}")

    refuse("another family", uf2(img, family=0x68ED2B88), needle="family")
    refuse("no family flag", uf2(img, flags=0), needle="family")
    refuse("not-main-flash block", uf2(img, flags=mu.UF2_FLAG_FAMILY | 1), needle="not-main-flash")
    refuse("wrong base", uf2(img, addr=lambda i: 0x10000100 + 256 * i), needle="starts")
    refuse("base in RAM", uf2(img, addr=lambda i: 0x20000000 + 256 * i), needle="starts")
    refuse("a gap", uf2(img, addr=lambda i: mu.XIP_BASE + 256 * (i + (i >= 3))), needle="gap")
    refuse("an overlap", uf2(img, addr=lambda i: mu.XIP_BASE + 256 * (i - (i >= 3))), needle="gap")
    refuse("blocks out of order", uf2(img, addr=lambda i: mu.XIP_BASE + 256 * (i ^ 1)), needle="address")
    refuse("bad numbering", uf2(img, number=lambda i, n: (i, n + 1)), needle="numbered")
    refuse("payload size", uf2(img, size=252), needle="payload")
    refuse("bad magic", uf2(img, magic=0x12345678), needle="magic")
    refuse("truncated UF2", uf2(img)[:-100], needle="multiple of 512")
    refuse("empty UF2", b"", needle="multiple of 512")
    big = image(0x160100)
    refuse("end beyond 0x10160000", uf2(big), needle="beyond")
    refuse("bad boot2 CRC", uf2(image(0x1800, page0=BOOT2[:40] + bytes([BOOT2[40] ^ 0xFF]) + BOOT2[41:])), needle="boot2")
    refuse("SP below RAM", uf2(image(0x1800, sp=0x1FFFFFF0)), needle="SP")
    refuse("SP above RAM", uf2(image(0x1800, sp=0x20042004)), needle="SP")
    refuse("even reset vector", uf2(image(0x1800, reset=0x100001F6)), needle="reset")
    refuse("reset vector past the end", uf2(image(0x1800, reset=0x10001801)), needle="reset")
    refuse("reset vector in boot2", uf2(image(0x1800, reset=0x10000081)), needle="reset")
    refuse("test key signing RELEASE", None, *common, "--release", needle="RELEASE")
    refuse("test key as key_id 1", None, *common, "--key-id", "1", needle="TEST-ONLY")
    refuse("key_id 3", None, *common, "--key-id", "3", needle="key-id")
    other = tmp / "other.key"
    rc, _ = run("--genkey", other)
    check(rc == 0, "genkey")
    refuse("another key as key_id 0", None, *common, "--key", other, needle="test key")
    refuse("RELEASE and DIAGNOSTIC", None, *common, "--key", other, "--key-id", "1", "--release", "--diagnostic", needle="DIAGNOSTIC")
    refuse("RELEASE of a diagnostic keymap", None, *common, "--key", other, "--key-id", "1", "--release", "--keymap", "scanlab", needle="release keymaps")
    refuse("long version string", None, *common, "--version-string", "x" * 17, needle="16")
    refuse("unknown variant", None, "--kb", "svalboard/joystick/right", needle="variant")
    refuse("no side", None, "--version-string", "x", needle="--kb")
    refuse("bad image with a release key", uf2(image(0x1800, sp=0)), *common, "--key", other, "--key-id", "1", "--unsafe-allow-bad-image", needle="SP")
    # The escape hatch for rejection tests: test key only, and the device must still refuse it.
    bad = tmp / "bad_sp.uf2"
    bad.write_bytes(uf2(image(0x1800, sp=0x30000000)))
    make("badimage_sp.svup", *common, "--unsafe-allow-bad-image", expect="badimage", src=bad)
    bad = tmp / "bad_boot2.uf2"
    bad.write_bytes(uf2(image(0x1800, page0=bytes(256))))
    make("badimage_boot2.svup", *common, "--unsafe-allow-bad-image", expect="badimage", src=bad)
    # A release key may sign (structure checks pass); the firmware has no key 1 yet.
    make("badsig_releasekey.svup", *common, "--key", other, "--key-id", "1", "--release", expect="badsig")

    # ---- M3: unsigned, sign, verify ------------------------------------------------------
    # Release keys of our own in a header of our own (--keys-header), since the
    # real ones' private halves are not in the repository.
    seeds = {k: os.urandom(32) for k in mu.RELEASE_KEY_IDS}
    pubs = {k: mu.pure_public(s) for k, s in seeds.items()}
    key_files = {}
    for k, s in seeds.items():
        key_files[k] = tmp / f"release{k}.key"
        key_files[k].write_text("# test release key\n" + s.hex() + "\n")
    header = tmp / "keys.h"

    def write_header(dry):
        header.write_text(f"#define SVAL_UPDATE_RELEASE_KEYS_DRY_RUN {int(dry)}\n" + "".join(
            f"#define SVAL_UPDATE_RELEASE_KEY_{k} \\\n    {{ {', '.join(f'0x{b:02x}' for b in pubs[k])}, }}\n" for k in pubs))

    write_header(True)
    check(mu.release_keys(header) == (pubs, True), "release_keys() reads a header")

    def release_img(flags=mu.BI_RELEASE | mu.BI_KEYS_DRY_RUN, fw=3, version=b"vM3-test", keys=None, extra=b""):
        body = bytearray(image(0x1800))
        rec = struct.pack(mu.BUILD_INFO_FMT, b"SVBI", 1, flags, 2, 1, fw, version.ljust(16, b"\0"))
        body[0x800:0x800 + len(rec)] = rec
        for i, k in enumerate(sorted(pubs) if keys is None else keys):
            body[0x900 + 32 * i:0x920 + 32 * i] = pubs[k]
        body[0xA00:0xA00 + len(extra)] = extra
        return bytes(body)

    rel_common = ["--kb", "svalboard/right", "--keymap", "sval", "--release"]

    def unsigned(name, img, *args):
        src = tmp / f"{name}.uf2"
        src.write_bytes(uf2(img))
        im, mf = tmp / f"{name}.img", tmp / f"{name}.manifest"
        for p in (im, mf):
            if p.exists():
                p.unlink()
        rc, text = run("unsigned", src, "--image-out", im, "--manifest-out", mf, *(args or rel_common))
        return rc, text, im, mf

    rc, text, im, mf = unsigned("rel", release_img())
    check(rc == 0, f"unsigned: built ({text.strip()})")
    f = mu.parse_manifest(mf.read_bytes())
    check((f["key_id"], f["flags"], f["fw_version"], f["keymap_id"], f["hand"], f["pointing_id"]) == (1, 1, 3, 1, 1, 0),
          f"unsigned manifest fields {f}")
    check(mu.manifest_version(f) == "vM3-test" and im.read_bytes() == release_img(), "unsigned: version from the record, raw image")

    def sign(name, *args, im=im, mf=mf, env=None):
        target = tmp / f"{name}.svup"
        if target.exists():
            target.unlink()
        cmd = [sys.executable, "-I", str(TOOL), "sign", "--image", str(im), "--manifest", str(mf), "-o", str(target),
               "--keys-header", str(header), *map(str, args)]
        p = subprocess.run(cmd, capture_output=True, text=True, env=env)
        return p.returncode, p.stdout + p.stderr, target

    def verify(svup, build="release", hdr=header):
        return run("verify", svup, "--build", build, "--keys-header", hdr)

    for k in mu.RELEASE_KEY_IDS:
        rc, text, signed = sign(f"signed{k}", "--key", key_files[k], "--expect-version", "vM3-test", "--expect-fw-version", "3")
        check(rc == 0 and "DRY RUN" in text, f"sign with release key {k}: {text.strip()}")
        if rc:
            continue
        manifest, sig, body = mu.parse_svup(signed.read_bytes())
        g = mu.parse_manifest(manifest)
        check(g["key_id"] == k and body == release_img(), f"signed with key {k}: key_id {g['key_id']}")
        check(mu.pure_verify(pubs[k], manifest, sig), f"key {k} signature verifies")
        check(manifest[:6] + manifest[7:] == mf.read_bytes()[:6] + mf.read_bytes()[7:], "the signer changes only key_id")
        for build, want in (("release", 0), ("plain", 0), ("test", 0)):
            rc, text = verify(signed, build)
            check(rc == want, f"verify --build {build} of a key {k} image: rc {rc}: {text.strip()}")
        rc, text = run("verify", signed)  # the committed (DRY RUN) keys: not ours
        check(rc == 2 and "BAD_SIG" in text, f"verify against the committed keys refuses: {text.strip()}")
        index.append((signed.name, "badsig"))
        (out / signed.name).write_bytes(signed.read_bytes())

    env = dict(os.environ, SVAL_TEST_SIGNING_KEY=seeds[2].hex())
    rc, text, signed = sign("signed_env", "--key-env", "SVAL_TEST_SIGNING_KEY", env=env)
    check(rc == 0 and mu.parse_manifest(signed.read_bytes())["key_id"] == 2, f"sign --key-env: {text.strip()}")
    rc, text, _ = sign("signed_env_empty", "--key-env", "SVAL_NO_SUCH_VARIABLE")
    check(rc == 2 and "empty" in text, f"sign --key-env with an empty variable: {text.strip()}")

    # test-key images: accepted by a test build, refused by release and plain builds
    for build, want in (("test", 0), ("release", 2), ("plain", 2)):
        rc, text = verify(p, build)
        check(rc == want, f"verify --build {build} of a test-key image: rc {rc}: {text.strip()}")
    rc, text = verify(out / "ok_diagnostic.svup", "test")
    check(rc == 0, f"verify --build test of a DIAGNOSTIC test image: {text.strip()}")
    rc, text = verify(out / "badsig_sigbyte.svup", "test")
    check(rc == 2 and "BAD_SIG" in text, f"verify of a flipped signature: {text.strip()}")

    def refuse_sign(what, needle, *args, img=None, manifest_edit=None, image_edit=None, key=1):
        nonlocal_im, nonlocal_mf = im, mf
        if img is not None:
            rc0, text0, nonlocal_im, nonlocal_mf = unsigned("case", img)
            check(rc0 == 0, f"{what}: unsigned built ({text0.strip()})")
        if manifest_edit or image_edit:
            m2, i2 = bytearray(nonlocal_mf.read_bytes()), bytearray(nonlocal_im.read_bytes())
            if manifest_edit:
                manifest_edit(m2)
            if image_edit:
                image_edit(i2)
            nonlocal_mf, nonlocal_im = tmp / "edit.manifest", tmp / "edit.img"
            nonlocal_mf.write_bytes(m2)
            nonlocal_im.write_bytes(i2)
        keyarg = key_files[key] if isinstance(key, int) else key
        rc, text, target = sign("refused", "--key", keyarg, *args, im=nonlocal_im, mf=nonlocal_mf)
        check(rc == 2 and needle.lower() in text.lower() and not target.exists(), f"sign refuses {what}: rc {rc}: {text.strip()}")

    def set_flags(v):
        return lambda m: m.__setitem__(slice(10, 12), struct.pack("<H", v))

    refuse_sign("the test key", "TEST-ONLY", key=mu.TEST_KEY_FILE)
    refuse_sign("a key in neither slot", "neither release key", key=other)
    refuse_sign("a DIAGNOSTIC manifest", "DIAGNOSTIC", manifest_edit=set_flags(3))
    refuse_sign("a manifest without RELEASE", "RELEASE", manifest_edit=set_flags(0))
    refuse_sign("a scanlab keymap", "release keymap", manifest_edit=lambda m: m.__setitem__(9, 0))
    refuse_sign("an image that does not match its hash", "SHA-512", image_edit=lambda i: i.__setitem__(0x1000, i[0x1000] ^ 1))
    refuse_sign("a bad SP", "SP", image_edit=lambda i: i.__setitem__(slice(0x100, 0x104), b"\0\0\0\0"),
                manifest_edit=None)
    refuse_sign("an image without a build-info record", "no build-info", img=image(0x1800))
    refuse_sign("a test-key build", "TEST-ONLY key", img=release_img(flags=mu.BI_RELEASE | mu.BI_KEYS_DRY_RUN | mu.BI_TEST_KEY))
    refuse_sign("a test-hooks build", "test hooks", img=release_img(flags=mu.BI_RELEASE | mu.BI_KEYS_DRY_RUN | mu.BI_TEST_HOOKS))
    refuse_sign("a host-bootloader build", "HOST_BOOTLOADER", img=release_img(flags=mu.BI_RELEASE | mu.BI_KEYS_DRY_RUN | mu.BI_HOST_BOOTLOADER))
    refuse_sign("a keytest build", "KEYTEST", img=release_img(flags=mu.BI_RELEASE | mu.BI_KEYS_DRY_RUN | mu.BI_KEYTEST))
    refuse_sign("a non-release updater build", "not a release updater build", img=release_img(flags=mu.BI_KEYS_DRY_RUN))
    refuse_sign("a build with other keys (DRY RUN flag)", "DRY RUN", img=release_img(flags=mu.BI_RELEASE))
    refuse_sign("a build missing release key 2", "release key 2", img=release_img(keys=[1]))
    refuse_sign("a build with the test public key in it", "TEST-ONLY public key", img=release_img(extra=mu.test_public_key()))
    refuse_sign("the wrong tag", "expected 'vLaunch9'", "--expect-version", "vLaunch9")
    refuse_sign("the wrong number", "expected 4", "--expect-fw-version", "4")
    refuse_sign("a manifest version that is not the image's", "the image reports",
                manifest_edit=lambda m: m.__setitem__(slice(28, 44), b"vOther".ljust(16, b"\0")))
    write_header(False)
    refuse_sign("DRY RUN keys in the image, production keys in the header", "DRY RUN")
    write_header(True)

    rc, text, *_ = unsigned("fwmismatch", release_img(), *rel_common, "--fw-version", "9")
    check(rc == 2 and "reports 3" in text, f"unsigned refuses a --fw-version the image contradicts: {text.strip()}")
    rc, text, *_ = unsigned("vmismatch", release_img(), *rel_common, "--version-string", "vOther")
    check(rc == 2 and "reports 'vM3-test'" in text, f"unsigned refuses a --version-string the image contradicts: {text.strip()}")
    rc, text, *_ = unsigned("diag", release_img(), *rel_common, "--diagnostic")
    check(rc == 2 and "DIAGNOSTIC" in text, f"unsigned refuses RELEASE + DIAGNOSTIC: {text.strip()}")
    rc, text, *_ = unsigned("twoinfo", release_img(extra=b"SVBI\x01" + bytes(23)))
    check(rc == 2 and "build-info records" in text, f"unsigned refuses two build-info records: {text.strip()}")
    # the one-step path takes the record's version too
    rec_uf2 = tmp / "rec.uf2"
    rec_uf2.write_bytes(uf2(release_img(flags=mu.BI_TEST_KEY, fw=2002, version=b"m3-B")))
    q = make("ok_record_version.svup", "--kb", "svalboard/left", src=rec_uf2)
    g = mu.parse_manifest(q.read_bytes()[:mu.MANIFEST_BYTES])
    check((g["fw_version"], mu.manifest_version(g)) == (2002, "m3-B"), f"one step: version from the record {g}")

    # ---- real builds ----------------------------------------------------------------------
    for real in reals:
        m = re.match(r"(svalboard(?:_trackball_pmw33(?:60|89)|_trackpoint|_azoteq)?_(?:left|right))_(\w+)\.uf2$", real.name)
        check(m is not None, f"real UF2 name {real.name}")
        if not m:
            continue
        kb = m.group(1).replace("_", "/")
        name = f"ok_real_{real.stem}.svup"
        p = make(name, "--kb", kb, "--keymap", m.group(2), "--version-string", "real", src=real)
        if p.exists():
            _, _, body = mu.parse_svup(p.read_bytes())
            check(mu.crc32_mpeg2(body[:252]) == 0xD58F0B07, f"{real.name}: page 0 CRC d58f0b07")
            binf = real.with_suffix(".bin")
            if binf.exists():
                raw = binf.read_bytes()
                raw += b"\xff" * (-len(raw) % 256)
                check(body[:len(raw)] == raw and len(body) - len(raw) < 256, f"{real.name}: image equals the .bin")

    (out / "index.txt").write_text("".join(f"{n} {e}\n" for n, e in index))
    print(f"make_update.py: {checks} checks, {failures} failures, {len(index)} .svup files for the C cross-check")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
