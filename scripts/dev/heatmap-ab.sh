#!/usr/bin/env bash
# heatmap-ab.sh — S6d A/B evidence: two sentinel-gui processes (legacy and gpu heatmap
# renderer) on the running local recorder, driven through the same Agent API sequence,
# with target=heatmap screenshots and state snapshots at every step.
#
#   scripts/dev/heatmap-ab.sh [run-name]          # A/B sequence -> screenshots/s6d/<run-name>/
#   scripts/dev/heatmap-ab.sh --soak 10 [run-name] # gpu only, follow-live 1m, N minutes of samples
#
# Env: SENTINEL_BUILD_DIR (default build/mac-clang), AB_LEGACY_PORT (17121), AB_GPU_PORT (17122),
#      AB_SETTLE_TIMEOUT_S (30), AB_LEGACY_SETTLE_S (4), SENTINEL_PROBES (passed to the GUIs).
#
# Rules it keeps (AGENTS.md): it is a client of the always-on recorder (never starts a server);
# it never touches the owner's settings: renderer and API port are process-only CLI overrides,
# every settings POST carries persist:false, and the GUIs are ended with SIGTERM (no closeEvent,
# so no _last_session layout write). The owner's QSettings plist is exported before and after and
# the diff is reported. Screenshots use target=heatmap only (FM-120).
#
# Caveat: sentinel-gui spawns the screener server on port 17200 and kills whatever holds that
# port first, so this script refuses to run while the owner's GUI (17100 or 17200) is up.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${SENTINEL_BUILD_DIR:-$ROOT/build/mac-clang}
GUI_BIN=$BUILD/apps/sentinel-gui/sentinel-gui
LEGACY_PORT=${AB_LEGACY_PORT:-17121}
GPU_PORT=${AB_GPU_PORT:-17122}
SETTLE_TIMEOUT=${AB_SETTLE_TIMEOUT_S:-30}
LEGACY_SETTLE=${AB_LEGACY_SETTLE_S:-4}
SERVER_PORT=${AB_SERVER_PORT:-8080}
PLIST_DOMAIN=com.sentinel.SentinelTerminal

SOAK_MIN=0
if [[ "${1:-}" == "--soak" ]]; then SOAK_MIN=${2:?usage: --soak <minutes> [run-name]}; shift 2; fi
RUN=${1:-$(date +%Y%m%d-%H%M%S)}
OUT=$ROOT/screenshots/s6d/$RUN
mkdir -p "$OUT"

