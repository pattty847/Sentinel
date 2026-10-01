#!/bin/bash
# Deploy sentinel-server and/or sentinel-capture from build/mac-clang into
# ~/Sentinel-runtime/bin, sign them with the local "Sentinel Local Code Signing"
# identity (so macOS Full Disk Access keyed to identifier + certificate survives
# the new binary), restart the launchd service and verify it writes within 60 s.
#
#   scripts/dev/deploy-runtime.sh server|capture|both
#
# Never run a bare sentinel-server binary to smoke-test it: from the repo root it
# would contend for the recorder's data locks. On a failed verification the
# script restores the previous binary and restarts it.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
RT="$HOME/Sentinel-runtime/bin"
IDENTITY="Sentinel Local Code Signing"
LOGS="$HOME/Library/Logs/Sentinel"
which=${1:?usage: deploy-runtime.sh server|capture|both}

deploy() { # name label binary marker-pattern
    local name=$1 label=$2 bin=$3
    local src="$REPO/build/mac-clang/apps/$bin/$bin" dst="$RT/$bin"
    [ -x "$src" ] || { echo "missing $src (build first)"; exit 1; }
    cp "$dst" "$dst.prev"
    cp "$src" "$dst.new"
    codesign -f -s "$IDENTITY" --identifier "com.sentinel.$name" "$dst.new" >/dev/null
    codesign --verify --strict "$dst.new"
    mv "$dst.new" "$dst"
    echo "$(git -C "$REPO" rev-parse --short HEAD) $(date '+%F %T') $bin" >> "$RT/DEPLOYED"
    local before
    before=$(ls -t "$LOGS"/"$bin"-2*.log 2>/dev/null | head -1 || true)
    launchctl kickstart -k "gui/$(id -u)/$label"
    local ok=0
    for _ in $(seq 1 30); do
        sleep 2
        local log
        log=$(ls -t "$LOGS"/"$bin"-2*.log 2>/dev/null | head -1 || true)
        [ -n "$log" ] && [ "$log" != "$before" ] || continue
        if [ "$name" = server ] && grep -q "Recording v2 started" "$log"; then ok=1; break; fi
        if [ "$name" = capture ] && grep -q "Capture stats\|storedFrames\|Subscription confirmed" "$log"; then ok=1; break; fi
    done
    if [ $ok = 1 ]; then
        echo "deployed $bin: writing (log $(basename "$log"))"
        rm -f "$dst.prev"
    else
        echo "DEPLOY FAILED for $bin: no write within 60 s; restoring previous binary" >&2
        mv "$dst.prev" "$dst"
        launchctl kickstart -k "gui/$(id -u)/$label"
        exit 2
    fi
}

case $which in
    server) deploy server com.sentinel.recorder sentinel-server ;;
    capture) deploy capture com.sentinel.capture sentinel-capture ;;
    both) deploy server com.sentinel.recorder sentinel-server; deploy capture com.sentinel.capture sentinel-capture ;;
    *) echo "usage: deploy-runtime.sh server|capture|both"; exit 1 ;;
esac
