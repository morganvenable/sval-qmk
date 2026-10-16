#!/usr/bin/env python3
"""Host tests for keyboards/svalboard/tools/release_version.py (the D32 version check).

    test_release_version.py OUT_DIR

The previous release's committed number is stubbed, so no tags or network
are needed.
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


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def run(out, tag, rels, committed):
    """main() with stubs: rels as (tag, draft, prerelease, published) tuples, committed {tag: n}."""
    lst = out / "releases.tsv"
    lst.write_text("".join(f"{t}\t{str(d).lower()}\t{str(p).lower()}\t{when}\n" for t, d, p, when in rels))
    gh_out = out / "github_output"
    if gh_out.exists():
        gh_out.unlink()
    rv.committed_at = lambda t: committed.get(t, 0)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = rv.main([tag, "--repo", "x/y", "--releases", str(lst), "--github-output", str(gh_out)])
    outputs = dict(line.split("=", 1) for line in gh_out.read_text().splitlines()) if gh_out.exists() else {}
    return rc, buf.getvalue(), outputs


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    fw = rv.parse_version_file((ROOT / rv.VERSION_FILE).read_text(), rv.VERSION_FILE)
    check(1 <= fw <= rv.MAX_FW, f"committed fw_version {fw}")
    check(not hasattr(rv, "mu"), "release_version.py has no key checks")

    # vLaunch and vLaunch2 were published under the old names; their numbers still count
    history = [("vLaunch", False, False, "2026-10-07T04:37:07Z"), ("vLaunch2", False, False, "2026-10-08T20:20:35Z"), ("vRC2", False, True, "2026-10-09T00:00:00Z"), ("vDraft", True, False, "")]

    # a prerelease (x tag): published at once, compared with vLaunch2 (before the updater: 0)
    rc, text, o = run(out, "x3", history, {})
    check(rc == 0 and o == dict(version="x3", fw_version=str(fw), prerelease="true", draft="false", previous="vLaunch2"), f"x3 is a prerelease: {rc} {o} {text}")
    # a full release (v tag): a draft, not a prerelease
    rc, text, o = run(out, "v3", history, {"vLaunch2": fw - 1})
    check(rc == 0 and o == dict(version="v3", fw_version=str(fw), prerelease="false", draft="true", previous="vLaunch2"), f"v3 is a full release: {rc} {o} {text}")
    rc, text, o = run(out, "v3", history, {})
    check(rc == 0 and o.get("prerelease") == "false", f"a v tag needs nothing else: {rc} {o} {text}")
    # the number must go up
    rc, text, o = run(out, "v3", history, {"vLaunch2": fw})
    check(rc == 1 and "not greater" in text and not o, f"same number refused: {text}")
    rc, text, _ = run(out, "x3", history, {"vLaunch2": fw + 5})
    check(rc == 1 and "not greater" in text, f"lower number refused: {text}")
    # this tag's own release and drafts are not compared; prereleases are
    rc, _, o = run(out, "v3", history + [("v3", False, False, "2026-12-01T00:00:00Z")], {"vLaunch2": fw - 1, "v3": fw})
    check(rc == 0 and o.get("previous") == "vLaunch2", f"this tag's own release is skipped: {o}")
    rc, _, o = run(out, "v3", history, {"vDraft": fw + 9, "vLaunch2": fw - 1})
    check(rc == 0, "a draft's number is not compared")
    rc, text, _ = run(out, "v3", history, {"vRC2": fw, "vLaunch2": fw - 1})
    check(rc == 1 and "vRC2" in text, f"another prerelease's number is compared: {text}")

    # x tags count: v3 after x3 needs a higher number (graduating is a rebuild)
    xs = history + [("x3", False, True, "2026-10-20T00:00:00Z")]
    rc, text, o = run(out, "x3.1", xs, {"vLaunch2": fw - 2, "x3": fw - 1})
    check(rc == 0 and o.get("prerelease") == "true" and o.get("previous") == "x3", f"x3.1 follows x3: {o} {text}")
    rc, text, _ = run(out, "x3.1", xs, {"vLaunch2": fw - 2, "x3": fw})
    check(rc == 1 and "not greater" in text, f"x3.1 must raise the number: {text}")
    rc, text, _ = run(out, "v3", xs, {"vLaunch2": fw - 1, "x3": fw})
    check(rc == 1 and "x3" in text, f"v3 must come after the x tags before it: {text}")
    rc, _, o = run(out, "v3", xs, {"vLaunch2": fw - 2, "x3": fw - 1})
    check(rc == 0 and o.get("draft") == "true" and o.get("previous") == "x3", f"v3 after x3: {o}")
    rc, _, o = run(out, "x3", [], {})
    check(rc == 0 and o.get("previous") == "", "no previous release")

    # release tags: v or x, a number, optional point parts; nothing else releases
    for tag, pre in (("v3", "false"), ("v3.1", "false"), ("v10.2.3", "false"), ("x3", "true"), ("x3.1", "true"), ("x10.0", "true")):
        rc, text, o = run(out, tag, history, {})
        check(rc == 0 and o.get("prerelease") == pre and o.get("draft") != pre, f"{tag} is a release tag: {rc} {text}")
    for tag in ("vLaunch2", "vLaunch3", "vNext1", "v3-rc1", "x3-rc1", "v3-dryrun1", "x", "v", "3", "V3", "X3", "v3.", "v.3", "v3..1", "x3a", "vx3", "xv3", "v03a", "v3\n", " v3", "keybard-host-v3"):
        rc, text, o = run(out, tag, history, {})
        check(rc == 1 and "not a release tag" in text and not o, f"tag {tag!r} refused: {text}")

    # tag names that cannot be a version string
    for tag in ("v1.2.3.4.5.6.7.8.9", "x12345678901234567"):
        rc, text, _ = run(out, tag, history, {})
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
