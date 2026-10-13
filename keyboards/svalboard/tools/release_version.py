#!/usr/bin/env python3
"""The release version check (proposed D32), run by release CI before any build.

    release_version.py TAG --repo OWNER/REPO [--releases FILE] [--github-output FILE]

Launch tags are names (vLaunch2), not numbers, so the numeric firmware version
(SVAL_FW_VERSION, D17) is committed in keyboards/svalboard/updater/
fw_version.txt and the version string is the tag name. This checks, and exits
1 on any failure:

  - the tag is a usable version string: at most 16 of A-Z a-z 0-9 . _ + -
    (it goes into the manifest and the firmware's 16-byte version field);
  - the committed number is greater than the highest one committed at any
    published release that is not a draft, a dry run or this tag. Bleeding-
    edge releases count, so vLaunch3 must come after every vNext before it.
    A release from before this file existed counts as 0. Dry runs are not
    compared against, so a dry run never uses up a number.

A dry-run tag is one whose name contains "dryrun" or "dry-run" (any case),
such as vLaunch3-dryrun1: a trial of the release run. A bleeding-edge tag is
vNext followed by a number, such as vNext1. Both are always published as
prereleases, so neither becomes the repository's latest release. Every other
tag must be a stable firmware tag, vLaunch followed by a number (vLaunch3);
anything else is refused, since it would publish as the latest release.
Names with a point release (vLaunch2.3) belong to Keybard and Keybard Host
only (D38).

Writes version=, fw_version=, prerelease= and previous= lines to
--github-output (release CI passes $GITHUB_OUTPUT), and prints them. The list
of releases comes from `gh api` (needs GH_TOKEN), or from --releases: a file
of "tag<TAB>draft<TAB>prerelease<TAB>published_at" lines, for tests.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

VERSION_FILE = "keyboards/svalboard/updater/fw_version.txt"
REPO_ROOT = HERE.parents[2]
DRY_RUN_RE = re.compile(r"dry-?run", re.IGNORECASE)
NEXT_RE = re.compile(r"^vNext[0-9]+$")  # bleeding edge, for next.keybard
STABLE_RE = re.compile(r"^vLaunch[0-9]+$")  # stable; vLaunch2.3 etc. are Keybard/Host-only names
MAX_FW = 999999999  # rules.mk takes at most 9 digits
VERSION_RE = re.compile(r"^[A-Za-z0-9._+-]{1,16}$")  # SVAL_FW_VERSION_STRING (rules.mk, make_update.py)


def parse_version_file(text, where):
    nums = [ln.strip() for ln in text.splitlines() if ln.strip() and not ln.strip().startswith("#")]
    if len(nums) != 1 or not re.fullmatch(r"[0-9]{1,9}", nums[0]):
        raise ValueError(f"{where}: expected one line with a number of at most 9 digits")
    return int(nums[0])


def committed_at(tag):
    """The number committed at a tag, or 0 if the file did not exist there yet."""
    p = subprocess.run(["git", "-C", str(REPO_ROOT), "show", f"refs/tags/{tag}:{VERSION_FILE}"], capture_output=True, text=True)
    if p.returncode != 0:
        exists = subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}"], capture_output=True, text=True)
        if exists.returncode != 0:
            raise ValueError(f"the previous release's tag {tag} is not in this checkout (fetch tags: fetch-depth 0)")
        return 0
    return parse_version_file(p.stdout, f"{tag}:{VERSION_FILE}")


def releases(repo, path):
    if path:
        text = Path(path).read_text()
    else:
        text = subprocess.run(["gh", "api", "--paginate", f"repos/{repo}/releases", "--jq", '.[] | [.tag_name, .draft, .prerelease, .published_at] | @tsv'], capture_output=True, text=True, check=True).stdout
    out = []
    for line in text.splitlines():
        if line.strip():
            tag, draft, pre, published = (line.split("\t") + ["", "", ""])[:4]
            out.append(dict(tag=tag, draft=draft == "true", prerelease=pre == "true", published=published))
    return out


def previous_release(rels, tag):
    """The counted release with the highest committed number, and that number."""
    done = [r for r in rels if not r["draft"] and not DRY_RUN_RE.search(r["tag"]) and r["tag"] != tag and r["published"]]
    if not done:
        return None, 0
    # on a tie, the newest stable release
    n, _, _, best = max((committed_at(r["tag"]), not r["prerelease"], r["published"], r["tag"]) for r in done)
    return best, n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tag")
    ap.add_argument("--repo", required=True)
    ap.add_argument("--releases", help="tab-separated release list instead of gh api (tests)")
    ap.add_argument("--github-output", help="append the outputs here ($GITHUB_OUTPUT)")
    args = ap.parse_args(argv)
    errors = []

    tag = args.tag
    if not VERSION_RE.match(tag):
        errors.append(f"tag {tag!r} is not a version string: at most 16 of A-Z a-z 0-9 . _ + -")
    dry_run = bool(DRY_RUN_RE.search(tag))
    pre = dry_run or bool(NEXT_RE.match(tag))
    if tag.lower().startswith("vnext") and not pre:
        # a near miss would publish as the latest release
        errors.append(f"tag {tag!r}: bleeding-edge tags are vNext and a number, such as vNext1")
    elif not pre and not STABLE_RE.match(tag):
        # any other tag would publish as the latest release
        errors.append(f"tag {tag!r}: stable firmware tags are vLaunch and a number, such as vLaunch3 "
                      "(vLaunch2.3 and the like name Keybard and Keybard Host releases only)")
    try:
        fw = parse_version_file((REPO_ROOT / VERSION_FILE).read_text(), VERSION_FILE)
    except (OSError, ValueError) as e:
        print(f"FAIL: {e}")
        return 1

    try:
        prev, prev_fw = previous_release(releases(args.repo, args.releases), tag)
    except (subprocess.CalledProcessError, ValueError) as e:
        print(f"FAIL: cannot read the previous release: {e}")
        return 1
    if fw <= prev_fw:
        errors.append(f"{VERSION_FILE} says {fw}, not greater than {prev_fw} at the previous release {prev}: raise it")

    outputs = dict(version=tag, fw_version=fw, prerelease="true" if pre else "false", previous=prev or "")
    for k, v in outputs.items():
        print(f"{k}={v}")
    if errors:
        for e in errors:
            print(f"FAIL: {e}")
        return 1
    if args.github_output:
        with open(args.github_output, "a") as f:
            f.writelines(f"{k}={v}\n" for k, v in outputs.items())
    print(f"ok: {tag} is fw {fw} (previous release {prev or 'none'}: {prev_fw})"
          f"{', a dry run: prerelease' if dry_run else ', bleeding edge: prerelease' if pre else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
