#!/usr/bin/env bash
# cloud-gui.sh — run sentinel-server + sentinel-gui headless (Xvfb) and take screenshots
# through the GUI API. For Claude Code on the web / any Linux box without a display.
#
#   scripts/dev/cloud-gui.sh start            # Xvfb + server + gui, waits for the API port
#   scripts/dev/cloud-gui.sh shot <name> [target]   # -> screenshots/<name>.png (target: main|heatmap|lab)
#   scripts/dev/cloud-gui.sh status
#   scripts/dev/cloud-gui.sh logs             # warnings/errors from both run logs
#   scripts/dev/cloud-gui.sh stop
#
# Env: SENTINEL_BUILD_DIR (default build/linux-cloud), SENTINEL_DISPLAY (default :99),
#      SENTINEL_SCREEN (default 1920x1080x24), SENTINEL_API_PORT (default 17100).
# Rendering is Mesa llvmpipe (software OpenGL): correct pixels, not representative frame cost.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${SENTINEL_BUILD_DIR:-$ROOT/build/linux-cloud}
DISPLAY_NUM=${SENTINEL_DISPLAY:-:99}
SCREEN=${SENTINEL_SCREEN:-1920x1080x24}
API_PORT=${SENTINEL_API_PORT:-17100}
RUN=$ROOT/.cloud-run
mkdir -p "$RUN"
# Container default locale is C (ASCII): Qt then mis-decodes UTF-8 labels (e.g. watchlist icons).
export LANG=C.UTF-8 LC_ALL=C.UTF-8

log() { echo "[cloud-gui] $*" >&2; }
alive() { [[ -f "$RUN/$1.pid" ]] && kill -0 "$(cat "$RUN/$1.pid")" 2>/dev/null; }

spawn() { # <name> <cmd...>
    local name=$1; shift
    if alive "$name"; then log "$name already running (pid $(cat "$RUN/$name.pid"))"; return; fi
    nohup "$@" >"$RUN/$name.out" 2>&1 &
    echo $! >"$RUN/$name.pid"
    log "$name started (pid $!, stdout: .cloud-run/$name.out)"
}

wait_port() { # <port> <seconds> <name>
    for _ in $(seq "$2"); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
        alive "$3" || { log "$3 exited early:"; tail -20 "$RUN/$3.out" >&2; return 1; }
        sleep 1
    done
    log "timeout waiting for $3 on :$1"; return 1
}

bin_of() { # <app dir> <exe>: single-config generators put $<CONFIG> in the path when a build type is set
    local c
    for c in "" Debug/ Release/ RelWithDebInfo/; do
        [[ -x "$BUILD/apps/$1/$c$2" ]] && { echo "$BUILD/apps/$1/$c$2"; return; }
    done
}

latest_log() { # <app>
    local d=${SENTINEL_LOG_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/Sentinel/logs}
    ls -t "$d"/"$1"-*.log 2>/dev/null | head -1
}

case "${1:-}" in
start)
    GUI_BIN=$(bin_of sentinel-gui sentinel-gui); SERVER_BIN=$(bin_of sentinel-server sentinel-server)
    [[ -n "$GUI_BIN" && -n "$SERVER_BIN" ]] || { log "no build in $BUILD; run: cmake --build --preset linux-cloud"; exit 1; }
    [[ -f "$ROOT/certs/sentinel-server.crt" ]] || (cd "$ROOT" && bash certs/gen-certs.sh >/dev/null)
    # The cloud sandbox TLS-intercepts egress with its own CA; the pinned
    # resources/certs/ca-bundle.crt cannot verify it, so point MDC at the sandbox bundle.
    if [[ -n "${SSL_CERT_FILE:-}" && ! -e "$ROOT/config/.server_config.yaml" ]]; then
        printf 'mdc:\n  ssl_ca_bundle: %s\n' "$SSL_CERT_FILE" >"$ROOT/config/.server_config.yaml"
        log "wrote config/.server_config.yaml (mdc.ssl_ca_bundle=$SSL_CERT_FILE)"
    fi
    spawn xvfb Xvfb "$DISPLAY_NUM" -screen 0 "$SCREEN" -nolisten tcp
    sleep 1
    cd "$ROOT"
    spawn server "$SERVER_BIN"
    DISPLAY=$DISPLAY_NUM spawn gui "$GUI_BIN"
    wait_port "$API_PORT" 60 gui
    log "ready: GUI API on http://127.0.0.1:$API_PORT"
    ;;
shot)
    name=${2:?usage: shot <name> [target]}
    target=${3:-main}
    resp=$(curl -sS "http://127.0.0.1:$API_PORT/screenshot?name=$name&target=$target")
    echo "$resp"
    grep -q '"ok":true' <<<"$resp"
    ;;
status)
    for n in xvfb server gui; do
        if alive "$n"; then echo "$n: running (pid $(cat "$RUN/$n.pid"))"; else echo "$n: stopped"; fi
    done
    echo "gui log:    $(latest_log sentinel-gui)"
    echo "server log: $(latest_log sentinel-server)"
    ;;
logs)
    for app in sentinel-server sentinel-gui; do
        f=$(latest_log "$app"); [[ -n "$f" ]] || continue
        echo "== $f"; grep -E ' [WEF] ' "$f" | tail -${LINES_TAIL:-30} || true
    done
    ;;
stop)
    for n in gui server xvfb; do
        alive "$n" && kill "$(cat "$RUN/$n.pid")" && log "stopped $n"
        rm -f "$RUN/$n.pid"
    done
    ;;
*)
    sed -n 2,14p "$0"; exit 2 ;;
esac
