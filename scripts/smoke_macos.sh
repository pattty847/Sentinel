#!/usr/bin/env bash
# smoke_macos.sh — Non-interactive sanity checks for a macOS release staging folder or extracted zip.
#
# Usage:
#   ./scripts/smoke_macos.sh [STAGE_DIR]
#       Uses STAGE_DIR, or newest repo dist/Sentinel-macos-* (still inside the clone — dev convenience).
#
# Clean-room mode (simulate a user-downloaded zip; **no cwd dependency on artifact contents):
#   ./scripts/smoke_macos.sh --clean-room [/path/to/Sentinel-macos-YYYYMMDD.zip]
#       Unpacks under /tmp/Sentinel-release-test.* — default zip: newest dist/Sentinel-macos-*.zip
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

CLEAN_ROOM=false
POSITIONAL=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean-room) CLEAN_ROOM=true; shift ;;
    -h|--help)
      sed -n '2,13p' "$0"
      exit 0
      ;;
    *)
      POSITIONAL+=("$1"); shift ;;
  esac
done

pick_stage() {
  local latest=""
  local d
  for d in "$REPO_ROOT"/dist/Sentinel-macos-*; do
    [[ -d "$d" ]] || continue
    if [[ -z "$latest" || "$d" -nt "$latest" ]]; then latest="$d"; fi
  done
  if [[ -n "$latest" ]]; then
    echo "$latest"
    return 0
  fi
  return 1
}

pick_zip() {
  ls -t "$REPO_ROOT"/dist/Sentinel-macos-*.zip 2>/dev/null | head -1 || true
}


STAGE=""
if [[ "$CLEAN_ROOM" == true ]]; then
  ZIP_PATH="${POSITIONAL[0]:-}"
  if [[ -z "$ZIP_PATH" ]]; then
    ZIP_PATH="$(pick_zip)"
  fi
  [[ -f "$ZIP_PATH" ]] || fail "Clean-room zip not found. Build with scripts/release_macos.sh or pass Sentinel-macos-*.zip explicitly."
  ZIP_PATH="$(cd "$(dirname "$ZIP_PATH")" && pwd)/$(basename "$ZIP_PATH")"

  EXTRA_TMP_REMOVE="$(mktemp -d /tmp/Sentinel-release-test.XXXXXX)"

  echo "[smoke clean-room] Unzipping to ${EXTRA_TMP_REMOVE}"
  unzip -q "$ZIP_PATH" -d "$EXTRA_TMP_REMOVE"

  STAGE="$(find "$EXTRA_TMP_REMOVE" -maxdepth 1 -type d -name 'Sentinel-macos-*' ! -path "$EXTRA_TMP_REMOVE" | head -1)"
  [[ -n "$STAGE" && -d "$STAGE" ]] || fail "Zip did not expand to Sentinel-macos-*/ directory"
  STAGE="$(cd "$STAGE" && pwd)"

  echo "[smoke clean-room] Using unpacked bundle: $STAGE (not cwd of source repo)"

