#!/usr/bin/env python3
"""Host test for no-chord dev builds (SVAL_UPDATE_NO_CHORD) with sval_update.py.

    test_no_chord_tool.py LIB.so OUT_DIR

LIB.so is the updater built with SVAL_UPDATE_NO_CHORD (util/updater_test/run.sh).
With --yes, an update of this half, of the other half over the split cable, and
pair all run with no chord and no question, and say nothing about the chord.
"""
import contextlib
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_sval_update_tool as t  # noqa: E402  (the fake board and the simulated clock)

su, mu, lib, check = t.su, t.mu, t.lib, t.check


def main():
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    # A wait for the chord is a poll that sleeps in CONFIRM_WAIT (state 2).
    chords = []

    def sleep(s):
        if lib.host_state() == 2:
            chords.append(s)
        t.fake_sleep(s)

    su.time.sleep = sleep
    su.time.monotonic = t.fake_monotonic
    su.ask = t.no_question

    img = t.image(0x5100)
    files = {}
    for side, hand, pointing in (("left", 0, 1), ("right", 1, 3)):
        m = mu.build_manifest(img, hand=hand, pointing_id=pointing, keymap_id=0, flags=0, epoch=0, storage_format=2,
                              fw_version=9, version="dev")
        files[side] = out / f"{side}.svup"
        files[side].write_bytes(m + img)

    lib.host_lib_reset(1)
    info = su.decode_info(*t.FakeBoard().op(su.INFO))
    check(info["no_chord"] and not info["release_build"], f"INFO: a no-chord build {info}")

    # this half
    lib.host_lib_reset(1)
    board = t.FakeBoard()
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.Session(board, t.args_with(yes=True)).run(files["right"])
    lib.host_passes(150)
    text = log.getvalue()
    check(rc == 0 and lib.host_commit_runs() == 1, f"this half: exit {rc}\n{text}")
    check(not chords and "chord" not in text and "Index South" not in text, f"this half\n{text}")

    # the other half, over the split cable
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    board = t.FakeBoard()
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.Session(board, t.args_with(yes=True)).run(files["left"])
    text = log.getvalue()
    check(rc == 0 and lib.host_slave_commits() == 1, f"the other half: exit {rc}\n{text}")
    check(not chords and "chord" not in text, f"the other half\n{text}")

    # pair: both halves
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    su.reopen = lambda serial, timeout: t.FakeBoard()
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.pair(t.FakeBoard(), t.args_with(yes=True), [files["left"], files["right"]], [])
    text = log.getvalue()
    check(rc == 0 and lib.host_slave_commits() == 1 and lib.host_master_reboots() == 1, f"pair: exit {rc}\n{text}")
    check(not chords and "chord" not in text and "pair: presence match" in text, f"pair\n{text}")

    print(f"no-chord tool: {t.checks} checks, {t.failures} failures")
    return 1 if t.failures else 0


if __name__ == "__main__":
    sys.exit(main())
