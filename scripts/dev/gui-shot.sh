#!/usr/bin/env bash
# gui-shot.sh: client for scripts/dev/gui-host.py. Lets a sandboxed agent (no window server)
# start the GUI, drive the Agent API and read screenshots.
#
#   scripts/dev/gui-shot.sh launch [--replace] [--fresh-profile] [--build <worktree>]
#   scripts/dev/gui-shot.sh shot <name> [--after <operationId>] [--settle] [--target window|<dock-id>|toolbar|chartmenu|settings[:Tab]]
#   scripts/dev/gui-shot.sh api GET|POST </api/v1/...> [json]       # state, viewport, heatmap/settings ...
#   scripts/dev/gui-shot.sh docks [list|focus <id>|show <id>|hide <id>]
#   scripts/dev/gui-shot.sh status | stop | profile-reset
#
# The host defaults to main's build. --build runs only the fixed sentinel-gui binary under an
# approved worktree root; build it through the queue first. The GUI cannot trade, switches only
# to recorded products, and refuses screen-region grabs (target=main).
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
        replace=false; fresh=false; build=
        while (( $# )); do
            case "$1" in
                --replace) replace=true; shift ;;
                --fresh-profile) fresh=true; shift ;;
                --build) build=${2:?--build needs an absolute worktree path}; shift 2 ;;
                *) die "unknown argument $1 (launch takes --replace, --fresh-profile and --build)" ;;
            esac
        done
        host POST /launch "$(jq -n --argjson p "$replace" --argjson f "$fresh" --arg b "$build" '{replace:$p, freshProfile:$f} + (if $b == "" then {} else {build:$b} end)')" ;;
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
    docks)
        action=${1:-list}; port=$(session_port)
        case "$action" in
            list) body= ; method=GET ;;
            focus) body=$(jq -n --arg id "${2:?focus needs a dock id}" '{focus:$id}'); method=POST ;;
            show|hide) body=$(jq -n --arg id "${2:?$action needs a dock id}" --argjson on "$([[ "$action" == show ]] && echo true || echo false)" '{visible:{($id):$on}}'); method=POST ;;
            *) die "docks takes list, focus <id>, show <id> or hide <id>" ;;
        esac
        if [[ "$method" == GET ]]; then curl -s --max-time 15 "http://127.0.0.1:$port/api/v1/docks"
        else curl -s --max-time 15 -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$port/api/v1/docks"; fi ;;
    status) host GET /status ;;
    stop) host POST /stop '{}' ;;
    profile-reset) host POST /profile-reset '{}' ;;
    *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
