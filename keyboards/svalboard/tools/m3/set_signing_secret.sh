#!/usr/bin/env bash
# Put a release signing key in the release-signing environment's secret
# SVAL_UPDATE_SIGNING_KEY (D29), without printing it. Refuses a key that is
# not one of the two release key slots in updater/update_release_keys.h, and
# the TEST-ONLY key.
#
#   bash keyboards/svalboard/tools/m3/set_signing_secret.sh KEYFILE [OWNER/REPO]
#
# KEYFILE is a make_update.py --genkey seed file (for the dry run:
# ~/m3-keys/dryrun-release-1.key). Only its seed line is sent, on stdin.
set -euo pipefail
key="${1:?usage: set_signing_secret.sh KEYFILE [OWNER/REPO]}"
R="${2:-morganvenable/sval-qmk}"
here="$(cd "$(dirname "$0")/.." && pwd)"
MU="$here/make_update.py"

pub="$(python3 -I "$MU" --print-public --key "$key")"
if [ "$pub" = "$(python3 -I "$MU" --print-public)" ]; then
  echo "refused: $key is the TEST-ONLY key" >&2
  exit 1
fi
slot=0
n=1
for k in $(python3 -I "$MU" --print-release-keys); do
  [ "$k" = "$pub" ] && slot=$n
  n=$((n + 1))
done
if [ "$slot" = 0 ]; then
  echo "refused: $key is neither release key in update_release_keys.h" >&2
  exit 1
fi
grep -v '^#' "$key" | grep -E '^[0-9a-fA-F]{64}$' \
  | gh secret set SVAL_UPDATE_SIGNING_KEY --env release-signing -R "$R"
echo "set SVAL_UPDATE_SIGNING_KEY (release key $slot, public $pub) on $R"
gh secret list --env release-signing -R "$R"
