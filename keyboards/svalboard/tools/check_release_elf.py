#!/usr/bin/env python3
"""Lint release updater ELFs before they are signed (plan M3).

    check_release_elf.py BUILD.elf [BUILD.elf ...]

Release CI builds every release keymap as a release updater build
(SVAL_UPDATER=yes SVAL_UPDATE_RELEASE=yes). For each ELF this checks, and
exits 1 on any failure:

  - it is an updater build: one sval_update_build_info record (updater/
    update_keys.h) with magic SVBI and info_ver 1, and updater_task linked;
  - the record says RELEASE, and none of TEST_KEY, TEST_HOOKS, KEYTEST or
    HOST_BOOTLOADER; its DRY RUN bit matches update_release_keys.h;
  - two release key slots: release_keys is 64 bytes holding key 1 then key 2
    of update_release_keys.h, and each key appears exactly once in the
    image's initialised data;
  - no test key: the TEST-ONLY public key appears nowhere in the image, and
    there is no test_key symbol;
  - no test hooks (update_commit_test_halt, op_test_halt, halt_point,
    halt_fed), no SVAL_KEYTEST (keytest_task, keytest_command) and no
    SVAL_HOST_BOOTLOADER (scanlab.c's reboot_pending, reboot_due_ms).

The symbol checks look at the ELF's own symbol table, independently of the
build-info record, so a record that lies about the build is caught. The
split message table is checked separately (check_split_tables.py --golden),
and the commit's RAM code by check_ram_funcs.py.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_split_tables as cst  # noqa: E402  (same directory: the ELF reader)
import make_update as mu  # noqa: E402  (keys, build-info layout)

STT_OBJECT, STT_FUNC = 1, 2
FORBIDDEN = {
    "test_key": "the TEST-ONLY key (SVAL_UPDATE_TEST_KEY)",
    "update_commit_test_halt": "test hooks (SVAL_UPDATE_TEST_HOOKS)",
    "op_test_halt": "test hooks (SVAL_UPDATE_TEST_HOOKS)",
    "halt_point": "test hooks (SVAL_UPDATE_TEST_HOOKS)",
    "halt_fed": "test hooks (SVAL_UPDATE_TEST_HOOKS)",
    "keytest_task": "SVAL_KEYTEST",
    "keytest_command": "SVAL_KEYTEST",
    "reboot_pending": "SVAL_HOST_BOOTLOADER",
    "reboot_due_ms": "SVAL_HOST_BOOTLOADER",
}


def alloc_bytes(path):
    """The contents of every allocated SHT_PROGBITS section (code, .rodata, .data's load image)."""
    data = Path(path).read_bytes()
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, _ = struct.unpack_from("<HHH", data, 0x2E)
    out = []
    for i in range(e_shnum):
        _, sh_type, flags, _, offset, size = struct.unpack_from("<IIIIII", data, e_shoff + i * e_shentsize)
        if sh_type == 1 and flags & 0x2:  # SHT_PROGBITS, SHF_ALLOC
            out.append(data[offset:offset + size])
    return b"\0".join(out)


def check(path, keys, dry_run, test_pub):
    errors = []
    symbols, read = cst.read_elf(path, kinds=(STT_OBJECT, STT_FUNC))
    for name, what in FORBIDDEN.items():
        if name in symbols:
            errors.append(f"symbol {name}: {what} is in this build")
    if "updater_task" not in symbols:
        errors.append("no updater_task: not an updater build (SVAL_UPDATER=yes)")

    info = symbols.get("sval_update_build_info", [])
    if len(info) != 1 or info[0][1] != mu.BUILD_INFO_BYTES:
        errors.append(f"expected one {mu.BUILD_INFO_BYTES}-byte sval_update_build_info, found {info}")
    else:
        (magic, ver, flags, nkeys, proto, fw, version, hand, pointing_id, keymap_id,
         reserved) = struct.unpack(mu.BUILD_INFO_FMT, read(info[0][0], mu.BUILD_INFO_BYTES))
        if magic + bytes([ver]) != mu.BUILD_INFO_MAGIC:
            errors.append(f"sval_update_build_info has magic {magic!r} version {ver}")
        if hand not in mu.HAND.values() or pointing_id not in mu.POINTING.values() or reserved:
            errors.append(f"the build-info record has hand {hand}, pointing_id {pointing_id}, reserved {reserved}")
        if keymap_id not in mu.KEYMAP.values():
            errors.append(f"the build-info record has keymap_id {keymap_id}: not a release keymap (sval, blank)")
        if not flags & mu.BI_RELEASE:
            errors.append("not a release updater build (SVAL_UPDATE_RELEASE)")
        for bit, what in mu.BI_NOT_IN_RELEASE.items():
            if flags & bit:
                errors.append(f"the build-info record says {what}")
        if bool(flags & mu.BI_KEYS_DRY_RUN) != dry_run:
            errors.append("the build-info DRY RUN bit does not match update_release_keys.h")
        if nkeys != len(mu.RELEASE_KEY_IDS):
            errors.append(f"the build-info record says {nkeys} release key slots")
        if proto != mu.UPDATER_PROTO:
            errors.append(f"updater protocol {proto}, the tools speak {mu.UPDATER_PROTO}")

    slots = symbols.get("release_keys", [])
    want = b"".join(keys[k] for k in mu.RELEASE_KEY_IDS)
    if len(slots) != 1 or slots[0][1] != len(want):
        errors.append(f"expected one {len(want)}-byte release_keys (two key slots), found {slots}")
    elif read(slots[0][0], len(want)) != want:
        errors.append("release_keys does not hold key 1 then key 2 of update_release_keys.h")

    blob = alloc_bytes(path)
    for k in mu.RELEASE_KEY_IDS:
        if blob.count(keys[k]) != 1:
            errors.append(f"release key {k} appears {blob.count(keys[k])} times in the image, expected once")
    if test_pub in blob:
        errors.append("the TEST-ONLY public key is in the image")
    return errors


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if argv else 2
    try:
        keys, dry_run = mu.release_keys()  # refuses retired keys in a production header
    except mu.Refused as e:
        print(f"FAIL: {e}")
        return 1
    test_pub = mu.test_public_key()
    failed = 0
    for path in argv:
        try:
            errors = check(path, keys, dry_run, test_pub)
        except (cst.ElfError, OSError, struct.error) as e:
            errors = [str(e)]
        name = Path(path).name
        if errors:
            failed += 1
            print(f"FAIL {name}")
            for e in errors:
                print(f"    {e}")
        else:
            print(f"ok   {name}: release updater build, two release key slots{' (DRY RUN keys)' if dry_run else ''}, "
                  "no test key, test hooks, keytest or host bootloader")
    print(f"{len(argv) - failed} of {len(argv)} release ELFs pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
