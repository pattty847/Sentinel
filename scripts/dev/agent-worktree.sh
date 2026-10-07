#!/usr/bin/env bash
# agent-worktree.sh — isolated Sentinel worktrees with a ready build, for agents and humans.
#
#   scripts/dev/agent-worktree.sh create <branch> [base-ref]   # default base: main
#   scripts/dev/agent-worktree.sh remove <branch> [--force]
#   scripts/dev/agent-worktree.sh land <branch> [--yes] [-m <message>] [--trailer 'Key: value']...
#   scripts/dev/agent-worktree.sh list
#
# land = the merge queue, one branch at a time: rebase the branch onto the
# current main (rerere on), build and run ctest in its worktree, stop for review
# if the rebase changed any commit (git range-diff), refuse if main moved, then
# merge --no-ff into main and remove the worktree. The rebased tip contains main,
# so the tested tree is exactly what lands. Run it from the main checkout.
# --trailer (repeatable) appends git trailers to the merge commit for the workflow
# audit (AGENTS.md section 10); read them with
# git log --first-parent main --format='%h %s%n%(trailers:only,unfold)'.
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
        # _agent/ (invariants, failure modes, decisions) is gitignored; link the
        # shared copy so agents read current rules and record into one place.
        if [[ -d "$REPO/_agent" && ! -e "$dir/_agent" ]]; then
            ln -s "$REPO/_agent" "$dir/_agent"
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
    land)
        branch=${2:?usage: land <branch> [--yes] [-m <message>] [--trailer 'Key: value']...}
        shift 2
        confirmed=0
        message="merge: $branch"
        trailers=""
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --yes) confirmed=1; shift ;;
                -m) message=${2:?-m needs a message}; shift 2 ;;
                --trailer)
                    t=${2:?--trailer needs 'Key: value'}
                    [[ "$t" =~ ^[A-Za-z][A-Za-z0-9-]*:\ [^[:cntrl:]]+$ ]] ||
                        { echo "error: bad trailer '$t' (want 'Key: value' on one line)" >&2; exit 2; }
                    trailers+="$t"$'\n'; shift 2 ;;
                *) echo "error: unknown option $1" >&2; exit 2 ;;
            esac
        done
        [[ "$(git -C "$REPO" symbolic-ref --short HEAD)" == "main" ]] ||
            { echo "error: $REPO is not on main" >&2; exit 1; }
        git -C "$REPO" diff --quiet && git -C "$REPO" diff --cached --quiet ||
            { echo "error: main checkout has uncommitted changes to tracked files" >&2; exit 1; }
        dir=$(path_of_branch "$branch")
        [[ -z "$dir" ]] && { echo "error: no worktree for branch $branch" >&2; exit 1; }
        [[ -z "$(git -C "$dir" status --porcelain --untracked-files=no)" ]] ||
            { echo "error: $dir has uncommitted changes" >&2; exit 1; }
        main_tip=$(git -C "$REPO" rev-parse main)
        old_tip=$(git -C "$dir" rev-parse HEAD)
        old_base=$(git -C "$dir" merge-base HEAD main)
        if [[ "$old_base" != "$main_tip" ]]; then
            echo "rebasing $branch onto main ($main_tip)..."
            if ! git -C "$dir" -c rerere.enabled=true -c rerere.autoUpdate=true rebase main; then
                git -C "$dir" rebase --abort || true
                echo "error: rebase conflicts. Resolve in $dir (git rebase main), commit, then rerun land." >&2
                exit 1
            fi
        fi
        new_tip=$(git -C "$dir" rev-parse HEAD)
        echo "building and testing $branch at $new_tip..."
        (cd "$dir" && "$REPO/scripts/dev/build-queue.sh" --label "land $branch (build)" -- cmake --build --preset mac-clang -j 2 > "$dir/.land-build.log" 2>&1) ||
            { echo "error: build failed, see $dir/.land-build.log" >&2; exit 1; }
        (cd "$dir" && "$REPO/scripts/dev/build-queue.sh" --label "land $branch (ctest)" -- ctest --test-dir build/mac-clang --output-on-failure --timeout 600 > "$dir/.land-ctest.log" 2>&1) ||
            { tail -20 "$dir/.land-ctest.log" >&2; echo "error: ctest failed, see $dir/.land-ctest.log" >&2; exit 1; }
        grep -E "tests passed" "$dir/.land-ctest.log" || true
        if [[ "$old_tip" != "$new_tip" ]]; then
            changed=$(git -C "$REPO" range-diff "$old_base..$old_tip" "$main_tip..$new_tip" | grep -c '^[0-9-]*: *[0-9a-f-]* ! ' || true)
            if [[ "$changed" -gt 0 && "$confirmed" -eq 0 ]]; then
                git -C "$REPO" range-diff "$old_base..$old_tip" "$main_tip..$new_tip"
                echo "stop: the rebase changed $changed commit(s) (above). Review, then rerun with --yes." >&2
                exit 3
            fi
        fi
        [[ "$(git -C "$REPO" rev-parse main)" == "$main_tip" ]] ||
            { echo "error: main moved during land; rerun land" >&2; exit 1; }
        if [[ -n "$trailers" ]]; then
            git -C "$REPO" merge --no-ff -q "$branch" -m "$message" -m "${trailers%$'\n'}"
        else
            git -C "$REPO" merge --no-ff -q "$branch" -m "$message"
        fi
        echo "landed $branch as $(git -C "$REPO" rev-parse --short HEAD)"
        "$0" remove "$branch"
        echo "queue rule: rebase and retest every other READY branch before it lands (run land on each)."
        ;;
    list)
        git -C "$REPO" worktree list
        ;;
    *)
        sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
