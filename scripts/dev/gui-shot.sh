#!/usr/bin/env bash
# gui-shot.sh: client for scripts/dev/gui-host.py. Lets a sandboxed agent (no window server)
# start the GUI, drive the Agent API and read screenshots.
#
#   scripts/dev/gui-shot.sh binaries                                  # what launch accepts
#   scripts/dev/gui-shot.sh launch [--binary main|<id>] [--renderer gpu|legacy] [--replace]
#   scripts/dev/gui-shot.sh shot <name> [--after <operationId>] [--settle] [--target heatmap|lab|telemetry|toolbar|settings[:Tab]]
#   scripts/dev/gui-shot.sh api GET|POST </api/v1/...> [json]       # state, viewport, heatmap/settings ...
#   scripts/dev/gui-shot.sh status | stop
#
# The host never runs a path you name: `main` is the main checkout's build, and an <id> is a build
# of a branch the orchestrator blessed after review (`gui-host.py bless <worktree>`). So your own
# unreviewed worktree build cannot be launched; ask the orchestrator to bless it after review.
# launch prints the session JSON with `port` (that GUI's Agent API) and `shotDir`. shot prints the
# absolute PNG path: read it directly (Codex and Claude both open local images). If the host is
# not running, ask the orchestrator to start it (scripts/dev/gui-host.py); nothing here starts it.
# Needs curl and jq. Screen grabs (target=main) are refused by the GUI itself in this mode.
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
        binary=main; renderer=gpu; replace=false
        while (( $# )); do
            case "$1" in
                --binary) binary=${2:?--binary needs main|<id>}; shift 2 ;;
                --renderer) renderer=${2:?--renderer needs gpu|legacy}; shift 2 ;;
                --replace) replace=true; shift ;;
                *) die "unknown argument $1 (launch takes --binary main|<id>, never a path)" ;;
            esac
        done
        host POST /launch "$(jq -n --arg b "$binary" --arg r "$renderer" --argjson p "$replace" \
            '{binary:$b, renderer:$r, replace:$p}')" ;;
    binaries) host GET /binaries ;;
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
