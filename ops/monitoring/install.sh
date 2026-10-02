#!/usr/bin/env bash
# install.sh - Sentinel monitoring on this Mac: VictoriaMetrics (scrape + 1 y
# store), Grafana (dashboards + alerts to ntfy) and node_exporter, each a launchd
# user agent. See ops/monitoring/README.md.
#
#   ops/monitoring/install.sh                 # brew install, render, load, verify
#   ops/monitoring/install.sh --no-brew       # same, formulae already installed
#   ops/monitoring/install.sh --dry-run <dir> # render with a dummy topic into <dir> and lint; nothing else
#   ops/monitoring/install.sh --uninstall     # unload and remove the three plists; data is kept
#
# ntfy topic (the only secret): SENTINEL_NTFY_TOPIC in the environment, or a
# line SENTINEL_NTFY_TOPIC=<topic> in ops/monitoring/ntfy.env (gitignored).
#
# Touches only com.sentinel.metrics, com.sentinel.grafana and
# com.sentinel.node-exporter. It never stops, restarts or edits the recorder
# (com.sentinel.recorder) or the capture (com.sentinel.capture).
set -euo pipefail

# Physical paths: a symlink must not hide a checkout or runtime dir on T7.
HERE=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(cd "$HERE/../.." && pwd -P)
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

# True when <path> (or, if it does not exist yet, its nearest existing parent)
# is on the internal disk: physically outside /Volumes and on a non-/Volumes mount.
on_internal_disk() {
    local p=$1
    while [[ ! -e "$p" ]]; do p=$(dirname "$p"); done
    local phys mount
    phys=$(cd "$p" && pwd -P)
    mount=$(df -P "$phys" | awk 'NR == 2 {print $6}')
    [[ "$phys" != /Volumes/* && -n "$mount" && "$mount" != /Volumes/* ]]
}

# The services read prometheus.yml and the Grafana provisioning from this
# checkout and keep their data in ~/Sentinel-runtime/monitoring for as long as
# they run: both must be on the internal disk, never on T7 (an agent worktree
# lives there), because the monitor must outlive a T7 failure.
if [[ $mode != dry-run ]]; then
    for path in "$REPO" "$RT"; do
        if ! on_internal_disk "$path"; then
            echo "error: $path is not on the internal disk; run install.sh from the main checkout" >&2
            echo "       and keep ~/Sentinel-runtime on the internal disk" >&2
            exit 1
        fi
    done
fi

if [[ $mode == dry-run ]]; then
    # Never the real topic: dry-run output may land in an unignored place.
    topic=dry-run-topic
else
    topic=${SENTINEL_NTFY_TOPIC:-}
    if [[ -z "$topic" && -f "$HERE/ntfy.env" ]]; then
        if ! git -C "$REPO" check-ignore -q "$HERE/ntfy.env"; then
            echo "error: $HERE/ntfy.env is not gitignored; refusing to read a secret that could be committed" >&2
            exit 1
        fi
        topic=$(sed -n 's/^SENTINEL_NTFY_TOPIC=//p' "$HERE/ntfy.env" | tail -1 | tr -d "\"' \r")
    fi
fi
if [[ ! "$topic" =~ ^[A-Za-z0-9_-]{8,64}$ ]]; then
    echo "error: set SENTINEL_NTFY_TOPIC (8-64 chars of A-Z a-z 0-9 _ -), or put" >&2
    echo "       SENTINEL_NTFY_TOPIC=<topic> in $HERE/ntfy.env (gitignored)." >&2
    echo "       Pick a random topic: it is the only secret on ntfy.sh." >&2
    exit 1
fi
# ntfy's built-in template=grafana drops the priority; an inline template keeps it (iOS hides default-priority pushes).
ntfy_url="https://ntfy.sh/$topic?template=yes&title=%7B%7B.title%7D%7D&message=%7B%7B.message%7D%7D&priority=high"

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

# Everything rendered from here on is private (brew above keeps its own umask):
# the Grafana plist carries the ntfy URL and must not exist world-readable even
# briefly before a chmod.
umask 077

# Renders <template> into a fresh mktemp file (mode 600, created by mktemp in
# the destination directory, so never a pre-existing file with old permissions),
# checks it (placeholders; plutil for a .plist) and renames it over <output>.
render() { # template output
    # sed replacement escapes '&'; a plist additionally needs XML '&amp;'.
    local url=$ntfy_url esc_url tmp
    [[ "$2" == *.plist ]] && url=${url//&/&amp;}
    esc_url=${url//&/\\&}
    tmp=$(mktemp "$(dirname "$2")/.$(basename "$2").XXXXXX")
    if ! sed -e "s|@BREW@|$BREW|g" -e "s|@RT@|$RT|g" -e "s|@REPO@|$REPO|g" -e "s|@LOGS@|$LOGS|g" \
            -e "s|@NTFY_URL@|$esc_url|g" "$1" > "$tmp"; then
        rm -f "$tmp"
        echo "error: rendering $1 failed" >&2
        exit 1
    fi
    if grep -q '@[A-Z_]*@' "$tmp"; then
        rm -f "$tmp"
        echo "error: unrendered placeholder in $2 (from $1)" >&2
        exit 1
    fi
    if [[ "$2" == *.plist ]] && ! plutil -lint "$tmp" >/dev/null; then
        plutil -lint "$tmp" >&2 || true
        rm -f "$tmp"
        echo "error: $2 is not a valid plist" >&2
        exit 1
    fi
    mv -f "$tmp" "$2"
}

if [[ $mode == dry-run ]]; then
    mkdir -p "$out"
    for label in "${LABELS[@]}"; do
        render "$HERE/launchd/$label.plist.in" "$out/$label.plist"
    done
    render "$HERE/grafana/grafana.ini.in" "$out/grafana.ini"
    echo "rendered and linted into $out (nothing installed or loaded)"
    exit 0
fi

mkdir -p "$RT/vmdata" "$RT/grafana/data" "$RT/grafana/logs" "$RT/grafana/plugins" "$LOGS" "$AGENTS"
render "$HERE/grafana/grafana.ini.in" "$RT/grafana/grafana.ini"
for label in "${LABELS[@]}"; do
    dst="$AGENTS/$label.plist"
    rm -f "$dst.new" # left by an older version of this script; may hold the topic
    render "$HERE/launchd/$label.plist.in" "$dst"
    launchctl bootout "$DOMAIN/$label" 2>/dev/null || true
    # bootout returns before launchd has removed the job; bootstrapping too early fails with EIO.
    for _ in $(seq 1 50); do launchctl print "$DOMAIN/$label" >/dev/null 2>&1 || break; sleep 0.2; done
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
