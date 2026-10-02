#!/usr/bin/env bash
# gui-shot.sh: client for scripts/dev/gui-host.py. Lets a sandboxed agent (no window server)
# start the GUI from its own worktree build, drive the Agent API and read screenshots.
#
#   scripts/dev/gui-shot.sh launch [worktree] [--renderer gpu|legacy] [--replace]
#   scripts/dev/gui-shot.sh shot <name> [--after <operationId>] [--settle] [--target heatmap|telemetry|toolbar|settings[:Tab]]
#   scripts/dev/gui-shot.sh api GET|POST </api/v1/...> [json]       # state, viewport, heatmap/settings ...
#   scripts/dev/gui-shot.sh status | stop
#
# launch defaults to the current worktree (build it first). It prints the session JSON, with
# `port` (the Agent API of that GUI) and `shotDir`. shot prints the absolute PNG path: read it
# directly (Codex and Claude both open local images). If the host is not running, ask the
# orchestrator to start it (scripts/dev/gui-host.py); nothing here starts it.
# Needs curl and jq. Privacy: never target=main (the host refuses it).
set -euo pipefail

HOST=${GUI_HOST_URL:-http://127.0.0.1:${GUI_HOST_PORT:-17190}}
die() { echo "gui-shot: $*" >&2; exit 1; }
host() { # <GET|POST> <path> [json]
    local out data=()
    [[ -z "${3:-}" ]] || data=(-d "$3")
    out=$(curl -s --max-time 120 -X "$1" -H 'X-Gui-Host: 1' -H 'Content-Type: application/json' \
        ${data[@]+"${data[@]}"} "$HOST$2") || die "gui-host not reachable at $HOST (ask the orchestrator to run scripts/dev/gui-host.py)"
    echo "$out"
    [[ "$(jq -r '.ok // false' <<<"$out")" == true ]] || return 1
}
session_port() {
    local p
    p=$(curl -s --max-time 5 "$HOST/status" | jq -r '.session.port // empty') || true
    [[ -n "$p" ]] || die "no running GUI session (gui-shot.sh launch first)"
    echo "$p"
}

cmd=${1:-}; shift || true
case "$cmd" in
    launch)
        wt=$(git rev-parse --show-toplevel 2>/dev/null || pwd); renderer=gpu; replace=false
        while (( $# )); do
            case "$1" in
                --renderer) renderer=${2:?--renderer needs gpu|legacy}; shift 2 ;;
                --replace) replace=true; shift ;;
                -*) die "unknown flag $1" ;;
                *) wt=$1; shift ;;
            esac
        done
        wt=$(cd "$wt" && pwd -P) || die "no such worktree: $wt" # the host resolves paths from its own cwd
        host POST /launch "$(jq -n --arg w "$wt" --arg r "$renderer" --argjson p "$replace" \
            '{worktree:$w, renderer:$r, replace:$p}')" ;;
    shot)
        name=${1:?usage: shot <name> [--after <op>] [--settle] [--target T]}; shift
        body=$(jq -n --arg n "$name" '{name:$n}')
        while (( $# )); do
            case "$1" in
                --after) body=$(jq --arg v "${2:?}" '.afterOperation=$v' <<<"$body"); shift 2 ;;
                --settle) body=$(jq '.settle=true' <<<"$body"); shift ;;
                --target) body=$(jq --arg v "${2:?}" '.target=$v' <<<"$body"); shift 2 ;;
                *) die "unknown flag $1" ;;
            esac
        done
        host POST /shot "$body" ;;
    api)
        method=${1:?usage: api GET|POST <path> [json]}; path=${2:?usage: api GET|POST <path> [json]}
        [[ "$path" != *screenshot* ]] || die "use 'shot' for screenshots (it enforces the privacy targets and the 1/s limit)"
        port=$(session_port); data=()
        [[ -z "${3:-}" ]] || data=(-H 'Content-Type: application/json' -d "$3")
        curl -s --max-time 15 -X "$method" ${data[@]+"${data[@]}"} "http://127.0.0.1:$port$path" ;;
    status) host GET /status ;;
    stop) host POST /stop '{}' ;;
    *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