log() { printf '[heatmap-ab %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
now_ms() { perl -MTime::HiRes=time -e 'printf("%d\n", time()*1000)'; }
listening() { lsof -nP -iTCP:"$1" -sTCP:LISTEN -t 2>/dev/null; }

declare -A PID PORT
cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    for mode in "${!PID[@]}"; do
        if kill -0 "${PID[$mode]}" 2>/dev/null; then
            # SIGTERM: Qt installs no handler, the process dies before closeEvent, so the
            # owner's _last_session layout is never written by an A/B process.
            kill -TERM "${PID[$mode]}" 2>/dev/null || true
        fi
    done
    sleep 1
    for mode in "${!PID[@]}"; do
        kill -0 "${PID[$mode]}" 2>/dev/null && kill -KILL "${PID[$mode]}" 2>/dev/null || true
        local f
        f=$(ls -t ~/Library/Logs/Sentinel/sentinel-gui-*-"${PID[$mode]}".log 2>/dev/null | head -1 || true)
        [[ -n "$f" ]] && cp "$f" "$OUT/$mode-run.log"
    done
    if [[ -f "$OUT/settings-before.plist" ]]; then
        defaults export "$PLIST_DOMAIN" - >"$OUT/settings-after.plist" 2>/dev/null || true
        if diff -u "$OUT/settings-before.plist" "$OUT/settings-after.plist" >"$OUT/settings-diff.txt"; then
            log "owner settings ($PLIST_DOMAIN): UNCHANGED"
        else
            log "owner settings ($PLIST_DOMAIN): CHANGED, see $OUT/settings-diff.txt"
        fi
    fi
    log "done (exit $rc): $OUT"
    exit "$rc"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- preconditions
[[ -x "$GUI_BIN" ]] || { log "no sentinel-gui in $BUILD (cmake --build --preset mac-clang)"; exit 1; }
[[ -n "$(listening "$SERVER_PORT")" ]] || { log "no server listening on :$SERVER_PORT (the recorder must be up; never start one here)"; exit 1; }
for p in 17100 17200; do
    [[ -z "$(listening "$p")" ]] || { log "port $p is in use: the owner's GUI seems to be running; refusing (the GUI kills port 17200 holders)"; exit 1; }
done
for p in "$LEGACY_PORT" "$GPU_PORT"; do
    [[ -z "$(listening "$p")" ]] || { log "port $p is in use; set AB_LEGACY_PORT/AB_GPU_PORT"; exit 1; }
done
[[ -f "$ROOT/certs/sentinel-server.crt" ]] || { log "missing certs/sentinel-server.crt (copy it from the main checkout)"; exit 1; }
defaults export "$PLIST_DOMAIN" - >"$OUT/settings-before.plist"
cd "$ROOT"

# ---------------------------------------------------------------- API helpers
api() { # <mode> <path>
    curl -s --max-time 10 "http://127.0.0.1:${PORT[$1]}$2"
}
post() { # <mode> <path> <json>
    curl -s --max-time 10 -H 'Content-Type: application/json' -d "$3" "http://127.0.0.1:${PORT[$1]}$2"
}
wait_op() { # <mode> <opId> -> prints status
    local st
    for _ in $(seq 20); do
        st=$(api "$1" "/api/v1/operations/$2?waitMs=5000" | jq -r '.data.status // "unknown"')
        [[ "$st" == "applied" ]] || break
    done
    echo "$st"
}
shot() { # <mode> <name> [opId]: target=heatmap only (FM-120); 1 shot/s per process
    local mode=$1 name=$2 op=${3:-}
    # shot runs inside $(...), so the per-process last-shot time lives in a file, not a variable
    local last=0; [[ -f "$OUT/.lastshot-$mode" ]] && last=$(cat "$OUT/.lastshot-$mode")
    local since=$(( $(now_ms) - last ))
    (( since >= 1100 )) || sleep "$(awk "BEGIN{print (1100-$since)/1000}")"
    local q="name=ab-$mode-$name&target=heatmap"
    [[ -n "$op" ]] && q="$q&afterOperation=$op&waitMs=5000"
    local r
    r=$(api "$mode" "/api/v1/screenshot?$q")
    now_ms >"$OUT/.lastshot-$mode"
    if [[ "$(jq -r .ok <<<"$r")" == "true" ]]; then
        mv "$ROOT/screenshots/ab-$mode-$name.png" "$OUT/$mode-$name.png"
        jq -r '"frame=\(.frameId) vv=\(.viewportVersion)"' <<<"$r"
    else
        log "screenshot $mode/$name failed: $r"; echo "failed"
    fi
}
snapshot() { # <mode> <name>: state + heatmap/state + viewport into one JSON
    jq -n --argjson s "$(api "$1" /api/v1/state)" --argjson h "$(api "$1" /api/v1/heatmap/state)" \
          --argjson v "$(api "$1" /api/v1/viewport)" '{state:$s.data, heatmap:$h.data, viewport:$v.data}' \
        >"$OUT/$1-$2.json"
}
gpu_settled() { # <mode> [tfMs]: 0 when heatmap/state says settled (and drawn tf matches)
    local h
    h=$(api "$1" /api/v1/heatmap/state)
    [[ "$(jq -r '.data.settled' <<<"$h")" == "true" ]] || return 1
    [[ -z "${2:-}" || "$(jq -r '.data.drawnTimeframeMs' <<<"$h")" == "$2" ]]
}
wait_settled() { # <mode> [tfMs] -> prints ms waited
    local t0 waited mode=$1 tf=${2:-}
    t0=$(now_ms)
    if [[ "$mode" == gpu ]]; then
        while ! gpu_settled "$mode" "$tf"; do
            waited=$(( $(now_ms) - t0 ))
            (( waited < SETTLE_TIMEOUT * 1000 )) || { echo "timeout:$waited"; return; }
            sleep 0.1
        done
    else
        sleep "$LEGACY_SETTLE" # legacy has no settled flag; it fetches a band then draws
    fi
    echo $(( $(now_ms) - t0 ))
}
row() { # <mode> <step> <opMs> <settleMs> <frameShot> <settledShot>
    local mode=$1 step=$2
    jq -r --arg m "$mode" --arg s "$step" --arg op "$3" --arg st "$4" --arg f1 "$5" --arg f2 "$6" \
        '[$m,$s,$op,$st,$f1,$f2,
          (.state.render.frameP95Ms|tostring),(.state.render.rateHz|tostring),
          (.heatmap.settled|tostring),(.heatmap.drawnTimeframeMs|tostring),(.heatmap.drawnTickUnits|tostring),
          (.heatmap.tickMode|tostring),(.heatmap.indicatorText|tostring),
          (.heatmap.liveAgeMs|tostring),(.heatmap.liveAgeP95Ms|tostring),(.heatmap.gpuBytes|tostring),
          (.viewport.viewportVersion|tostring),(.viewport.startMs|tostring),(.viewport.endMs|tostring),
          (.viewport.priceMin|tostring),(.viewport.priceMax|tostring)] | @tsv' "$OUT/$mode-$step.json" >>"$OUT/timings.tsv"
}

