#!/usr/bin/env bash
# Append one local SentinelBar milestone with a unique id.
# Target: $SENTINEL_EVENTS_FILE if set, else the primary journal. When that is not
# writable (Codex sandbox) the line goes to <worktree>/.sentinel-events.jsonl of the
# linked worktree the caller runs in; `agent-worktree.sh land` moves it into the
# primary journal before it removes the worktree. No writable target exits 1.
set -u
[[ $# -ge 3 ]] || { echo "usage: sentinel-event.sh <kind> <tone> <text> [--slice id] [--branch name]" >&2; exit 2; }
kind=$1 tone=$2 message=$3
shift 3
slice= branch=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --slice) [[ $# -ge 2 ]] || exit 2; slice=$2; shift 2 ;;
        --branch) [[ $# -ge 2 ]] || exit 2; branch=$2; shift 2 ;;
        *) echo "sentinel-event: unknown option $1" >&2; exit 2 ;;
    esac
done
case "$tone" in info|ok|warn|bad) ;; *) echo "sentinel-event: invalid tone" >&2; exit 2 ;; esac
json_string() {
    local value=$1
    local backslash='\'
    value=${value//"$backslash"/"$backslash$backslash"}
    value=${value//\"/\\\"}
    value=${value//$'\n'/\\n}
    value=${value//$'\r'/\\r}
    value=${value//$'\t'/\\t}
    value=${value//[$'\001'-$'\037']/}
    printf '"%s"' "$value"
}
at=$(( $(date +%s) * 1000 ))
id=$(printf '%s-%s-%04x%04x' "$at" "$$" "$RANDOM" "$RANDOM")
line="{\"id\":$(json_string "$id"),\"at\":$at,\"kind\":$(json_string "$kind"),\"text\":$(json_string "$message"),\"tone\":$(json_string "$tone")"
[[ -z "$slice" ]] || line+=",\"slice\":$(json_string "$slice")"
[[ -z "$branch" ]] || line+=",\"branch\":$(json_string "$branch")"
line+='}'

append() { mkdir -p "${1%/*}" 2>/dev/null && (printf '%s\n' "$line" >> "$1") 2>/dev/null; }

primary=${SENTINEL_EVENTS_FILE:-"$HOME/Library/Application Support/Sentinel/events.jsonl"}
append "$primary" && exit 0

# Fallback only inside a linked worktree of the repository this script belongs to.
script_common=$(git -C "$(dirname "$0")" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
top=$(git rev-parse --show-toplevel 2>/dev/null)
git_dir=$(git rev-parse --path-format=absolute --git-dir 2>/dev/null)
common=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
if [[ -n "$top" && -n "$common" && "$git_dir" != "$common" && "$common" == "$script_common" ]]; then
    append "$top/.sentinel-events.jsonl" && exit 0
    echo "sentinel-event: cannot write $primary or $top/.sentinel-events.jsonl; event lost: $kind" >&2
else
    echo "sentinel-event: cannot write $primary and $PWD is not a linked worktree; event lost: $kind" >&2
fi
exit 1
