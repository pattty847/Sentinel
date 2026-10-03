#!/bin/bash
# FIFO build queue for agents on the owner's 16 GB Mac: one build or test run at a time,
# in arrival order, and every waiter knows its place in line.
#
#   scripts/dev/build-queue.sh [--label <name>] -- <command...>   wait for your turn, run, release
#   scripts/dev/build-queue.sh status                             show the line
#
# Example: scripts/dev/build-queue.sh --label lt-sol/x -- cmake --build --preset mac-clang -j 4
#
# Tickets live in /tmp (writable from Codex sandboxes and Claude agents alike). Each ticket is
# refreshed every 10 s while its owner waits or runs; a ticket not refreshed for 120 s is stale
# (its owner died) and is removed, so a crashed agent never blocks the line.
set -uo pipefail
QDIR=${SENTINEL_BUILD_QUEUE:-/tmp/sentinel-build-queue}
STALE_S=120
mkdir -p "$QDIR"

now_ns() { python3 -c 'import time; print(time.time_ns())'; }
age_s() { echo $(( $(date +%s) - $(stat -f %m "$1" 2>/dev/null || echo 0) )); }
prune() {
    for t in "$QDIR"/*.ticket; do
        [ -e "$t" ] || continue
        [ "$(age_s "$t")" -gt "$STALE_S" ] && rm -f "$t" && echo "build-queue: removed stale ticket $(basename "$t")" >&2
    done
}
line() { ls "$QDIR"/*.ticket 2>/dev/null | sort; }
label_of() { sed -n 1p "$1" 2>/dev/null; }

if [ "${1:-}" = status ]; then
    prune
    i=0
    for t in $(line); do
        i=$((i + 1))
        state=waiting; [ $i -eq 1 ] && state=running
        echo "$i. $(label_of "$t") ($state, $(age_s "$t")s since heartbeat)"
    done
    [ $i -eq 0 ] && echo "build-queue: empty"
    exit 0
fi

label="$(basename "$PWD")"
[ "${1:-}" = --label ] && { label=$2; shift 2; }
[ "${1:-}" = -- ] && shift
[ $# -gt 0 ] || { echo "usage: build-queue.sh [--label name] -- <command...> | status" >&2; exit 2; }

ticket="$QDIR/$(now_ns)-$$.ticket"
echo "$label" > "$ticket"
( while [ -e "$ticket" ]; do touch "$ticket" 2>/dev/null; sleep 10; done ) &
heartbeat=$!
cleanup() { rm -f "$ticket"; kill "$heartbeat" 2>/dev/null; }
trap cleanup EXIT INT TERM

last=""
while :; do
    prune
    tickets=($(line))
    pos=0
    for i in "${!tickets[@]}"; do [ "${tickets[$i]}" = "$ticket" ] && pos=$((i + 1)); done
    [ $pos -eq 0 ] && { echo "$label" > "$ticket"; continue; }   # pruned by mistake: rejoin
    [ $pos -eq 1 ] && break
    msg="build-queue: $label is #$pos of ${#tickets[@]}; running now: $(label_of "${tickets[0]}")"
    [ "$msg" != "$last" ] && { echo "$msg" >&2; last=$msg; }
    sleep 5
done
echo "build-queue: $label's turn: $*" >&2
"$@"
status=$?
echo "build-queue: $label done (exit $status)" >&2
exit $status
