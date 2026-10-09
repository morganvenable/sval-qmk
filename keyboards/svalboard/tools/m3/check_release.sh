#!/usr/bin/env bash
# Check a published release's assets (M3, D31 step 4): its state, then every
# .svup against its .uf2 and as a release build would check it.
#
#   bash keyboards/svalboard/tools/m3/check_release.sh TAG [OWNER/REPO]
#
# Downloads into ~/m3dry/TAG (kept: the hardware check uses these files).
# A dry-run tag must be a prerelease; any release must not be a draft and
# must hold exactly 12 .uf2 and 12 .svup files.
set -euo pipefail
tag="${1:?usage: check_release.sh TAG [OWNER/REPO]}"
R="${2:-morganvenable/sval-qmk}"
here="$(cd "$(dirname "$0")" && pwd)"
dir="$HOME/m3dry/$tag"

state="$(gh release view "$tag" -R "$R" --json isDraft,isPrerelease \
  -q '"draft=" + (.isDraft|tostring) + " prerelease=" + (.isPrerelease|tostring)')"
echo "$tag: $state"
case "$state" in *draft=true*) echo "FAIL: still a draft"; exit 1 ;; esac
if grep -qiE 'dry-?run' <<<"$tag" && [ "${state##*prerelease=}" != true ]; then
  echo "FAIL: a dry-run tag must be a prerelease"
  exit 1
fi
rm -rf "$dir"
mkdir -p "$dir"
gh release download "$tag" -R "$R" -D "$dir"
python3 -I "$here/check_release_assets.py" "$dir" "$tag"
