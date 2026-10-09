#!/usr/bin/env bash
# M3 fork setup (D29, D31; docs/updater.md, "Fork setup and the dry run").
# Run by hand, once, by an admin of the repository. Needs gh, logged in.
#
#   bash keyboards/svalboard/tools/m3/setup_fork.sh [OWNER/REPO] [REVIEWER]
#
# 1. The environment release-signing: REVIEWER (default morganvenable) must
#    approve every run, and only v* tags may deploy to it.
# 2. A tag ruleset "release tags": v* tags cannot be moved (force-pushed) or
#    deleted, except by repository admins. Release CI also checks that the
#    tag still names the run's commit, but this stops the move itself.
# Prints both back. Safe to run again: it updates instead of duplicating.
set -euo pipefail
R="${1:-morganvenable/sval-qmk}"
WHO="${2:-morganvenable}"
E="repos/$R/environments/release-signing"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

id="$(gh api "users/$WHO" -q .id)"
cat > "$tmp/env.json" <<EOF
{
  "wait_timer": 0,
  "prevent_self_review": false,
  "reviewers": [{"type": "User", "id": $id}],
  "deployment_branch_policy": {"protected_branches": false, "custom_branch_policies": true}
}
EOF
gh api -X PUT "$E" --input "$tmp/env.json" >/dev/null
have="$(gh api "$E/deployment-branch-policies" -q '.branch_policies[] | .type + ":" + .name')"
if ! grep -qx 'tag:v\*' <<<"$have"; then
  gh api -X POST "$E/deployment-branch-policies" -f name='v*' -f type=tag >/dev/null
fi

cat > "$tmp/ruleset.json" <<'EOF'
{
  "name": "release tags",
  "target": "tag",
  "enforcement": "active",
  "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
  "rules": [{"type": "update"}, {"type": "deletion"}],
  "bypass_actors": [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}]
}
EOF
rs="$(gh api "repos/$R/rulesets" -q '.[] | select(.name == "release tags") | .id')"
if [ -n "$rs" ]; then
  gh api -X PUT "repos/$R/rulesets/$rs" --input "$tmp/ruleset.json" >/dev/null
else
  gh api -X POST "repos/$R/rulesets" --input "$tmp/ruleset.json" >/dev/null
fi

echo "environment release-signing on $R:"
gh api "$E" -q '{reviewers: [.protection_rules[]? | select(.type == "required_reviewers") | .reviewers[].reviewer.login], prevent_self_review: ([.protection_rules[]? | .prevent_self_review] | map(select(. != null))), policy: .deployment_branch_policy}'
gh api "$E/deployment-branch-policies" -q '.branch_policies[] | "  deploys from " + .type + " " + .name'
echo "ruleset:"
gh api "repos/$R/rulesets" -q '.[] | select(.name == "release tags") | "  " + .name + " (" + .target + ", " + .enforcement + ")"'
