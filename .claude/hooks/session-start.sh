#!/bin/bash
# SessionStart hook: make Claude Code on the web sessions able to build, test,
# run the GUI headless and take screenshots. No-op on local machines.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "$CLAUDE_PROJECT_DIR"
bash scripts/setup/bootstrap-cloud.sh

# Configure once per checkout; the build itself is left to the session
# (ccache in ~/.cache/ccache keeps rebuilds on a cached container short).
if [ ! -f build/linux-cloud/build.ninja ]; then
  cmake --preset linux-cloud > /tmp/sentinel-configure.log 2>&1 \
    || { echo "configure failed, see /tmp/sentinel-configure.log" >&2; exit 1; }
fi

echo "Sentinel cloud env ready: cmake --build --preset linux-cloud && ctest --preset linux-cloud; GUI: scripts/dev/cloud-gui.sh start|shot <name>|stop"
