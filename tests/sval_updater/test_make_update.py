#!/usr/bin/env python3
"""Host tests for keyboards/svalboard/tools/make_update.py.

    test_make_update.py OUT_DIR [REAL.uf2 ...]

Checks the refusals, the manifest it builds (format 2: a .svup is the
108-byte manifest, then the image), the release checks, the CRC vectors, and
writes .svup files plus OUT_DIR/index.txt for the C test (test_updater) to
cross-check. REAL.uf2 files, if given, are QMK builds named like
svalboard_trackball_pmw3389_right_sval.uf2.
"""
import hashlib
import importlib.util
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


def run(*args, answer=""):
    """The tool with stdin holding answer: empty is no answer (EOF) at its y/N question."""
    p = subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)], capture_output=True, text=True, input=answer)
    return p.returncode, p.stdout + p.stderr


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    reals = [Path(a) for a in sys.argv[2:]]
    index = []
    tmp = Path(tempfile.mkdtemp())

    # ---- vectors -------------------------------------------------------------------
    # The storage-format floor is kept in two places: the firmware's and the tool's must agree.
    cfg = (ROOT / "keyboards/svalboard/config.h").read_text()
    fw_fmt = re.search(r"^#define SVAL_UPDATE_STORAGE_FORMAT (\d+)", cfg, re.M)
    check(fw_fmt is not None and int(fw_fmt.group(1)) == mu.STORAGE_FORMAT, f"make_update.STORAGE_FORMAT {mu.STORAGE_FORMAT} == SVAL_UPDATE_STORAGE_FORMAT {fw_fmt and fw_fmt.group(1)}")
    check(mu.crc32_mpeg2(b"123456789") == 0x0376E6E7, "CRC-32/MPEG-2 check value")
    check(mu.crc32_mpeg2(BOOT2[:252]) == 0xD58F0B07, "real page 0 CRC d58f0b07")
    check(mu.boot2_valid(BOOT2), "real page 0 valid")
    check(mu.crc32_mpeg2(bytes(252)) == 0x7065399A and not mu.boot2_valid(bytes(256)), "zero page 7065399a, invalid")
    check(mu.crc32_mpeg2(b"\xff" * 252) == 0x0B8FD31A and not mu.boot2_valid(b"\xff" * 256), "erased page 0b8fd31a, invalid")
    check((mu.MANIFEST_VERSION, mu.UPDATER_PROTO, mu.MANIFEST_BYTES) == (2, 3, 108), "format 2, protocol 3: a 108-byte manifest")
    check(not any(hasattr(mu, n) for n in ("sign", "verify", "SIG_BYTES", "release_keys", "TEST_KEY_FILE")), "make_update.py has no signing left")

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
    # A .svup is the manifest, then the image: nothing between them.
    check(len(data) == 108 + len(img) and data[108:] == img, ".svup = 108-byte manifest + image")
    parsed = mu.parse_svup(data)
    check(len(parsed) == 2, "parse_svup returns (manifest, image)")
    manifest, body = parsed
    f = mu.parse_manifest(manifest)
    check(body == img and manifest == data[:108], "image bytes are the UF2 payload")
    check(f["magic"] == 0x50555653 and manifest[:4] == b"SVUP", "magic")
    check((f["manifest_ver"], f["reserved0"], f["hand"], f["pointing_id"], f["keymap_id"]) == (2, 0, 1, 3, 1), f"fields {f}")
    check((f["flags"], f["security_epoch"], f["storage_format"], f["updater_proto"]) == (0, 0, 2, 3), f"fields {f}")
    check((f["image_len"], f["fw_version"], f["reserved"]) == (len(img), 0x010203, 0), f"fields {f}")
    check(f["version"] == b"M1-test-A".ljust(16, b"\0"), "version string")
    check(f["sha512"] == hashlib.sha512(img).digest(), "sha512")
    # round trip: the manifest rebuilt from the parsed fields is byte for byte the same
    again = struct.pack(mu.MANIFEST_FMT, *(f[k] for k in ("magic", "manifest_ver", "reserved0", "hand", "pointing_id", "keymap_id", "flags", "security_epoch", "storage_format", "updater_proto", "image_len", "fw_version", "reserved", "version", "sha512")))
    check(again + body == data, ".svup round trip")
    again_path = tmp / "ok_basic_again.svup"
    rc, text = run(good, "-o", again_path, *common)
    check(rc == 0 and again_path.read_bytes() == data, "the same input makes the same .svup")

    make("ok_left_trackpoint.svup", "--kb", "svalboard/trackpoint/left", "--keymap", "blank", "--epoch", "7")
    make("ok_diagnostic.svup", *common, "--diagnostic", "--keymap", "scanlab")
    (tmp / "good.bin").write_bytes(img)
    make("ok_raw.svup", *common, "--raw", src=tmp / "good.bin")
    for kb, hand, pointing in (("svalboard/left", 0, 5), ("svalboard/azoteq/right", 1, 4), ("svalboard/trackball/pmw3360/left", 0, 2)):
        check(mu.kb_fields(kb) == (hand, pointing), f"kb_fields {kb}")

    # The image no longer matches the manifest's hash: the device refuses it at verify.
    flipped = bytearray(data)
    flipped[108 + 0x1000] ^= 0x01
    (out / "badhash_image.svup").write_bytes(flipped)
    index.append(("badhash_image.svup", "badhash"))
    # A version-1 .svup (manifest version 1, then a 64-byte signature): the device
    # refuses the manifest, and the tools refuse the file.
    v1m = bytearray(manifest)
    v1m[4:6] = struct.pack("<H", 1)
    v1m[6] = 1  # key_id 1
    v1 = bytes(v1m) + bytes(64) + img
    (out / "v1_manifest.svup").write_bytes(v1)
    index.append(("v1_manifest.svup", "v1"))
    try:
        mu.parse_svup(v1)
        check(False, "parse_svup refuses a version-1 .svup")
    except mu.Refused as e:
        check("version-1" in str(e), f"parse_svup refuses a version-1 .svup: {e}")
    rc, text = run("verify", out / "v1_manifest.svup", "--build", "plain")
    check(rc == 2 and "version-1" in text, f"verify refuses a version-1 .svup: {text.strip()}")
    try:
        mu.parse_svup(data + b"\0")
        check(False, "parse_svup refuses a length that is not the manifest's")
    except mu.Refused as e:
        check("length" in str(e), f"parse_svup refuses a length that is not the manifest's: {e}")

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
    refuse("RELEASE and DIAGNOSTIC", None, *common, "--release", "--diagnostic", needle="DIAGNOSTIC")
    refuse("RELEASE of a diagnostic keymap", None, *common, "--release", "--keymap", "scanlab", needle="release keymaps")
    refuse("RELEASE of an image without a build-info record", None, *common, "--release", "--keymap", "sval", needle="no build-info")
    refuse("long version string", None, *common, "--version-string", "x" * 17, needle="16")
    refuse("unknown variant", None, "--kb", "svalboard/joystick/right", needle="variant")
    refuse("no side", None, "--version-string", "x", needle="--kb")
    refuse("--expect-* without --release", None, *common, "--expect-version", "x", needle="--release")
    refuse("a bad image for release", uf2(image(0x1800, sp=0)), *common, "--release", "--keymap", "sval", "--unsafe-allow-bad-image", needle="never --release")
    for gone in (("--key", "x"), ("--key-id", "1"), ("--signer", "pure"), ("--genkey", "x"), ("--print-public",)):
        rc, text = run(good, "-o", tmp / "gone.svup", *common, *gone)
        check(rc != 0 and "unrecognized arguments" in text, f"{gone[0]} is gone: {text.strip()[-80:]}")
    for gone in ("sign", "unsigned"):
        rc, text = run(gone, "--image", "x")
        check(rc != 0, f"the '{gone}' subcommand is gone: {text.strip()[-80:]}")
    # The escape hatch for rejection tests: the device must still refuse these.
    bad = tmp / "bad_sp.uf2"
    bad.write_bytes(uf2(image(0x1800, sp=0x30000000)))
    make("badimage_sp.svup", *common, "--unsafe-allow-bad-image", expect="badimage", src=bad)
    bad = tmp / "bad_boot2.uf2"
    bad.write_bytes(uf2(image(0x1800, page0=bytes(256))))
    make("badimage_boot2.svup", *common, "--unsafe-allow-bad-image", expect="badimage", src=bad)

    # ---- the build-info record and --release ------------------------------------------------
    def release_img(flags=mu.BI_RELEASE, fw=3, version=b"vM3-test", extra=b"", hand=1, pointing_id=5, keymap_id=1, info_ver=3, proto=3, reserved0=0):
        body = bytearray(image(0x1800))
        rec = struct.pack(mu.BUILD_INFO_FMT, b"SVBI", info_ver, flags, reserved0, proto, fw, version.ljust(16, b"\0"), hand, pointing_id, keymap_id, 0)
        body[0x800:0x800 + len(rec)] = rec
        body[0xA00:0xA00 + len(extra)] = extra
        return bytes(body)

    rel_common = ["--kb", "svalboard/right", "--keymap", "sval", "--release"]

    def rel(name, img, *args):
        src = tmp / f"{name}.uf2"
        src.write_bytes(uf2(img))
        target = tmp / f"{name}.svup"
        if target.exists():
            target.unlink()
        rc, text = run(src, "-o", target, *(args or rel_common))
        return rc, text, target

    rc, text, r = rel("rel", release_img(), *rel_common, "--expect-version", "vM3-test", "--expect-fw-version", "3")
    check(rc == 0, f"--release: built ({text.strip()})")
    if rc == 0:
        rm, rb = mu.parse_svup(r.read_bytes())
        f = mu.parse_manifest(rm)
        check((f["reserved0"], f["flags"], f["fw_version"], f["keymap_id"], f["hand"], f["pointing_id"]) == (0, 1, 3, 1, 1, 5), f"release manifest fields {f}")
        check(mu.manifest_version(f) == "vM3-test" and rb == release_img(), "release: version from the record, raw image")
        for build in ("release", "plain"):
            rc, text = run("verify", r, "--build", build)
            check(rc == 0, f"verify --build {build} of a release image: {text.strip()}")
        (out / "ok_release.svup").write_bytes(r.read_bytes())
        index.append(("ok_release.svup", "ok"))
    rc, text = run("verify", p, "--build", "release")
    check(rc == 0, f"verify --build release of an unflagged image: {text.strip()}")
    rc, text = run("verify", out / "ok_diagnostic.svup", "--build", "release")
    check(rc == 2 and "DIAGNOSTIC" in text, f"verify --build release refuses DIAGNOSTIC: {text.strip()}")
    rc, text = run("verify", out / "ok_diagnostic.svup", "--build", "plain")
    check(rc == 0, f"verify --build plain takes DIAGNOSTIC: {text.strip()}")
    rc, text = run("verify", out / "badhash_image.svup", "--build", "plain")
    check(rc == 2 and "BAD_HASH" in text, f"verify refuses a changed image: {text.strip()}")
    nz = bytearray(data)
    nz[6] = 1
    (tmp / "reserved0.svup").write_bytes(nz)
    rc, text = run("verify", tmp / "reserved0.svup", "--build", "plain")
    check(rc == 2 and "INVALID" in text, f"verify refuses a nonzero reserved byte: {text.strip()}")

    def refuse_rel(what, needle, img=None, *args):
        rc, text, target = rel("refused", release_img() if img is None else img, *(args or rel_common))
        check(rc == 2 and needle.lower() in text.lower() and not target.exists(), f"--release refuses {what}: rc {rc}: {text.strip()}")

    refuse_rel("an image without a build-info record", "no build-info", image(0x1800))
    refuse_rel("a test-hooks build", "test hooks", release_img(flags=mu.BI_RELEASE | mu.BI_TEST_HOOKS))
    refuse_rel("a host-bootloader build", "HOST_BOOTLOADER", release_img(flags=mu.BI_RELEASE | mu.BI_HOST_BOOTLOADER))
    refuse_rel("a keytest build", "KEYTEST", release_img(flags=mu.BI_RELEASE | mu.BI_KEYTEST))
    refuse_rel("a non-release updater build", "not a release updater build", release_img(flags=0))
    refuse_rel("a record with a retired key flag", "reserved", release_img(flags=mu.BI_RELEASE | 0x20))
    refuse_rel("a record with reserved0 set", "reserved", release_img(reserved0=2))
    refuse_rel("a protocol-1 build", "protocol", release_img(proto=1))
    refuse_rel("a protocol-2 build (32-byte reports)", "protocol", release_img(proto=2))
    refuse_rel("the wrong tag", "expected 'v9'", None, *rel_common, "--expect-version", "v9")
    refuse_rel("the wrong number", "expected 4", None, *rel_common, "--expect-fw-version", "4")
    refuse_rel("a blank-keymap image labeled sval", "keymap_id 1, the image was built with 2", release_img(keymap_id=2))
    refuse_rel("an image built from another keymap", "not a release keymap build", release_img(keymap_id=0), "--kb", "svalboard/right", "--keymap-id", "1", "--release")
    refuse_rel("a host-test record", "not a release keymap build", release_img(hand=0xFF, pointing_id=0xFF))
    # Gentle (D33), release or not: a --kb that is not the image's side or pointing device
    # (auto labeled as one module, a right image as left) is a warning the user confirms.
    # No answer (no terminal) or no writes nothing; y or --yes writes it as labeled.
    for args in (rel_common, ["--kb", "svalboard/right"]):
        for kb, needle, hp in (("svalboard/left", "says side left (hand 0), but the image was built for right", (0, 5)), ("svalboard/trackball/pmw3389/right", "says pointing device pmw3389 (pointing_id 3), but the image was built for auto", (1, 3))):
            for answer, extra, ok in (("", [], False), ("n\n", [], False), ("y\n", [], True), ("", ["--yes"], True)):
                src = tmp / "mis.uf2"
                src.write_bytes(uf2(release_img()))
                target = tmp / "mis.svup"
                if target.exists():
                    target.unlink()
                rc, text = run(src, "-o", target, "--kb", kb, *args[2:], *extra, answer=answer)
                what = f"{kb} {args[2:]} {extra} answer {answer!r}"
                check(needle in text and "refused: the" not in text, f"{what}: warned: {text.strip()}")
                if ok:
                    check(rc == 0 and target.exists(), f"{what}: written: rc {rc}: {text.strip()}")
                    if target.exists():
                        g = mu.parse_manifest(target.read_bytes()[:mu.MANIFEST_BYTES])
                        check((g["hand"], g["pointing_id"]) == hp, f"{what}: labeled as --kb says {g}")
                    check(("--yes: going ahead" in text) == bool(extra), f"{what}: --yes noted")
                else:
                    check(rc == 2 and "not confirmed" in text and not target.exists(), f"{what}: not written: rc {rc}: {text.strip()}")
    # the matching --kb is never asked about (stdin empty: a question would refuse)
    rc, text, _ = rel("match", release_img(), *rel_common)
    check(rc == 0 and "warning" not in text, f"a matching --kb: no question: {text.strip()}")
    rc, text, _ = rel("fwmismatch", release_img(), *rel_common, "--fw-version", "9")
    check(rc == 2 and "reports 3" in text, f"refuses a --fw-version the image contradicts: {text.strip()}")
    rc, text, _ = rel("vmismatch", release_img(), *rel_common, "--version-string", "vOther")
    check(rc == 2 and "reports 'vM3-test'" in text, f"refuses a --version-string the image contradicts: {text.strip()}")
    rc, text, _ = rel("twoinfo", release_img(extra=b"SVBI\x03" + bytes(27)))
    check(rc == 2 and "build-info records" in text, f"refuses two build-info records: {text.strip()}")
    for old in (1, 2):
        rc, text, _ = rel(f"v{old}rec", image(0x1800)[:0x800] + b"SVBI" + bytes([old]) + bytes(0x1800 - 0x805), "--kb", "svalboard/right")
        check(rc == 2 and f"version-{old} build-info" in text, f"refuses a version-{old} record: {text.strip()}")
    # the plain path takes the record's version and keymap too
    rec_uf2 = tmp / "rec.uf2"
    rec_uf2.write_bytes(uf2(release_img(flags=0, fw=2102, version=b"smoke-B", hand=0, keymap_id=2)))
    q = make("ok_record_version.svup", "--kb", "svalboard/left", src=rec_uf2)
    g = mu.parse_manifest(q.read_bytes()[:mu.MANIFEST_BYTES])
    check((g["fw_version"], mu.manifest_version(g), g["keymap_id"]) == (2102, "smoke-B", 2), f"version and keymap from the record {g}")

    # ---- real builds ----------------------------------------------------------------------
    for real in reals:
        m = re.match(r"(svalboard(?:_trackball_pmw33(?:60|89)|_trackpoint|_azoteq)?_(?:left|right))_(\w+)\.uf2$", real.name)
        check(m is not None, f"real UF2 name {real.name}")
        if not m:
            continue
        kb = m.group(1).replace("_", "/")
        name = f"ok_real_{real.stem}.svup"
        p = make(name, "--kb", kb, "--keymap", m.group(2), src=real)
        if p.exists():
            _, body = mu.parse_svup(p.read_bytes())
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