launch() { # <mode> <port>
    local mode=$1 port=$2
    PORT[$mode]=$port
    SPAWN_MS[$mode]=$(now_ms)
    nohup "$GUI_BIN" --heatmap-renderer "$mode" --api-port "$port" >"$OUT/$mode.out" 2>&1 &
    PID[$mode]=$!
    log "$mode started pid ${PID[$mode]} api :$port"
}
declare -A SPAWN_MS
wait_ready() { # <mode>: API up, connected, first heatmap data (legacy) / settled (gpu)
    local mode=$1 t
    for _ in $(seq 300); do
        [[ -n "$(listening "${PORT[$mode]}")" ]] && break
        kill -0 "${PID[$mode]}" || { log "$mode exited early"; tail -20 "$OUT/$mode.out" >&2; exit 1; }
        sleep 0.1
    done
    for _ in $(seq 600); do
        if [[ "$mode" == gpu ]]; then
            gpu_settled gpu && break
        else
            [[ "$(api legacy /api/v1/state | jq -r '.data.lastReceivedAtMs.heatmap // empty')" != "" ]] && break
        fi
        sleep 0.1
    done
    t=$(( $(now_ms) - SPAWN_MS[$mode] ))
    log "$mode ready: cold start (spawn -> first heatmap data) ${t} ms"
    echo "$mode	cold_start_ms	$t" >>"$OUT/summary.tsv"
}

# ---------------------------------------------------------------- soak (gpu only)
if (( SOAK_MIN > 0 )); then
    launch gpu "$GPU_PORT"
    wait_ready gpu
    post gpu /api/v1/timeframe '{"heatmapTimeframeMs":60000,"candleTimeframeMs":60000}' >/dev/null
    post gpu /api/v1/viewport '{"followLive":true}' >/dev/null
    sleep 2
    printf 't_s\tliveAgeMs\tliveAgeP50\tliveAgeP95\tpublishP95\tliveSamples\tframeP50\tframeP95\trateHz\tgpuCpu\trecorderCpu\tcaptureCpu\tloadavg\n' >"$OUT/soak.tsv"
    end=$(( $(date +%s) + SOAK_MIN * 60 )); t0=$(date +%s)
    rec_pid=$(pgrep -f 'bin/sentinel-server' | head -1 || true); cap_pid=$(pgrep -f 'bin/sentinel-capture' | head -1 || true)
    while (( $(date +%s) < end )); do
        h=$(api gpu /api/v1/heatmap/state); s=$(api gpu /api/v1/state)
        cpu() { [[ -n "$1" ]] && ps -o %cpu= -p "$1" | tr -d ' ' || echo na; }
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(( $(date +%s) - t0 ))" \
            "$(jq -r '[.data.liveAgeMs,.data.liveAgeP50Ms,.data.liveAgeP95Ms,.data.livePublishP95Ms,.data.liveSamples]|@tsv' <<<"$h")" \
            "$(jq -r '[.data.render.frameP50Ms,.data.render.frameP95Ms,.data.render.rateHz]|@tsv' <<<"$s")" \
            "$(cpu "${PID[gpu]}")" "$(cpu "$rec_pid")" "$(cpu "$cap_pid")" "$(sysctl -n vm.loadavg)" >>"$OUT/soak.tsv"
        sleep 5
    done
    shot gpu soak-end >/dev/null
    snapshot gpu soak-end
    python3 - "$OUT/soak.tsv" <<'PY'
import sys, statistics
rows=[l.rstrip('\n').split('\t') for l in open(sys.argv[1])][1:]
def col(i):
    out=[]
    for r in rows:
        try: out.append(float(r[i]))
        except: pass
    return out
def pct(v,p):
    v=sorted(v); return v[min(len(v)-1,int(round(p*(len(v)-1))))] if v else float('nan')
age=col(1); fp95=col(7); gcpu=col(9); rcpu=col(10); ccpu=col(11)
print(f"soak samples={len(rows)} liveAgeMs(sampled) p50={pct(age,.5):.0f} p95={pct(age,.95):.0f} max={max(age):.0f}")
print(f"node cumulative at end: p50={rows[-1][2]} p95={rows[-1][3]} publishP95={rows[-1][4]} samples={rows[-1][5]}")
print(f"frameP95Ms sampled p50={pct(fp95,.5):.2f} p95={pct(fp95,.95):.2f} max={max(fp95):.2f}")
print(f"cpu% mean gui={statistics.mean(gcpu):.1f} recorder={statistics.mean(rcpu):.1f} capture={statistics.mean(ccpu):.1f}")
PY
    exit 0
