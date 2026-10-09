#!/usr/bin/env python3
"""Host tests for keyboards/svalboard/tools/check_release_artifacts.py (M3).

    test_release_artifacts.py OUT_DIR

Builds a synthetic release directory (for each of the 12 release builds: a
minimal ELF with one loadable segment at 0x10000000, the .img, the unsigned
manifest from make_update.py unsigned, and the .uf2), checks that it passes,
that --expect takes its own hashes and refuses others, and that each kind of
substitution is refused.
"""
import contextlib
import importlib.util
import io
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "keyboards/svalboard/tools"
sys.path.insert(0, str(TOOLS))
import check_release_artifacts as cra  # noqa: E402
import make_update as mu  # noqa: E402

spec = importlib.util.spec_from_file_location("tmu", Path(__file__).parent / "test_make_update.py")
tmu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tmu)

VER = "vM3-test"
checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def elf(flash, extra_ram=b""):
    """A 32-bit ARM ELF with a PT_LOAD for flash at 0x10000000 and, if given, one for
    .data loaded right after it in flash (paddr) but linked in RAM (vaddr)."""
    segs = [(0x10000000, 0x10000000, flash)]
    if extra_ram:
        segs.append((0x20000000, 0x10000000 + len(flash), extra_ram))
    phoff = 52
    off = phoff + 32 * len(segs)
    hdr = bytearray(b"\x7fELF\x01\x01\x01" + bytes(9))
    hdr += struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, 0x100001F7, phoff, 0, 0x05000200, 52, 32, len(segs), 40, 0, 0)
    ph, body = b"", b""
    for vaddr, paddr, data in segs:
        ph += struct.pack("<8I", 1, off + len(body), vaddr, paddr, len(data), len(data), 5, 4)
        body += data
    return bytes(hdr) + ph + body


def image_for(kb, km, data_tail=b""):
    hand, pointing_id = mu.kb_fields(kb)
    body = bytearray(tmu.image(0x1800))
    rec = struct.pack(mu.BUILD_INFO_FMT, b"SVBI", 2, mu.BI_RELEASE | mu.BI_KEYS_DRY_RUN, 2, 1, 3,
                      VER.encode().ljust(16, b"\0"), hand, pointing_id, mu.KEYMAP[km], 0)
    body[0x800:0x800 + len(rec)] = rec
    return bytes(body)


def make_dir(d):
    d.mkdir(parents=True)
    for i, (kb, km) in enumerate(cra.RELEASE_BUILDS):
        n = cra.build_name(kb, km)
        flash = image_for(kb, km)
        data = bytes([i]) * 0x40 if i % 2 else b""  # some builds with a .data segment
        img = flash + data
        img += b"\xff" * (-len(img) % 256)
        (d / f"{n}.elf").write_bytes(elf(flash, data))
        (d / f"{n}_{VER}.uf2").write_bytes(tmu.uf2(img))
        p = subprocess.run([sys.executable, "-I", str(TOOLS / "make_update.py"), "unsigned", str(d / f"{n}_{VER}.uf2"),
                            "--kb", kb, "--keymap", km, "--release", "--image-out", str(d / f"{n}.img"),
                            "--manifest-out", str(d / f"{n}.manifest")], capture_output=True, text=True)
        check(p.returncode == 0, f"unsigned {n}: {p.stdout}{p.stderr}")


def quiet_main(args):
    with contextlib.redirect_stdout(io.StringIO()):
        return cra.main(args)


