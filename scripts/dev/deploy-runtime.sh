#!/bin/bash
# Deploy signed runtime binaries or restore the last deployed binary. Never run
# sentinel-server directly: it would contend with the launchd recorder's locks.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
RT="$HOME/Sentinel-runtime/bin"
IDENTITY="Sentinel Local Code Signing"
LOGS="$HOME/Library/Logs/Sentinel"
DRY_RUN=0
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

verify_writes() { # name label binary previous-log
    local name=$1 label=$2 bin=$3 before=$4 log
    launchctl kickstart -k "gui/$(id -u)/$label"
    for _ in $(seq 1 30); do
        sleep 2
        log=$(ls -t "$LOGS"/"$bin"-2*.log 2>/dev/null | head -1 || true)
        [[ -n $log && $log != "$before" ]] || continue
        if [[ $name == server ]] && grep -q "Recording v2 started" "$log"; then
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
    local rollback="$dst.rollback" before
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
    before=$(ls -t "$LOGS"/"$bin"-2*.log 2>/dev/null | head -1 || true)
    mv "$dst.new" "$dst"
    if verify_writes "$name" "$label" "$bin" "$before"; then
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
