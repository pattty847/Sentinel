#!/usr/bin/env bash
# Conductor runs this from the main checkout after landing the hook.
set -euo pipefail
common=$(git rev-parse --path-format=absolute --git-common-dir)
gitdir=$(git rev-parse --absolute-git-dir)
[[ "$common" == "$gitdir" ]] || { echo 'install-hooks: run from the main checkout' >&2; exit 1; }
repo=$(git rev-parse --show-toplevel)
[[ "$(git symbolic-ref --short HEAD)" == main ]] || { echo 'install-hooks: main checkout must be on main' >&2; exit 1; }
hooks=$(cd "$repo/scripts/dev/hooks" && pwd -P)
[[ -x "$hooks/pre-push" ]] || { echo 'install-hooks: executable pre-push hook missing' >&2; exit 1; }
git config --local core.hooksPath "$hooks"
# Preserve the first installation baseline: reinstalling must not exempt new commits.
if ! git config --local --get workflow.pushBaseline >/dev/null; then
    git config --local workflow.pushBaseline "$(git rev-parse refs/heads/main)"
fi
echo "install-hooks: $hooks (baseline $(git config --local --get workflow.pushBaseline))"
