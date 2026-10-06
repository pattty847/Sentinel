#!/bin/bash
# A9 and review r1/r2 finding 2-3: deploy-runtime.sh accepts only the
# restarted service's own log (file name and header PID, header exe, read once
# from a regular non-symlink file), only the current write marker (primary
# start, or the serving roller's latest readiness transition is "ready"), and
# only if launchd still reports that PID after the read.
set -u
repo=$1
script="$repo/scripts/dev/deploy-runtime.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
line='2026-10-06 12:00:00.000 D app     t1 ShadowRoller.cpp:800 |'
write_log() { # dir pid exe body...
    local dir=$1 pid=$2 path=$3
    shift 3
    mkdir -p "$dir"
    {
        echo "# Sentinel log: app=sentinel-server version=3.0.0-alpha pid=$pid"
        echo "# started=2026-10-06T12:00:00.000"
        echo "# exe=$path built=2026-10-06T11:59:00"
        printf '%s\n' "$@"
    } > "$dir/sentinel-server-20261006-120000-$pid.log"
}
ready="$line Roller serving ready products=2 root=/x"

# Part 1: check-server-log against static logs.
dir=$tmp/static
exe=/Users/agent/Sentinel-runtime/bin/sentinel-server
write_log "$dir" 101 "$exe" "$line Recording v2 started: dir=/x near=1@+/-5%"
write_log "$dir" 102 "$exe" "$line Roller started product=PEPE-USD mode=live day=1 root=/x" \
    "$line Roller writer open product=PEPE-USD" "$line Roller writer healthy product=PEPE-USD" "$ready"
# Failed startup: live and writers open, never healthy.
write_log "$dir" 103 "$exe" "$line Roller started product=BTC-USD mode=live day=1 root=/x" \
    "$line Roller writer open product=BTC-USD" \
    "$line Shadow roller retry product=BTC-USD error=checkpoint temporary write failed"
write_log "$dir" 104 "$exe" "$line Roller started product=BTC-USD mode=shadow day=1 root=/x"
write_log "$dir" 105 /Volumes/T7/sentinel-worktrees/x/build/mac-clang/apps/sentinel-server/sentinel-server "$ready"
write_log "$dir" 107 "$exe" "$ready"
mv "$dir/sentinel-server-20261006-120000-107.log" "$dir/sentinel-server-20261006-120000-106.log"
# Ready, then a product lost its writer: not ready now.
write_log "$dir" 108 "$exe" "$ready" "$line Roller serving not ready product=BTC-USD reason=writer closed"
# Ready again after a loss.
write_log "$dir" 109 "$exe" "$ready" "$line Roller serving not ready product=BTC-USD reason=x" "$ready"
# A symlinked log path (to a ready log with the right header) and a directory.
write_log "$tmp/elsewhere" 110 "$exe" "$ready"
ln -s "$tmp/elsewhere/sentinel-server-20261006-120000-110.log" "$dir/sentinel-server-20261006-120000-110.log"
mkdir "$dir/sentinel-server-20261006-120000-111.log"
check() { # want pid what
    if "$script" check-server-log "$dir" "$2" "$exe"; then got=0; else got=1; fi
    [[ $got == "$1" ]] || { echo "FAIL: $3"; fail=1; }
}
check 0 101 "primary marker rejected"
check 0 102 "roller serving readiness rejected"
check 1 103 "failed roller startup accepted"
check 1 104 "shadow-only roller accepted"
check 1 105 "unrelated process (other exe) accepted"
check 1 106 "log header of another PID accepted"
check 1 108 "readiness lost after ready accepted"
check 0 109 "readiness regained rejected"
check 1 110 "symlinked log accepted"
check 1 111 "directory log accepted"
check 1 112 "missing log accepted"
check 1 "1*" "PID pattern accepted"

# Part 2: verify_writes with launchctl and sleep stubbed on PATH (test only).
stubs=$tmp/stubs
mkdir -p "$stubs"
cat > "$stubs/launchctl" <<'STUB'
#!/bin/bash
[[ $1 == print ]] || exit 0
first=$(head -n 1 "$STUB_PIDS")
if [[ $(wc -l < "$STUB_PIDS") -gt 1 ]]; then tail -n +2 "$STUB_PIDS" > "$STUB_PIDS.t" && mv "$STUB_PIDS.t" "$STUB_PIDS"; fi
printf '\tpid = %s\n' "$first"
STUB
printf '#!/bin/bash\nexit 0\n' > "$stubs/sleep"
chmod +x "$stubs/launchctl" "$stubs/sleep"
verify() { # want pids... -- logs: <pid>:<ready|none|symlink> ... ; what is last arg
    local want=$1 what=$2 pids=$3 logs=$4 home=$tmp/home-$RANDOM entry pid kind got
    mkdir -p "$home/Library/Logs/Sentinel" "$home/Sentinel-runtime/bin"
    printf '%s\n' $pids > "$home/pids"
    for entry in $logs; do
        pid=${entry%%:*} kind=${entry#*:}
        case $kind in
            ready) write_log "$home/Library/Logs/Sentinel" "$pid" "$home/Sentinel-runtime/bin/sentinel-server" "$ready" ;;
            symlink) write_log "$home/other" "$pid" "$home/Sentinel-runtime/bin/sentinel-server" "$ready"
                     ln -s "$home/other/sentinel-server-20261006-120000-$pid.log" \
                         "$home/Library/Logs/Sentinel/sentinel-server-20261006-120000-$pid.log" ;;
        esac
    done
    if HOME=$home STUB_PIDS=$home/pids PATH="$stubs:$PATH" bash -c \
        'source "$1" && set +e && verify_writes server com.sentinel.recorder sentinel-server' _ "$script" > "$home/out" 2>&1
    then got=0; else got=1; fi
    [[ $got == "$want" ]] || { echo "FAIL: $what"; cat "$home/out"; fail=1; }
}
verify 0 "restarted ready process rejected" "300 301" "301:ready"
verify 1 "PID changed after the read accepted" "300 301 302" "301:ready"
verify 0 "stable replacement PID after a change rejected" "300 301 302" "301:ready 302:ready"
verify 1 "replaced (old) PID accepted" "300" "300:ready"
verify 1 "symlinked log of the restarted PID accepted" "300 301" "301:symlink"
(( fail )) && exit 1
echo "DEPLOY_MARKER: 3 static logs accepted, 9 rejected; verify_writes 2 accepted, 3 rejected"