else
  if [[ ${#POSITIONAL[@]} -ge 1 ]]; then
    STAGE="$(cd "${POSITIONAL[0]}" && pwd)"
  elif STAGE="$(pick_stage 2>/dev/null)"; then
    :
  else
    fail "No STAGE_DIR and no dist/Sentinel-macos-* folder. Pass a path or use --clean-room with a zip."
  fi
fi

# From here on smoke runs exclusively from extracted/staging bundle context for checks.
echo "Smoke-testing: $STAGE"

BIN_GUI="${STAGE}/Sentinel.app/Contents/MacOS/sentinel-gui"
BIN_SRV="${STAGE}/SentinelServer.app/Contents/MacOS/sentinel-server"

[[ -x "$BIN_GUI" ]] || fail "Missing executable: $BIN_GUI"
[[ -x "$BIN_SRV" ]] || fail "Missing executable: $BIN_SRV"
[[ -f "${STAGE}/config/server_config.yaml" ]] || fail "Missing server_config.yaml template"
[[ -f "${STAGE}/config/client_config.yaml" ]] || fail "Missing client_config.yaml template"
[[ -f "${STAGE}/resources/certs/ca-bundle.crt" ]] || fail "Missing resources/certs/ca-bundle.crt (TLS to Coinbase/REST breaks)"
[[ -f "${STAGE}/scripts/pyproject.toml" ]] || fail "Missing scripts/pyproject.toml"
[[ -f "${STAGE}/MANIFEST.txt" ]] || echo "WARN: MANIFEST.txt missing (older bundle?)"
[[ -f "${STAGE}/README_RELEASE.md" ]] || echo "WARN: README_RELEASE.md missing (older bundle?)"
[[ -f "${STAGE}/scripts/uv.lock" ]] || echo "WARN: scripts/uv.lock missing (uv lock may change dependency pin behavior)"
[[ -f "${STAGE}/LICENSE" ]] || echo "WARN: LICENSE missing from bundle"
[[ -d "${STAGE}/scripts/.venv" ]] || echo "WARN: scripts/.venv missing — run uv in scripts/ before shipping SEC/screener/candles"

# Clean-room only: refuse obvious embeds of **this repository's** clone path into the downloadable tree.
if [[ "$CLEAN_ROOM" == true ]]; then
  REPO_ROOT_FOR_SCAN="$(cd "$SCRIPT_DIR/.." && pwd)"
  leaking=""
  while IFS= read -r lf; do
    leaking="$lf"
    break
  done < <(grep -R --binary-files=without-match --exclude-dir=.venv -l -F "${REPO_ROOT_FOR_SCAN}" "$STAGE" 2>/dev/null || true)
  if [[ -n "${leaking}" ]]; then
    echo "First offending path refs (snippet):" >&2
    grep -n -F "${REPO_ROOT_FOR_SCAN}" "${leaking}" 2>/dev/null | head -12 >&2 || true
    fail "Text file '${leaking}' references the build-machine repo path (${REPO_ROOT_FOR_SCAN}); fix packaging/templates."
  fi
  if [[ "${SENTINEL_IGNORE_BINARY_PATH_SCAN:-0}" != "1" ]]; then
    for b in "$BIN_GUI" "$BIN_SRV"; do
      if strings "$b" 2>/dev/null | grep -qF "$REPO_ROOT_FOR_SCAN"; then
        fail "Binary $(basename "$b") embeds build repo path ${REPO_ROOT_FOR_SCAN}. Set SENTINEL_IGNORE_BINARY_PATH_SCAN=1 to skip (not recommended), or rebuild without CMAKE source dir embeds."
      fi
    done
  fi
fi

# Optional: Coinbase REST (public; no credentials)
COINBASE_REST_URL="${COINBASE_REST_URL:-https://api.exchange.coinbase.com/products/BTC-USD/ticker}"
if command -v curl >/dev/null 2>&1; then
  if curl -fsS "$COINBASE_REST_URL" | head -c 128 | grep -q .; then
    echo "OK Coinbase public REST reachable (Ticker sample)"
  else
    echo "WARN Coinbase REST check produced no readable output — offline or firewall?"
  fi
else
  echo "SKIP curl not available for Coinbase REST check"
fi

LOG="${TMPDIR:-/tmp}/sentinel-smoke.$$.log"

cleanup_server() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
}

cleanup_everything() {
  cleanup_server
  if [[ -n "${EXTRA_TMP_REMOVE:-}" && -d "${EXTRA_TMP_REMOVE}" ]]; then
    rm -rf "${EXTRA_TMP_REMOVE}"
  fi
}

trap cleanup_everything EXIT INT TERM HUP

export SENTINEL_HEALTH_PORT="${SENTINEL_HEALTH_PORT:-18090}"
if lsof -iTCP:"${SENTINEL_HEALTH_PORT}" -sTCP:LISTEN -t >/dev/null 2>&1; then
  fail "Port ${SENTINEL_HEALTH_PORT} already in use — free it or set SENTINEL_HEALTH_PORT"
fi

cd "$STAGE"
nohup env SENTINEL_HEALTH_PORT="${SENTINEL_HEALTH_PORT}" "$BIN_SRV" >"$LOG" 2>&1 &
SERVER_PID=$!

echo "Started server PID=$SERVER_PID (health port ${SENTINEL_HEALTH_PORT})"

sleep 0.5
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "--- sentinel-server exited before health check; last 80 lines of log ---" >&2
  tail -80 "$LOG" >&2 || true
  fail "sentinel-server process died immediately (see log above)"
fi

ok=false
for _ in $(seq 1 30); do
  if curl -fsS "http://127.0.0.1:${SENTINEL_HEALTH_PORT}/ping" 2>/dev/null | grep -q OK; then
    ok=true
    break
  fi
  sleep 0.2
done

if [[ "$ok" != true ]]; then
  echo "--- server log tail (last 80 lines) ---"
  tail -80 "$LOG" || true
  echo "--- diagnostics (ping failed on health endpoint) ---"
  echo "SENTINEL_HEALTH_PORT=${SENTINEL_HEALTH_PORT}"
  echo "STAGE(bundle root)=${STAGE}"
  echo "listening on ${SENTINEL_HEALTH_PORT}:"
  lsof -nP -iTCP:"${SENTINEL_HEALTH_PORT}" -sTCP:LISTEN 2>/dev/null || echo "(nothing listening)"
  echo "Subset of environment (sentinel/ssl related):"
  env 2>/dev/null | grep -E '^SENTINEL_|^SSL|^CURL|^HOME=' || true
  echo "curl trace:"
  curl -v --max-time 2 "http://127.0.0.1:${SENTINEL_HEALTH_PORT}/ping" 2>&1 || true
  fail "Health check did not return OK on http://127.0.0.1:${SENTINEL_HEALTH_PORT}/ping"
fi

if ! grep -q "Sentinel Server running" "$LOG" 2>/dev/null; then
  echo "WARN: Expected log line 'Sentinel Server running' not found (server may still be OK)"
else
  echo "OK Server log contains expected startup line"
fi

echo "OK smoke_macos.sh passed (server only; GUI not launched)."
