#!/bin/bash
# A9 and review r1 finding 2: deploy-runtime.sh accepts only the restarted
# service's own log (file name and header PID, header exe) and only a write
# marker: the primary recorder start, or the serving roller's readiness after
# every product's writer opened. "Roller started ... mode=live" alone is not one.
set -u
script="$1/scripts/dev/deploy-runtime.sh"
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
exe=/Users/agent/Sentinel-runtime/bin/sentinel-server
line='2026-10-06 12:00:00.000 D app     t1 ShadowRoller.cpp:800 |'
log() { # pid exe body...
    local pid=$1 path=$2
    shift 2
    {
        echo "# Sentinel log: app=sentinel-server version=3.0.0-alpha pid=$pid"
        echo "# started=2026-10-06T12:00:00.000"
        echo "# exe=$path built=2026-10-06T11:59:00"
        printf '%s\n' "$@"
    } > "$dir/sentinel-server-20261006-120000-$pid.log"
}
log 101 "$exe" "$line Recording v2 started: dir=/x near=1@+/-5%"
log 102 "$exe" "$line Roller started product=PEPE-USD mode=live day=1 root=/x" \
    "$line Roller writer open product=PEPE-USD" "$line Roller serving ready products=1 root=/x"
# Failed startup: started live, but the writer never opened (product lease held).
log 103 "$exe" "$line Roller started product=BTC-USD mode=live day=1 root=/x" \
    "$line Shadow roller retry product=BTC-USD error=product writer lock unavailable: BTC-USD"
log 104 "$exe" "$line Roller started product=BTC-USD mode=shadow day=1 root=/x"
# Unrelated process: an isolated agent server (worktree exe) that is ready.
log 105 /Volumes/T7/sentinel-worktrees/x/build/mac-clang/apps/sentinel-server/sentinel-server \
    "$line Roller serving ready products=7 root=/x"
# A log named for PID 106 whose header belongs to another process.
log 107 "$exe" "$line Roller serving ready products=7 root=/x"
mv "$dir/sentinel-server-20261006-120000-107.log" "$dir/sentinel-server-20261006-120000-106.log"
fail=0
expect() { # want pid what
    if "$script" check-server-log "$dir" "$2" "$exe"; then got=0; else got=1; fi
    [[ $got == "$1" ]] || { echo "FAIL: $3"; fail=1; }
}
expect 0 101 "primary marker rejected"
expect 0 102 "roller serving readiness rejected"
expect 1 103 "failed roller startup accepted"
expect 1 104 "shadow-only roller accepted"
expect 1 105 "unrelated process (other exe) accepted"
expect 1 106 "log header of another PID accepted"
expect 1 108 "missing log accepted"
expect 1 "1*" "PID pattern accepted"
(( fail )) && exit 1
echo "DEPLOY_MARKER: 2 accepted; failed startup, shadow-only, other exe, other PID, missing rejected"