def main():
    out = Path(sys.argv[1])
    if out.exists():
        shutil.rmtree(out)
    good = out / "good"
    make_dir(good)
    hashes, problems = cra.check_dir(good, VER)
    check(not problems and len(hashes) == 12, f"a clean directory passes: {problems}")
    expect = " ".join(f"{k}={v}" for k, v in hashes.items())
    check(quiet_main([str(good), "--version", VER, "--expect", expect]) == 0, "--expect with its own hashes")
    wrong = expect.replace(next(iter(hashes.values())), "0" * 128)
    check(quiet_main([str(good), "--version", VER, "--expect", wrong]) == 1, "--expect with another hash refused")
    check(quiet_main([str(good), "--version", VER, "--expect", " ".join(expect.split()[:11])]) == 1,
          "--expect with 11 images refused")
    check(quiet_main([str(good), "--version", "vOther"]) == 1, "another version refused")

    r, l, rb = "svalboard_right_sval", "svalboard_left_sval", "svalboard_right_blank"

    def case(what, needle, edit):
        d = out / "case"
        if d.exists():
            shutil.rmtree(d)
        shutil.copytree(good, d)
        edit(d)
        _, probs = cra.check_dir(d, VER)
        check(any(needle in p for p in probs), f"{what}: refused with {needle!r}: {probs}")

    def flip(path, at=0x1000):
        b = bytearray(path.read_bytes())
        b[at] ^= 1
        path.write_bytes(b)

    def remanifest(d, n, kb, km):
        subprocess.run([sys.executable, "-I", str(TOOLS / "make_update.py"), "unsigned", str(d / f"{n}.img"), "--raw",
                        "--kb", kb, "--keymap", km, "--release", "--image-out", str(d / "x.img"),
                        "--manifest-out", str(d / f"{n}.manifest")], capture_output=True, text=True)
        (d / "x.img").unlink(missing_ok=True)

    case("a flipped image byte with a re-made manifest", "not the ELF's flash contents",
         lambda d: (flip(d / f"{r}.img"), remanifest(d, r, "svalboard/right", "sval")))
    case("a flipped .uf2 byte", "the .uf2 does not hold the .img", lambda d: flip(d / f"{r}_{VER}.uf2", 32 + 0x10))
    case("another ELF", "not the ELF's flash contents", lambda d: shutil.copy(d / f"{l}.elf", d / f"{r}.elf"))
    case("a left image and manifest as right", "the manifest says hand 0", lambda d: [
        shutil.copy(d / f"{l}{x}", d / f"{r}{x}") for x in (".img", ".manifest", ".elf")]
        + [shutil.copy(d / f"{l}_{VER}.uf2", d / f"{r}_{VER}.uf2")])
    case("a blank image and manifest as sval", "keymap_id 2", lambda d: [
        shutil.copy(d / f"{rb}{x}", d / f"{r}{x}") for x in (".img", ".manifest", ".elf")]
        + [shutil.copy(d / f"{rb}_{VER}.uf2", d / f"{r}_{VER}.uf2")])
    case("a manifest edited to the other hand", "the manifest says hand 0",
         lambda d: (d / f"{r}.manifest").write_bytes((d / f"{r}.manifest").read_bytes()[:7] + b"\0"
                                                     + (d / f"{r}.manifest").read_bytes()[8:]))
    case("an extra artifact", "unexpected file", lambda d: shutil.copy(d / f"{r}.img", d / "extra.img"))
    case("a missing manifest", "missing", lambda d: (d / f"{r}.manifest").unlink())
    case("a truncated manifest", "manifest is", lambda d: (d / f"{r}.manifest").write_bytes(b"\0" * 10))

    # the ELF reader against a non-ELF and an ELF that does not start at XIP_BASE
    bad = out / "bad.elf"
    bad.write_bytes(b"not an elf")
    try:
        cra.elf_flash_image(bad)
        check(False, "a non-ELF is refused")
    except mu.Refused:
        check(True, "a non-ELF is refused")
    raw = bytearray(elf(b"\0" * 0x100))
    struct.pack_into("<I", raw, 52 + 12, 0x20000000)  # paddr of the one segment
    bad.write_bytes(raw)
    try:
        cra.elf_flash_image(bad)
        check(False, "an ELF outside flash is refused")
    except mu.Refused:
        check(True, "an ELF outside flash is refused")

    print(f"check_release_artifacts.py: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
