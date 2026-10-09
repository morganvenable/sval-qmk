#!/usr/bin/env python3
"""Host tests for keyboards/svalboard/tools/release_version.py (the D32 version check).

    test_release_version.py OUT_DIR

The previous release's committed number and the keys header are stubbed, so
no tags or network are needed.
"""
import contextlib
import importlib.util
import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "keyboards/svalboard/tools/release_version.py"
spec = importlib.util.spec_from_file_location("release_version", TOOL)
rv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rv)

checks = failures = 0
REAL_RELEASE_KEYS = rv.mu.release_keys


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def run(out, tag, rels, committed, keys_dry_run, header=None):
    """main() with stubs: rels as (tag, draft, prerelease, published) tuples, committed {tag: n}."""
    lst = out / "releases.tsv"
    lst.write_text("".join(f"{t}\t{str(d).lower()}\t{str(p).lower()}\t{when}\n" for t, d, p, when in rels))
    gh_out = out / "github_output"
    if gh_out.exists():
        gh_out.unlink()
    rv.committed_at = lambda t: committed.get(t, 0)
    if header is None:
        rv.mu.release_keys = lambda *a, **k: ({1: b"1" * 32, 2: b"2" * 32}, keys_dry_run)
    else:
        rv.mu.release_keys = REAL_RELEASE_KEYS
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = rv.main([tag, "--repo", "x/y", "--releases", str(lst), "--github-output", str(gh_out),
                      *(["--keys-header", str(header)] if header else [])])
    outputs = dict(line.split("=", 1) for line in gh_out.read_text().splitlines()) if gh_out.exists() else {}
    return rc, buf.getvalue(), outputs


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    fw = rv.parse_version_file((ROOT / rv.VERSION_FILE).read_text(), rv.VERSION_FILE)
    check(1 <= fw <= rv.MAX_FW, f"committed fw_version {fw}")

    history = [("vLaunch", False, False, "2026-10-07T04:37:07Z"), ("vLaunch2", False, False, "2026-10-08T20:20:35Z"),
               ("vRC2", False, True, "2026-10-09T00:00:00Z"), ("vDraft", True, False, "")]

    # dry run with the DRY RUN keys: a prerelease, compared with vLaunch2 (pre-M3: 0)
    rc, text, o = run(out, "vM3-dryrun1", history, {}, True)
    check(rc == 0 and o == dict(version="vM3-dryrun1", fw_version=str(fw), prerelease="true", previous="vLaunch2"),
          f"dry run passes as a prerelease: {rc} {o} {text}")
    for tag in ("vM3-DryRun2", "vdry-run", "vLaunch3-dryrun"):
        rc, _, o = run(out, tag, history, {}, True)
        check(rc == 0 and o.get("prerelease") == "true", f"{tag} is a dry run")

    # a normal tag while the keys are DRY RUN: refused, and no outputs written
    rc, text, o = run(out, "vLaunch3", history, {}, True)
    check(rc == 1 and "DRY RUN" in text and not o, f"normal tag with DRY RUN keys refused: {text}")

    # production keys: a normal release
    rc, text, o = run(out, "vLaunch3", history, {"vLaunch2": fw - 1}, False)
    check(rc == 0 and o.get("prerelease") == "false" and o.get("previous") == "vLaunch2", f"normal release: {rc} {o} {text}")
    # the number must go up
    rc, text, o = run(out, "vLaunch3", history, {"vLaunch2": fw}, False)
    check(rc == 1 and "not greater" in text, f"same number refused: {text}")
    rc, text, _ = run(out, "vLaunch3", history, {"vLaunch2": fw + 5}, False)
    check(rc == 1 and "not greater" in text, f"lower number refused: {text}")
    # the newest published non-prerelease counts, not a prerelease or draft, and not this tag itself
    rc, _, o = run(out, "vLaunch3", history + [("vLaunch3", False, False, "2026-12-01T00:00:00Z")], {"vLaunch2": fw - 1, "vLaunch3": fw}, False)
    check(rc == 0 and o.get("previous") == "vLaunch2", f"this tag's own release is skipped: {o}")
    rc, _, o = run(out, "vLaunch3", history, {"vRC2": fw + 9, "vLaunch2": fw - 1}, False)
    check(rc == 0, "a prerelease's number is not compared")
    rc, _, o = run(out, "vM3-dryrun1", [], {}, True)
    check(rc == 0 and o.get("previous") == "", "no previous release")

    # a half-done key swap: DRY_RUN set to 0 while a retired (DRY RUN) key is
    # still in a slot. Refused for every tag, so no normal release is signed.
    real = (ROOT / "keyboards/svalboard/updater/update_release_keys.h").read_text()
    if "SVAL_UPDATE_RELEASE_KEYS_DRY_RUN 1" in real:
        half = out / "half_swap.h"
        half.write_text(real.replace("SVAL_UPDATE_RELEASE_KEYS_DRY_RUN 1", "SVAL_UPDATE_RELEASE_KEYS_DRY_RUN 0"))
        for tag in ("vLaunch3", "vM3-dryrun9"):
            rc, text, o = run(out, tag, history, {"vLaunch2": fw - 1}, False, header=half)
            check(rc == 1 and "retired key" in text and not o, f"{tag} with a half-done key swap refused: {text}")
        rc, text, o = run(out, "vM3-dryrun9", history, {}, True, header=ROOT / "keyboards/svalboard/updater/update_release_keys.h")
        check(rc == 0 and o.get("prerelease") == "true", f"the real DRY RUN header passes a dry run: {text}")

    # tag names that cannot be a version string
    for tag in ("vLaunch2-with-a-long-name", "v Launch", "v/1"):
        rc, text, _ = run(out, tag + "-dryrun", history, {}, True)
        check(rc == 1 and "not a version string" in text, f"tag {tag!r} refused: {text}")

    # the version file
    for text, ok in (("3\n", True), ("# c\n12\n", True), ("", False), ("3\n4\n", False), ("x\n", False), ("1234567890\n", False)):
        try:
            rv.parse_version_file(text, "f")
            got = True
        except ValueError:
            got = False
        check(got == ok, f"version file {text!r}")

    print(f"release_version.py: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
