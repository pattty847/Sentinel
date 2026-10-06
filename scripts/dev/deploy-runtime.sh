#!/bin/bash
# Deploy signed runtime binaries or restore the last deployed binary. Never run
# sentinel-server directly: it would contend with the launchd recorder's locks.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
RT="$HOME/Sentinel-runtime/bin"
IDENTITY="Sentinel Local Code Signing"
LOGS="$HOME/Library/Logs/Sentinel"
DRY_RUN=0
# Server write marker: the primary recorder started, or (recording.source:
# roller) every configured product's history writer opened. "Roller started"
# lines are diagnostics only: they precede the leases and checkpoint checks.
server_writing() { grep -Eq "Recording v2 started|Roller serving ready products=" "$1"; }
# The log of exactly that process: named and headed with its PID, headed with
# the executable that was deployed (never a newer log of another process).
service_log() { # dir bin pid exe
    local log
    [[ $3 =~ ^[0-9]+$ ]] || return 1
    for log in "$1/$2"-2*-"$3".log; do
        [[ -f $log ]] || continue
        head -n 1 "$log" | awk -v pid="pid=$3" '$NF == pid {ok = 1} END {exit !ok}' || continue
        head -n 5 "$log" | awk -v exe="exe=$4" '$1 == "#" && $2 == exe {ok = 1} END {exit !ok}' || continue
        echo "$log"
        return 0
    done
    return 1
}
if [[ ${1:-} == check-server-log ]]; then # check-server-log <dir> <pid> <exe>: tests and the runbook
    [[ $# == 4 ]] || { echo "usage: deploy-runtime.sh check-server-log <log dir> <pid> <exe>" >&2; exit 1; }
    log=$(service_log "$2" sentinel-server "$3" "$4") && server_writing "$log"; exit
fi
args=()
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

service_pid() { # label
    launchctl print "gui/$(id -u)/$1" 2>/dev/null | awk '$1 == "pid" && $2 == "=" {print $3; exit}' || true
}
verify_writes() { # name label binary
    local name=$1 label=$2 bin=$3 old pid log
    old=$(service_pid "$label")
    launchctl kickstart -k "gui/$(id -u)/$label"
    for _ in $(seq 1 30); do
        sleep 2
        # The restarted service's PID, each time (launchd may restart it). The
        # replaced process has the same exe path: never accept its log.
        pid=$(service_pid "$label")
        [[ -n $pid && $pid != "$old" ]] || continue
        log=$(service_log "$LOGS" "$bin" "$pid" "$RT/$bin") || continue
        if [[ $name == server ]] && server_writing "$log"; then
            echo "$bin: writing (log $(basename "$log"))"; return 0
        fi
        if [[ $name == capture ]] && grep -q "Capture stats\|storedFrames\|Subscription confirmed" "$log"; then
            echo "$bin: writing (log $(basename "$log"))"; return 0
        fi
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
        echo "DRY RUN: replace $dst; restart $label; verify writes within 60 s; restore previous binary on failure"
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
        echo "$ACTION FAILED for $bin: no write within 60 s; restoring previous binary" >&2
        mv "$dst.prev" "$dst"
        launchctl kickstart -k "gui/$(id -u)/$label"
        exit 2
    fi
}

case $WHICH in
    server) change_binary server com.sentinel.recorder sentinel-server ;;
    capture) change_binary capture com.sentinel.capture sentinel-capture ;;
    both) change_binary server com.sentinel.recorder sentinel-server
          change_binary capture com.sentinel.capture sentinel-capture ;;
esac
