#!/bin/bash
# Deploy signed runtime binaries or restore the last deployed binary. Never run
# sentinel-server directly: it would contend with the launchd recorder's locks.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
RT="$HOME/Sentinel-runtime/bin"
IDENTITY="Sentinel Local Code Signing"
LOGS="$HOME/Library/Logs/Sentinel"
DRY_RUN=0
# The server needs longer than capture: a roller-served server is ready only
# after every product's first durable checkpoint, which follows its catch-up
# and the next minute boundary after a new commit (up to about two minutes).
SERVER_TRIES=75 # x 2 s
CAPTURE_TRIES=30

# Write marker in one log's content (stdin): the primary recorder started, or
# the serving roller's latest readiness transition is "ready" (every product
# healthy after a durable checkpoint, none lost since). "Roller started" and
# "Roller writer open" lines are diagnostics only.
server_ready() {
    awk '/Recording v2 started/ {primary = 1}
         /Roller serving ready products=/ {ready = 1}
         /Roller serving not ready product=/ {ready = 0}
         END {exit !(primary || ready)}'
}
capture_ready() { grep -q "Capture stats\|storedFrames\|Subscription confirmed"; }
# One open of one regular file, never through a symlink: the opened file must
# still be the inode the (non-symlink) path names after the open. A failed or
# short read (fewer bytes than the file held at open; logs only grow) fails.
read_log() { # path
    local opened size content status=0 LC_ALL=C
    [[ -f $1 && ! -L $1 ]] || return 1
    exec 3<"$1" || return 1
    opened=$(stat -L -f %i /dev/fd/3 2>/dev/null || true)
    size=$(stat -L -f %z /dev/fd/3 2>/dev/null || true)
    if [[ -L $1 || -z $opened || $opened != "$(stat -f %i "$1" 2>/dev/null || true)" || ! $size =~ ^[0-9]+$ ]]; then
        exec 3<&-
        return 1
    fi
    content=$(cat <&3 && printf x) || status=1
    exec 3<&-
    content=${content%x}
    [[ $status == 0 && ${#content} -ge $size ]] || return 1
    printf '%s' "$content"
}
# The content of exactly that process's log: named for its PID, header PID and
# exe (the deployed binary) checked in the same single read as the marker.
service_content() { # dir bin pid exe
    local log content
    [[ $3 =~ ^[0-9]+$ ]] || return 1
    for log in "$1/$2"-2*-"$3".log; do
        content=$(read_log "$log") || continue
        head -n 1 <<<"$content" | awk -v pid="pid=$3" '$NF == pid {ok = 1} END {exit !ok}' || continue
        head -n 5 <<<"$content" | awk -v exe="exe=$4" '$1 == "#" && $2 == exe {ok = 1} END {exit !ok}' || continue
        printf '%s\n' "$content"
        return 0
    done
    return 1
}
service_pid() { # label
    launchctl print "gui/$(id -u)/$1" 2>/dev/null | awk '$1 == "pid" && $2 == "=" {print $3; exit}' || true
}
verify_writes() { # name label binary
    local name=$1 label=$2 bin=$3 old pid content tries=$CAPTURE_TRIES
    [[ $name == server ]] && tries=$SERVER_TRIES
    old=$(service_pid "$label")
    launchctl kickstart -k "gui/$(id -u)/$label"
    for _ in $(seq 1 "$tries"); do
        sleep 2
        # The restarted service's PID, each time (launchd may restart it). The
        # replaced process has the same exe path: never accept its log.
        pid=$(service_pid "$label")
        [[ -n $pid && $pid != "$old" ]] || continue
        content=$(service_content "$LOGS" "$bin" "$pid" "$RT/$bin") || continue
        if [[ $name == server ]]; then
            server_ready <<<"$content" || continue
        else
            capture_ready <<<"$content" || continue
        fi
        # Accept only if launchd still runs that process after the read.
        [[ $(service_pid "$label") == "$pid" ]] || continue
        echo "$bin: writing (pid $pid)"
        return 0
    done
    return 1
}

change_binary() { # name label binary
    local name=$1 label=$2 bin=$3
    local src="$REPO/build/mac-clang/apps/$bin/$bin" dst="$RT/$bin"
    local rollback="$dst.rollback"
    [[ -x $dst ]] || { echo "missing deployed $dst" >&2; exit 1; }
    if [[ $ACTION == deploy ]]; then
        [[ -x $src ]] || { echo "missing $src (build first)" >&2; exit 1; }
    else
        [[ -x $rollback ]] || { echo "missing rollback $rollback" >&2; exit 1; }
    fi
    if (( DRY_RUN )); then
        if [[ $ACTION == deploy ]]; then
            echo "DRY RUN: copy $src to $dst.new; sign and verify $dst.new"
        else
            echo "DRY RUN: copy $rollback to $dst.new; verify signature on $dst.new"
        fi
        echo "DRY RUN: replace $dst; restart $label; verify writes within the window (server 150 s, capture 60 s); restore previous binary on failure"
        [[ $ACTION == deploy ]] && echo "DRY RUN: on success move $dst.prev to $rollback"
        echo "DRY RUN: rollback command: $0 rollback $name"
        return
    fi
    if [[ $ACTION == deploy ]]; then
        cp "$src" "$dst.new"
        codesign -f -s "$IDENTITY" --identifier "com.sentinel.$name" "$dst.new" >/dev/null
    else
        cp -p "$rollback" "$dst.new"
    fi
    codesign --verify --strict "$dst.new"
    cp -p "$dst" "$dst.prev"
    mv "$dst.new" "$dst"
    if verify_writes "$name" "$label" "$bin"; then
        if [[ $ACTION == deploy ]]; then
            mv "$dst.prev" "$rollback"
        else
            rm -f "$dst.prev"
        fi
        echo "$(git -C "$REPO" rev-parse --short HEAD) $(date '+%F %T') $ACTION $bin" >> "$RT/DEPLOYED"
        echo "$ACTION complete for $bin; rollback command: $0 rollback $name"
    else
        echo "$ACTION FAILED for $bin: no write within the window; restoring previous binary" >&2
        mv "$dst.prev" "$dst"
        launchctl kickstart -k "gui/$(id -u)/$label"
        exit 2
    fi
}

main() {
    if [[ ${1:-} == check-server-log ]]; then # check-server-log <dir> <pid> <exe>: tests and the runbook
        [[ $# == 4 ]] || { echo "usage: deploy-runtime.sh check-server-log <log dir> <pid> <exe>" >&2; exit 1; }
        local content
        content=$(service_content "$2" sentinel-server "$3" "$4") || exit 1
        server_ready <<<"$content"
        exit
    fi
    local args=() arg
    for arg in "$@"; do
        if [[ $arg == --dry-run ]]; then DRY_RUN=1; else args+=("$arg"); fi
    done
    set -- ${args[@]+"${args[@]}"}
    ACTION=deploy
    if [[ ${1:-} == rollback ]]; then ACTION=rollback; shift; fi
    WHICH=${1:-}
    if [[ $# != 1 || ! $WHICH =~ ^(server|capture|both)$ || ( $ACTION == rollback && $WHICH == both ) ]]; then
        echo "usage: deploy-runtime.sh [--dry-run] server|capture|both | [--dry-run] rollback server|capture" >&2
        exit 1
    fi
    case $WHICH in
        server) change_binary server com.sentinel.recorder sentinel-server ;;
        capture) change_binary capture com.sentinel.capture sentinel-capture ;;
        both) change_binary server com.sentinel.recorder sentinel-server
              change_binary capture com.sentinel.capture sentinel-capture ;;
    esac
}

# Sourcing defines the functions only (tests stub launchctl and sleep on PATH).
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
fi
