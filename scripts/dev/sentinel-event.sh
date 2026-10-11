#!/usr/bin/env bash
# Append one local SentinelBar milestone. Failure of the primary path falls back to /tmp.
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
    printf '"%s"' "$value"
}
at=$(( $(date +%s) * 1000 ))
line="{\"at\":$at,\"kind\":$(json_string "$kind"),\"text\":$(json_string "$message"),\"tone\":$(json_string "$tone")"
[[ -z "$slice" ]] || line+=",\"slice\":$(json_string "$slice")"
[[ -z "$branch" ]] || line+=",\"branch\":$(json_string "$branch")"
line+='}'
primary="$HOME/Library/Application Support/Sentinel/events.jsonl"
if mkdir -p "${primary%/*}" 2>/dev/null && (printf '%s\n' "$line" >> "$primary") 2>/dev/null; then exit 0; fi
printf '%s\n' "$line" >> /tmp/sentinel-events.jsonl