fi

# ---------------------------------------------------------------- A/B sequence
printf 'mode\tstep\top_to_rendered_ms\tsettle_ms\tframe_shot\tsettled_shot\tframeP95\trateHz\tsettled\tdrawnTf\tdrawnTick\ttickMode\tindicator\tliveAgeMs\tliveAgeP95\tgpuBytes\tvv\tstartMs\tendMs\tpriceMin\tpriceMax\n' >"$OUT/timings.tsv"
: >"$OUT/summary.tsv"
launch legacy "$LEGACY_PORT"
launch gpu "$GPU_PORT"
wait_ready legacy
wait_ready gpu
MODES=(legacy gpu)

# One step on both processes: the op, the frame right after it renders, then the settled frame.
step() { # <id> <path> <json> [expectTfMs]  (json "-" = no op, screenshot only)
    local id=$1 path=$2 json=$3 tf=${4:-}
    for mode in "${MODES[@]}"; do
        local op="" opms="-" f1="-" f2 st
        if [[ "$json" != "-" && ( "$path" != heatmap-only || "$mode" == gpu ) ]]; then
            local p=$path; [[ "$p" == heatmap-only ]] && p=/api/v1/heatmap/settings
            local t0 r; t0=$(now_ms)
            r=$(post "$mode" "$p" "$json")
            op=$(jq -r '.data.operationId // empty' <<<"$r")
            [[ -n "$op" ]] || { log "$mode $id: $r"; continue; }
            local status; status=$(wait_op "$mode" "$op")
            opms=$(( $(now_ms) - t0 ))
            f1=$(shot "$mode" "$id-frame" "$op")
            log "$mode $id: $status in ${opms} ms, frame shot $f1"
        fi
        st=$(wait_settled "$mode" "$tf")
        f2=$(shot "$mode" "$id-settled")
        snapshot "$mode" "$id"
        row "$mode" "$id" "$opms" "$st" "$f1" "$f2"
        log "$mode $id: settled in ${st} ms, settled shot $f2"
    done
}

step 01-symbol /api/v1/symbol '{"symbol":"BTC-USD"}'
step 02-tf1m /api/v1/timeframe '{"heatmapTimeframeMs":60000,"candleTimeframeMs":60000}' 60000
step 03-tf5m /api/v1/timeframe '{"heatmapTimeframeMs":300000,"candleTimeframeMs":300000}' 300000
step 04-tf1h /api/v1/timeframe '{"heatmapTimeframeMs":3600000,"candleTimeframeMs":3600000}' 3600000

# The same explicit window for both: the last 36 hours of whole hours, +-1.5% around the book mid.
NOW_MS=$(now_ms); HOUR=3600000
END=$(( (NOW_MS / HOUR + 1) * HOUR )); START=$(( END - 36 * HOUR ))
MID=$(api gpu '/api/v1/book?levels=1' | jq -r '((.data.bestBid + .data.bestAsk) / 2) | floor')
PMIN=$(( MID * 985 / 1000 / 100 * 100 )); PMAX=$(( MID * 1015 / 1000 / 100 * 100 ))
echo "viewport_1h	start=$START end=$END priceMin=$PMIN priceMax=$PMAX mid=$MID" >>"$OUT/summary.tsv"
step 05-viewport /api/v1/viewport "{\"startMs\":$START,\"endMs\":$END,\"priceMin\":$PMIN,\"priceMax\":$PMAX}" 3600000

# Wheel x5 at the chart centre (one notch each, zoom in); per-wheel render time is logged.
W=$(api gpu /api/v1/viewport | jq -r '.data.widthPx'); H=$(api gpu /api/v1/viewport | jq -r '.data.heightPx')
CX=$(( W / 2 )); CY=$(( H / 2 ))
for mode in "${MODES[@]}"; do
    for i in 1 2 3 4 5; do
        t0=$(now_ms)
        op=$(post "$mode" /api/v1/input "{\"kind\":\"wheel\",\"target\":\"chart\",\"x\":$CX,\"y\":$CY,\"deltaY\":120}" | jq -r '.data.operationId // empty')
        st=$(wait_op "$mode" "$op")
        echo "$mode	wheel$i	${op}	$st	$(( $(now_ms) - t0 ))ms" >>"$OUT/summary.tsv"
        if [[ $i == 5 ]]; then f1=$(shot "$mode" "06-wheelx5-frame" "$op"); log "$mode wheel5 frame shot $f1"; fi
    done
