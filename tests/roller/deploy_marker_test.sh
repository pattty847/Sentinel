#!/bin/bash
# A9: deploy-runtime.sh accepts the serving roller's start line as the server
# write marker, keeps the primary marker, and rejects a shadow-only roller.
set -u
script="$1/scripts/dev/deploy-runtime.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
prefix='2026-10-06 12:00:00.000 D app     main'
echo "$prefix ServerDataModel.cpp:221 | Recording v2 started: dir=/x near=1@+/-5%" > "$tmp/primary.log"
echo "$prefix ShadowRoller.cpp:700 | Roller started product=PEPE-USD mode=live day=1 root=/x" > "$tmp/roller.log"
echo "$prefix ShadowRoller.cpp:700 | Roller started product=BTC-USD mode=shadow day=1 root=/x" > "$tmp/shadow.log"
fail=0
"$script" check-server-log "$tmp/primary.log" || { echo "FAIL: primary marker rejected"; fail=1; }
"$script" check-server-log "$tmp/roller.log" || { echo "FAIL: roller live marker rejected"; fail=1; }
if "$script" check-server-log "$tmp/shadow.log"; then echo "FAIL: shadow-only roller accepted"; fail=1; fi
(( fail )) && exit 1
echo "DEPLOY_MARKER: primary and roller live accepted, shadow rejected"
