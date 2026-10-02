#!/usr/bin/env bash
# install.sh - Sentinel monitoring on this Mac: VictoriaMetrics (scrape + 1 y
# store), Grafana (dashboards + alerts to ntfy) and node_exporter, each a launchd
# user agent. See ops/monitoring/README.md.
#
#   ops/monitoring/install.sh                 # brew install, render, load, verify
#   ops/monitoring/install.sh --no-brew       # same, formulae already installed
#   ops/monitoring/install.sh --dry-run <dir> # render plists + grafana.ini into <dir> and lint; nothing else
#   ops/monitoring/install.sh --uninstall     # unload and remove the three plists; data is kept
#
# ntfy topic (the only secret): SENTINEL_NTFY_TOPIC in the environment, or a
# line SENTINEL_NTFY_TOPIC=<topic> in ops/monitoring/ntfy.env (gitignored).
#
# Touches only com.sentinel.metrics, com.sentinel.grafana and
# com.sentinel.node-exporter. It never stops, restarts or edits the recorder
# (com.sentinel.recorder) or the capture (com.sentinel.capture).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RT="$HOME/Sentinel-runtime/monitoring"
LOGS="$HOME/Library/Logs/Sentinel"
AGENTS="$HOME/Library/LaunchAgents"
LABELS=(com.sentinel.metrics com.sentinel.node-exporter com.sentinel.grafana)
DOMAIN="gui/$(id -u)"

mode=install
out=""
case "${1:-}" in
    "") ;;
    --no-brew) mode=install-no-brew ;;
    --dry-run) mode=dry-run; out=${2:?usage: install.sh --dry-run <dir>} ;;
    --uninstall) mode=uninstall ;;
    *) echo "usage: install.sh [--no-brew | --dry-run <dir> | --uninstall]" >&2; exit 1 ;;
esac

if [[ $mode == uninstall ]]; then
    for label in "${LABELS[@]}"; do
        launchctl bootout "$DOMAIN/$label" 2>/dev/null && echo "unloaded $label" || echo "$label was not loaded"
        rm -f "$AGENTS/$label.plist"
    done
    echo "kept data in $RT"
    exit 0
fi

# The services read prometheus.yml and the Grafana provisioning from this
# checkout for as long as they run: it must be the main checkout on the internal
# disk, never an agent worktree on T7 (the monitor must outlive a T7 failure).
if [[ $mode != dry-run && "$REPO" == /Volumes/* ]]; then
    echo "error: run install.sh from the main checkout on the internal disk, not $REPO" >&2
    exit 1
fi

topic=${SENTINEL_NTFY_TOPIC:-}
if [[ -z "$topic" && -f "$HERE/ntfy.env" ]]; then
    if ! git -C "$REPO" check-ignore -q "$HERE/ntfy.env"; then
        echo "error: $HERE/ntfy.env is not gitignored; refusing to read a secret that could be committed" >&2
        exit 1
    fi
    topic=$(sed -n 's/^SENTINEL_NTFY_TOPIC=//p' "$HERE/ntfy.env" | tail -1 | tr -d "\"' \r")
fi
if [[ $mode == dry-run && -z "$topic" ]]; then
    topic=dry-run-topic
fi
if [[ ! "$topic" =~ ^[A-Za-z0-9_-]{8,64}$ ]]; then
    echo "error: set SENTINEL_NTFY_TOPIC (8-64 chars of A-Z a-z 0-9 _ -), or put" >&2
    echo "       SENTINEL_NTFY_TOPIC=<topic> in $HERE/ntfy.env (gitignored)." >&2
    echo "       Pick a random topic: it is the only secret on ntfy.sh." >&2
    exit 1
fi
ntfy_url="https://ntfy.sh/$topic?template=grafana"

if [[ $mode == install ]]; then
    brew install victoriametrics grafana node_exporter
fi
BREW=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
for bin in "$BREW/opt/victoriametrics/bin/victoria-metrics" "$BREW/opt/grafana/bin/grafana" \
           "$BREW/opt/node_exporter/bin/node_exporter"; do
    if [[ ! -x "$bin" && $mode != dry-run ]]; then
        echo "error: missing $bin (brew install victoriametrics grafana node_exporter)" >&2
        exit 1
    fi
done

render() { # template output
    local esc_url=${ntfy_url//&/\\&}
    sed -e "s|@BREW@|$BREW|g" -e "s|@RT@|$RT|g" -e "s|@REPO@|$REPO|g" -e "s|@LOGS@|$LOGS|g" \
        -e "s|@NTFY_URL@|$esc_url|g" "$1" > "$2"
    if grep -q '@[A-Z_]*@' "$2"; then
        echo "error: unrendered placeholder in $2" >&2
        exit 1
    fi
}

if [[ $mode == dry-run ]]; then
    mkdir -p "$out"
    for label in "${LABELS[@]}"; do
        render "$HERE/launchd/$label.plist.in" "$out/$label.plist"
        plutil -lint "$out/$label.plist"
    done
    render "$HERE/grafana/grafana.ini.in" "$out/grafana.ini"
    echo "rendered into $out (nothing installed or loaded)"
    exit 0
fi

mkdir -p "$RT/vmdata" "$RT/grafana/data" "$RT/grafana/logs" "$RT/grafana/plugins" "$LOGS" "$AGENTS"
render "$HERE/grafana/grafana.ini.in" "$RT/grafana/grafana.ini"
for label in "${LABELS[@]}"; do
    dst="$AGENTS/$label.plist"
    render "$HERE/launchd/$label.plist.in" "$dst.new"
    plutil -lint "$dst.new" >/dev/null
    chmod 600 "$dst.new" # the Grafana plist carries the ntfy URL
    mv "$dst.new" "$dst"
    launchctl bootout "$DOMAIN/$label" 2>/dev/null || true
    launchctl bootstrap "$DOMAIN" "$dst"
    echo "loaded $label"
done

wait_for() { # name url pattern
    for _ in $(seq 1 30); do
        if curl -fsS --max-time 2 "$2" 2>/dev/null | grep -q "$3"; then
            echo "ok: $1"
            return 0
        fi
        sleep 2
    done
    echo "FAILED: $1 did not answer at $2 within 60 s (see $LOGS/monitoring-*.err)" >&2
    return 1
}
status=0
wait_for victoriametrics http://127.0.0.1:8428/health OK || status=1
wait_for node_exporter http://127.0.0.1:9100/metrics node_filesystem_avail_bytes || status=1
wait_for grafana http://127.0.0.1:3000/api/health database || status=1
# up == 1 needs the sentinel-server build with /metrics (deploy-runtime.sh server);
# an older binary answers 404 and stays at up == 0.
wait_for "sentinel-server scrape (up == 1)" \
    'http://127.0.0.1:8428/api/v1/query?query=up%7Bjob%3D%22sentinel-server%22%7D' '"value":\[[^]]*,"1"\]' || status=1
echo "up per job:"
curl -fsS 'http://127.0.0.1:8428/api/v1/query?query=up' | python3 -c '
import json, sys
for r in json.load(sys.stdin)["data"]["result"]:
    print("  %s = %s" % (r["metric"].get("job"), r["value"][1]))' || true
echo "Grafana: http://127.0.0.1:3000 (home = Sentinel health). VictoriaMetrics UI: http://127.0.0.1:8428/vmui"
exit $status
