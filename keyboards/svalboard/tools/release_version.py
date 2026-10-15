#!/usr/bin/env python3
"""The release version check (proposed D32), run by release CI before any build.

    release_version.py TAG --repo OWNER/REPO [--releases FILE] [--github-output FILE]

Release tags are names (v3, x3), not numbers, so the numeric firmware version
(SVAL_FW_VERSION, D17) is committed in keyboards/svalboard/updater/
fw_version.txt and the version string is the tag name. Two kinds of tag
release (D41):

  - v and a number, with optional point parts (v3, v3.1): a full release,
    published by release CI as a draft for a person to publish;
  - x and a number, with optional point parts (x3, x3.1): a prerelease, for
    next.keybard.svalboard.com, published at once as a GitHub prerelease, so
    it never becomes the repository's latest release.

Every other tag is refused (vLaunch2, v3-rc1, x, 3), so nothing else can
publish. The same tag name marks the firmware, Keybard and Keybard Host
releases that go together. This checks, and exits 1 on any failure:

  - the tag is a release tag, as above, and a usable version string: at most
    16 of A-Z a-z 0-9 . _ + - (it goes into the manifest and the firmware's
    16-byte version field);
  - the committed number is greater than the highest one committed at any
    published release that is not a draft or this tag. Prereleases count, so
    v3 must come after every x tag before it: graduating x3 to v3 means
    raising the number in the commit tagged v3. A release from before this
    file existed counts as 0.

Writes version=, fw_version=, prerelease=, draft= and previous= lines to
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
FULL_RE = re.compile(r"^v[0-9]+(\.[0-9]+)*$")  # full release (v3, v3.1)
PRE_RE = re.compile(r"^x[0-9]+(\.[0-9]+)*$")  # prerelease, for next.keybard (x3, x3.1)
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
    done = [r for r in rels if not r["draft"] and r["tag"] != tag and r["published"]]
    if not done:
        return None, 0
    # on a tie, the newest full release
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
    pre = bool(PRE_RE.fullmatch(tag))
    if not pre and not FULL_RE.fullmatch(tag):
        # anything else must not publish at all
        errors.append(f"tag {tag!r} is not a release tag: full releases are v and a number (v3, v3.1), "
                      "prereleases are x and a number (x3, x3.1)")
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

    outputs = dict(version=tag, fw_version=fw, prerelease="true" if pre else "false", draft="false" if pre else "true", previous=prev or "")
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
          f"{', prerelease' if pre else ', full release'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
