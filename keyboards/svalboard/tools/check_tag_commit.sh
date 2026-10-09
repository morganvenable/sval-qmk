#!/usr/bin/env bash
# Release CI (release.yml, M3): fail unless this checkout is the run's commit
# and the tag that started the run still names that commit. Every job checks
# out github.sha, so a tag moved while the run waits for the signing approval
# cannot change what it builds or signs; this stops the run from publishing
# under a tag that now names something else.
#
#   GH_TOKEN=... GITHUB_REPOSITORY=o/r GITHUB_REF_NAME=tag GITHUB_SHA=sha \
#     bash keyboards/svalboard/tools/check_tag_commit.sh
set -euo pipefail
: "${GITHUB_REPOSITORY:?}" "${GITHUB_REF_NAME:?}" "${GITHUB_SHA:?}"

head="$(git rev-parse HEAD)"
if [ "$head" != "$GITHUB_SHA" ]; then
  echo "::error::checked out $head, but this run is for $GITHUB_SHA"
  exit 1
fi

api="repos/$GITHUB_REPOSITORY/git"
obj="$(gh api "$api/ref/tags/$GITHUB_REF_NAME" -q '.object.type + " " + .object.sha')"
type="${obj%% *}"
sha="${obj#* }"
# An annotated tag points at a tag object; follow it to the commit.
while [ "$type" = tag ]; do
  obj="$(gh api "$api/tags/$sha" -q '.object.type + " " + .object.sha')"
  type="${obj%% *}"
  sha="${obj#* }"
done
if [ "$sha" != "$GITHUB_SHA" ]; then
  echo "::error::tag $GITHUB_REF_NAME now names $sha; this run built $GITHUB_SHA"
  exit 1
fi
echo "ok: tag $GITHUB_REF_NAME names $GITHUB_SHA, the commit checked out"