done
step 06-wheelx5 - - 3600000

# Price-axis drag: press at mid height, move down 120 px (the QML zooms price at -deltaY*24), release.
for mode in "${MODES[@]}"; do
    post "$mode" /api/v1/input "{\"kind\":\"dragStart\",\"target\":\"priceAxis\",\"x\":20,\"y\":$CY}" >/dev/null
    post "$mode" /api/v1/input "{\"kind\":\"dragMove\",\"target\":\"priceAxis\",\"x\":20,\"y\":$(( CY + 60 ))}" >/dev/null
    post "$mode" /api/v1/input "{\"kind\":\"dragMove\",\"target\":\"priceAxis\",\"x\":20,\"y\":$(( CY + 120 ))}" >/dev/null
done
step 07-axisdrag /api/v1/input "{\"kind\":\"dragEnd\",\"target\":\"priceAxis\",\"x\":20,\"y\":$(( CY + 120 ))}" 3600000

# Manual $1 on the gpu process only (legacy has no Manual tick; its shot shows what it draws).
step 08-manual1 heatmap-only '{"tickMode":"manual","manualTick":100,"persist":false}' 3600000
# Still Manual $1 on gpu: the last 72 hours at 1h reach back before the recording started
# (time before the oldest data draws nothing) and over any deep-only ($10 grid) history
# (veil + resolution indicator, spec rule 2); legacy shows its own band over the same window.
step 08b-manual1-72h /api/v1/viewport "{\"startMs\":$(( END - 72 * HOUR )),\"endMs\":$END,\"priceMin\":$PMIN,\"priceMax\":$PMAX}" 3600000
step 09-followlive /api/v1/viewport '{"followLive":true}' 3600000
# Back to 1m (the owner's working timeframe): follow-live, then one shared explicit window.
step 10-tf1m-live /api/v1/timeframe '{"heatmapTimeframeMs":60000,"candleTimeframeMs":60000}' 60000
NOW_MS=$(now_ms); MIN=60000
END=$(( (NOW_MS / MIN + 1) * MIN )); START=$(( END - 180 * MIN ))
MID=$(api gpu '/api/v1/book?levels=1' | jq -r '((.data.bestBid + .data.bestAsk) / 2) | floor')
PMIN=$(( MID - 60 )); PMAX=$(( MID + 60 ))
echo "viewport_1m	start=$START end=$END priceMin=$PMIN priceMax=$PMAX mid=$MID" >>"$OUT/summary.tsv"
step 11-viewport1m /api/v1/viewport "{\"startMs\":$START,\"endMs\":$END,\"priceMin\":$PMIN,\"priceMax\":$PMAX}" 60000
step 12-followlive1m /api/v1/viewport '{"followLive":true}' 60000

# 60 s of follow-live at 1m on both: live data age samples (gpu) and frame p95 (both).
printf 'mode\tt_s\tliveAgeMs\tliveAgeP50\tliveAgeP95\tframeP95\trateHz\n' >"$OUT/live.tsv"
for i in $(seq 12); do
    for mode in "${MODES[@]}"; do
        h=$(api "$mode" /api/v1/heatmap/state); s=$(api "$mode" /api/v1/state)
        printf '%s\t%s\t%s\t%s\n' "$mode" "$(( i * 5 ))" \
            "$(jq -r '[.data.liveAgeMs,.data.liveAgeP50Ms,.data.liveAgeP95Ms]|@tsv' <<<"$h")" \
            "$(jq -r '[.data.render.frameP95Ms,.data.render.rateHz]|@tsv' <<<"$s")" >>"$OUT/live.tsv"
    done
    sleep 5
done
step 13-live60s - - 60000

# Pixel difference of every settled pair (ImageMagick, 3% fuzz), for the index.
if command -v magick >/dev/null; then
    : >"$OUT/pairs.tsv"
    for f in "$OUT"/legacy-*-settled.png; do
        n=$(basename "$f"); n=${n#legacy-}; g="$OUT/gpu-$n"
        [[ -f "$g" ]] || continue
        d=$(magick compare -metric AE -fuzz 3% "$f" "$g" null: 2>&1 || true) # "<count> (<fraction>)"
        frac=$(sed -E 's/.*\(([^)]*)\).*/\1/' <<<"$d")
        printf '%s\t%s\t%s\n' "${n%-settled.png}" "${d%% *}" "$(awk "BEGIN{printf \"%.2f%%\", 100*$frac}")" >>"$OUT/pairs.tsv"
    done
fi
log "A/B sequence complete"
