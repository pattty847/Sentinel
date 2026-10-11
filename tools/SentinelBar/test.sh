#!/usr/bin/env bash
# Run the SentinelBarCore tests. Command Line Tools ships Swift Testing but SwiftPM does not
# search its framework folder, so pass it explicitly.
set -euo pipefail
cd "$(dirname "$0")"
F=/Library/Developer/CommandLineTools/Library/Developer/Frameworks
L=/Library/Developer/CommandLineTools/Library/Developer/usr/lib
exec ../../scripts/dev/build-queue.sh --label SentinelBar-test -- swift test \
    -Xswiftc -F -Xswiftc "$F" -Xlinker -F -Xlinker "$F" \
    -Xlinker -rpath -Xlinker "$F" -Xlinker -rpath -Xlinker "$L" "$@"
