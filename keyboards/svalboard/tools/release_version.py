#!/usr/bin/env python3
"""The release version check (proposed D32), run by release CI before any build.

    release_version.py TAG --repo OWNER/REPO [--releases FILE] [--github-output FILE]

Launch tags are names (vLaunch2), not numbers, so the numeric firmware version
(SVAL_FW_VERSION, D17) is committed in keyboards/svalboard/updater/
fw_version.txt and the version string is the tag name. This checks, and exits
1 on any failure:

  - the tag is a usable version string: at most 16 of A-Z a-z 0-9 . _ + -
    (it goes into the manifest and the firmware's 16-byte version field);
  - the committed number is greater than the one committed at the previous
    published release: the newest release that is neither a draft nor a
    prerelease and is not this tag. A release from before this file existed
    counts as 0. Dry-run prereleases are not compared against, so a dry run
    never uses up a number;
  - a tag that is not a dry run is refused while update_release_keys.h holds
    the DRY RUN keys, so no normal release is ever signed with them.

A dry-run tag is one whose name contains "dryrun" or "dry-run" (any case),
such as vM3-dryrun1. Its release is always a prerelease.

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
sys.path.insert(0, str(HERE))
import make_update as mu  # noqa: E402  (release_keys)

VERSION_FILE = "keyboards/svalboard/updater/fw_version.txt"
REPO_ROOT = HERE.parents[2]
DRY_RUN_RE = re.compile(r"dry-?run", re.IGNORECASE)
MAX_FW = 999999999  # rules.mk takes at most 9 digits


def parse_version_file(text, where):
    nums = [ln.strip() for ln in text.splitlines() if ln.strip() and not ln.strip().startswith("#")]
    if len(nums) != 1 or not re.fullmatch(r"[0-9]{1,9}", nums[0]):
        raise ValueError(f"{where}: expected one line with a number of at most 9 digits")
    return int(nums[0])


def committed_at(tag):
    """The number committed at a tag, or 0 if the file did not exist there yet."""
    p = subprocess.run(["git", "-C", str(REPO_ROOT), "show", f"refs/tags/{tag}:{VERSION_FILE}"],
                       capture_output=True, text=True)
    if p.returncode != 0:
        exists = subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}"],
                                capture_output=True, text=True)
        if exists.returncode != 0:
            raise ValueError(f"the previous release's tag {tag} is not in this checkout (fetch tags: fetch-depth 0)")
        return 0
    return parse_version_file(p.stdout, f"{tag}:{VERSION_FILE}")


def releases(repo, path):
    if path:
        text = Path(path).read_text()
    else:
        text = subprocess.run(["gh", "api", "--paginate", f"repos/{repo}/releases", "--jq",
                               '.[] | [.tag_name, .draft, .prerelease, .published_at] | @tsv'],
                              capture_output=True, text=True, check=True).stdout
    out = []
    for line in text.splitlines():
        if line.strip():
            tag, draft, pre, published = (line.split("\t") + ["", "", ""])[:4]
            out.append(dict(tag=tag, draft=draft == "true", prerelease=pre == "true", published=published))
    return out


def previous_release(rels, tag):
    done = [r for r in rels if not r["draft"] and not r["prerelease"] and r["tag"] != tag and r["published"]]
    return max(done, key=lambda r: r["published"])["tag"] if done else None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tag")
    ap.add_argument("--repo", required=True)
    ap.add_argument("--releases", help="tab-separated release list instead of gh api (tests)")
    ap.add_argument("--github-output", help="append the outputs here ($GITHUB_OUTPUT)")
    args = ap.parse_args(argv)
    errors = []

    tag = args.tag
    if not mu.VERSION_RE.match(tag) or not tag:
        errors.append(f"tag {tag!r} is not a version string: at most 16 of A-Z a-z 0-9 . _ + -")
    dry_run = bool(DRY_RUN_RE.search(tag))
    try:
        fw = parse_version_file((REPO_ROOT / VERSION_FILE).read_text(), VERSION_FILE)
    except (OSError, ValueError) as e:
        print(f"FAIL: {e}")
        return 1
    _, keys_dry_run = mu.release_keys()
    if keys_dry_run and not dry_run:
        errors.append(f"{tag} is not a dry-run tag, but update_release_keys.h holds the DRY RUN keys: "
                      "swap in the production keys first (docs/updater.md, 'Release signing')")

    try:
        prev = previous_release(releases(args.repo, args.releases), tag)
        prev_fw = committed_at(prev) if prev else 0
    except (subprocess.CalledProcessError, ValueError) as e:
        print(f"FAIL: cannot read the previous release: {e}")
        return 1
    if fw <= prev_fw:
        errors.append(f"{VERSION_FILE} says {fw}, not greater than {prev_fw} at the previous release {prev}: raise it")

    outputs = dict(version=tag, fw_version=fw, prerelease="true" if dry_run else "false", previous=prev or "")
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
          f"{', a dry run: prerelease' if dry_run else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
