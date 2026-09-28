#!/usr/bin/env bash
# agent-worktree.sh — isolated Sentinel worktrees with a ready build, for agents and humans.
#
#   scripts/dev/agent-worktree.sh create <branch> [base-ref]   # default base: main
#   scripts/dev/agent-worktree.sh remove <branch> [--force]
#   scripts/dev/agent-worktree.sh list
#
# Worktrees live on the external drive when it is mounted
# (SENTINEL_WORKTREE_ROOT, default /Volumes/T7/sentinel-worktrees), otherwise
# under <repo>/.claude/worktrees. External worktrees are locked so a
# `git worktree prune` while the drive is unplugged cannot drop them.
# Non-interactive shells lack VCPKG_ROOT; this script supplies it.
set -euo pipefail

COMMON_DIR=$(git rev-parse --path-format=absolute --git-common-dir)
REPO=$(dirname "$COMMON_DIR")
EXTERNAL_ROOT=${SENTINEL_WORKTREE_ROOT:-/Volumes/T7/sentinel-worktrees}
export VCPKG_ROOT=${VCPKG_ROOT:-$HOME/vcpkg}
NINJA=$(command -v ninja || echo /opt/homebrew/bin/ninja)

worktree_root() {
    local mount
    mount=$(dirname "$EXTERNAL_ROOT")
    if [[ -d "$mount" && -w "$mount" ]]; then
        mkdir -p "$EXTERNAL_ROOT"
        echo "$EXTERNAL_ROOT"
    else
        echo "warning: $mount not mounted; using $REPO/.claude/worktrees" >&2
        echo "$REPO/.claude/worktrees"
    fi
}

dir_for() {
    local root
    root=$(worktree_root)
    echo "$root/${1//\//-}"
}

path_of_branch() {
    git -C "$REPO" worktree list --porcelain |
        awk -v b="refs/heads/$1" '/^worktree /{p=substr($0,10)} $0=="branch " b {print p}'
}

cmd=${1:-}
case "$cmd" in
    create)
        branch=${2:?usage: create <branch> [base-ref]}
        base=${3:-main}
        dir=$(dir_for "$branch")
        [[ -e "$dir" ]] && { echo "error: $dir already exists" >&2; exit 1; }
        git -C "$REPO" worktree add -q -b "$branch" "$dir" "$base"
        if [[ "$dir" == "$EXTERNAL_ROOT"/* ]]; then
            git -C "$REPO" worktree lock --reason "external drive worktree" "$dir"
        fi
        (cd "$dir" && cmake --preset mac-clang -DCMAKE_MAKE_PROGRAM="$NINJA" > "$dir/.configure.log" 2>&1) || {
            echo "error: configure failed, see $dir/.configure.log" >&2; exit 1; }
        echo "ready: $dir"
        echo "  branch: $branch (from $base)"
        echo "  build:  cmake --build --preset mac-clang"
        echo "  test:   ctest --test-dir build/mac-clang"
        ;;
    remove)
        branch=${2:?usage: remove <branch> [--force]}
        dir=$(path_of_branch "$branch")
        [[ -z "$dir" ]] && { echo "error: no worktree for branch $branch" >&2; exit 1; }
        git -C "$REPO" worktree unlock "$dir" 2>/dev/null || true
        if [[ "${3:-}" == "--force" ]]; then
            git -C "$REPO" worktree remove --force "$dir"
        else
            git -C "$REPO" worktree remove "$dir"
        fi
        if git -C "$REPO" branch -d "$branch" 2>/dev/null; then
            echo "removed $dir and merged branch $branch"
        else
            echo "removed $dir; branch $branch kept (not merged into HEAD)"
        fi
        ;;
    list)
        git -C "$REPO" worktree list
        ;;
    *)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
