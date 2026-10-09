#!/usr/bin/env python3
"""Check the release build artifacts before anything is published or signed (M3).

    check_release_artifacts.py DIR --version TAG [--github-output FILE]
    check_release_artifacts.py DIR --version TAG --expect "NAME=SHA512 ..."

DIR holds what release CI's build jobs uploaded: for each of the 12 release
keymap builds NAME (svalboard_trackball_pmw3389_right_sval, ...) exactly
NAME.elf, NAME.img (raw image), NAME.manifest (unsigned) and NAME_TAG.uf2,
and nothing else. For each build this checks, and exits 1 on any failure:

  - the raw image is the ELF's flash contents: the ELF's loadable segments
    at their load addresses, as objcopy -O binary writes them, padded with
    0xFF to a whole 256-byte UF2 block. So the image that is signed is the
    one check_release_elf.py and check_split_tables.py linted;
  - the .uf2 holds exactly that image (the file published for BOOTSEL);
  - the manifest's image length and SHA-512 are the image's, its version is
    TAG, and its side, pointing device and keymap are NAME's;
  - the image's build-info record names the same side, pointing device and
    keymap, so a left image cannot be signed as a right one.

It prints "NAME=SHA512" for each image. With --github-output it writes them
as one output, images=, for the signing job, which runs this again with
--expect: the directory it downloaded must hold the same 12 images, byte for
byte, and make_update.py sign gets each SHA-512 as --expect-sha512.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_update as mu  # noqa: E402

# The release keymap builds: release.yml's build_sval and build_blank matrices.
RELEASE_BUILDS = [(f"svalboard/{v}{side}", km)
                  for v, km in (("", "sval"), ("", "blank"), ("trackpoint/", "sval"), ("trackball/pmw3360/", "sval"),
                                ("trackball/pmw3389/", "sval"), ("azoteq/", "sval"))
                  for side in ("left", "right")]
PT_LOAD = 1


def build_name(kb, keymap):
    return kb.replace("/", "_") + "_" + keymap


def elf_flash_image(path):
    """The flash contents of an ELF as objcopy -O binary writes them (loadable
    segments with file contents, at their physical load addresses, gaps zero),
    padded with 0xFF to a whole 256-byte block as the UF2 conversion does."""
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise mu.Refused(f"{path}: not a 32-bit little-endian ELF")
    e_phoff, = struct.unpack_from("<I", data, 0x1C)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x2A)
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, _vaddr, p_paddr, p_filesz, _memsz, _flags, _align = struct.unpack_from(
            "<8I", data, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD and p_filesz:
            segs.append((p_paddr, data[p_offset:p_offset + p_filesz]))
    if not segs:
        raise mu.Refused(f"{path}: no loadable segments")
    base = min(a for a, _ in segs)
    if base != mu.XIP_BASE:
        raise mu.Refused(f"{path}: flash contents start at {base:#x}, not {mu.XIP_BASE:#x}")
    out = bytearray(max(a + len(b) for a, b in segs) - base)
    for a, b in segs:
        out[a - base:a - base + len(b)] = b
    out += b"\xff" * (-len(out) % 256)
    return bytes(out)


def check_dir(d, version):
    """({name: sha512 hex}, [problems]) for the artifacts in d."""
    d = Path(d)
    problems, hashes = [], {}
    want = {}
    for kb, km in RELEASE_BUILDS:
        n = build_name(kb, km)
        want[n] = (kb, km)
    expected_files = {f"{n}{ext}" for n in want for ext in (".elf", ".img", ".manifest")}
    expected_files |= {f"{n}_{version}.uf2" for n in want}
    present = {p.name for p in d.iterdir()} if d.is_dir() else set()
    for extra in sorted(present - expected_files):
        problems.append(f"unexpected file {extra}: the directory must hold only the 12 builds' artifacts")
    for missing in sorted(expected_files - present):
        problems.append(f"missing {missing}")
    if problems:
        return hashes, problems
    for n, (kb, km) in sorted(want.items()):
        def bad(msg):
            problems.append(f"{n}: {msg}")
        try:
            img = (d / f"{n}.img").read_bytes()
            if elf_flash_image(d / f"{n}.elf") != img:
                bad("the .img is not the ELF's flash contents (objcopy -O binary)")
            if mu.uf2_to_image((d / f"{n}_{version}.uf2").read_bytes()) != img:
                bad("the .uf2 does not hold the .img")
            raw = (d / f"{n}.manifest").read_bytes()
            if len(raw) != mu.MANIFEST_BYTES:
                bad(f"the manifest is {len(raw)} B, not {mu.MANIFEST_BYTES}")
                continue
            m = mu.parse_manifest(raw)
            hand, pointing_id = mu.kb_fields(kb)
            ident = dict(hand=hand, pointing_id=pointing_id, keymap_id=mu.KEYMAP[km])
            if m["image_len"] != len(img) or m["sha512"] != hashlib.sha512(img).digest():
                bad("the manifest's image length or SHA-512 is not the .img's")
            if mu.manifest_version(m) != version:
                bad(f"the manifest's version is {mu.manifest_version(m)!r}, not {version!r}")
            for k, v in ident.items():
                if m[k] != v:
                    bad(f"the manifest says {k} {m[k]}, {n} is {v}")
            info = mu.build_info(img)
            if info is None:
                bad("the image has no build-info record")
            else:
                for k, v in ident.items():
                    if info[k] != v:
                        bad(f"the image's build-info record says {k} {info[k]}, {n} is {v}")
            hashes[n] = hashlib.sha512(img).hexdigest()
        except (mu.Refused, OSError, struct.error) as e:
            bad(str(e))
    return hashes, problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--version", required=True, help="the tag: the .uf2 names and the manifests' version")
    ap.add_argument("--github-output", help="append images=NAME=SHA512 ... here ($GITHUB_OUTPUT)")
    ap.add_argument("--expect", help="NAME=SHA512 pairs (the lint job's images output): the images must be these")
    args = ap.parse_args(argv)
    hashes, problems = check_dir(args.dir, args.version)
    if args.expect is not None and not problems:
        given = dict(w.split("=", 1) for w in args.expect.split() if "=" in w)
        if given != hashes:
            names = sorted(set(given) ^ set(hashes)) or sorted(k for k in hashes if given.get(k) != hashes[k])
            problems.append(f"the images are not the ones the lint checked: {', '.join(names)}")
    for p in problems:
        print(f"FAIL: {p}")
    if problems:
        return 1
    line = " ".join(f"{n}={h}" for n, h in sorted(hashes.items()))
    for n, h in sorted(hashes.items()):
        print(f"{n}={h}")
    if args.github_output:
        with open(args.github_output, "a") as f:
            f.write(f"images={line}\n")
    print(f"ok: {len(hashes)} release builds: each .img is its ELF's flash contents and its .uf2's image; "
          f"manifests and build-info records match the build names; version {args.version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
