#!/usr/bin/env bash
# release_macos.sh — repeatable macOS release bundle under dist/ (or a custom staging dir).
#
# Usage:
#   ./scripts/release_macos.sh [--build] [--no-zip] [STAGING_DIR]
#
# - Default STAGING_DIR: <repo>/dist/Sentinel-macos-YYYYMMDD
# - Builds with CMake preset mac-clang-release when --build is passed
# - Expects an existing Release build unless --build
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_PRESET="${SENTINEL_MAC_BUILD_PRESET:-mac-clang-release}"
BUILD_DIR="${SENTINEL_BUILD_DIR:-$REPO_ROOT/build/$BUILD_PRESET}"

DO_BUILD=false
NO_ZIP=false
POSITIONAL=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) DO_BUILD=true; shift ;;
    --no-zip) NO_ZIP=true; shift ;;
    -h|--help)
      grep '^#' "$0" | head -14 | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      POSITIONAL+=("$1"); shift
      ;;
  esac
done
STAGING_OVERRIDE="${POSITIONAL[0]:-}"

resolve_binary() {
  local name="$1" # sentinel-gui | sentinel-server
  local gui_or_server="$2" # gui | server
  local sub=sentinel-gui
  if [[ "$gui_or_server" == server ]]; then
    sub=sentinel-server
  fi
  for p in "$BUILD_DIR/apps/$sub/Release/$name" \
           "$BUILD_DIR/apps/$sub/$name"; do
    if [[ -f "$p" ]]; then
      echo "$p"
      return 0
    fi
  done
  return 1
}

if [[ "$DO_BUILD" == true ]]; then
  echo "[release] cmake --preset $BUILD_PRESET"
  if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake not found." >&2
    exit 1
  fi
  (cd "$REPO_ROOT" && cmake "--preset=$BUILD_PRESET")
  echo "[release] cmake --build --preset $BUILD_PRESET"
  (cd "$REPO_ROOT" && cmake --build "--preset=$BUILD_PRESET" -j 8)
fi

GUI_BIN="$(resolve_binary sentinel-gui gui || true)"
SERVER_BIN="$(resolve_binary sentinel-server server || true)"
if [[ -z "$GUI_BIN" ]] || [[ -z "$SERVER_BIN" ]]; then
  echo "Missing build outputs under $BUILD_DIR" >&2
  echo "  GUI: ${GUI_BIN:-MISSING}"
  echo "  server: ${SERVER_BIN:-MISSING}" >&2
  echo "Build with: cmake --preset $BUILD_PRESET && cmake --build --preset $BUILD_PRESET" >&2
  echo "Or re-run this script with --build." >&2
  exit 1
fi

if [[ -z "$STAGING_OVERRIDE" ]]; then
  mkdir -p "$REPO_ROOT/dist"
  STAGE="$REPO_ROOT/dist/Sentinel-macos-$(date +%Y%m%d)"
else
  STAGE="$STAGING_OVERRIDE"
fi

echo "[release] Staging folder: $STAGE"
mkdir -p "$STAGE"
rm -rf "$STAGE"/*

# ── Repo assets ───────────────────────────────────────────────────────────────
mkdir -p "$STAGE/resources/certs"
if [[ -f "$REPO_ROOT/resources/certs/ca-bundle.crt" ]]; then
  cp "$REPO_ROOT/resources/certs/ca-bundle.crt" "$STAGE/resources/certs/ca-bundle.crt"
elif [[ -f "/etc/ssl/cert.pem" ]]; then
  cp "/etc/ssl/cert.pem" "$STAGE/resources/certs/ca-bundle.crt"
else
  for candidate in "$(brew --prefix 2>/dev/null)/etc/openssl/cert.pem" \
                   "$(brew --prefix 2>/dev/null)/opt/openssl/etc/openssl/cert.pem"; do
    if [[ -n "$candidate" && -f "$candidate" ]]; then
      cp "$candidate" "$STAGE/resources/certs/ca-bundle.crt"
      break
    fi
  done
fi
if [[ ! -f "$STAGE/resources/certs/ca-bundle.crt" ]]; then
  echo "[release] WARN: resources/certs/ca-bundle.crt missing; copy from repo resources/ or macOS/OpenSSL certs."
fi

cp -R "$REPO_ROOT/config" "$STAGE/"
cp -R "$REPO_ROOT/certs" "$STAGE/"
if [[ -f "$REPO_ROOT/certs/gen-certs.sh" ]]; then
  chmod +x "$STAGE/certs/gen-certs.sh"
fi

# QML beside package (override with SENTINEL_QML_PATH in run.sh)
mkdir -p "$STAGE/libs/gui"
cp -R "$REPO_ROOT/libs/gui/qml" "$STAGE/libs/gui/"
if [[ -f "$REPO_ROOT/libs/gui/qmldir" ]]; then
  cp "$REPO_ROOT/libs/gui/qmldir" "$STAGE/libs/gui/"
fi

# Python scripts (venv not copied — uv sync fills .venv later)
mkdir -p "$STAGE/third_party"
if [[ -d "$REPO_ROOT/../CopeTech-Edgar" ]]; then
  echo "[release] Including third_party/CopeTech-Edgar from sibling checkout."
  rsync -a --delete-after \
    --exclude '.git/' \
    "$REPO_ROOT/../CopeTech-Edgar/" "$STAGE/third_party/CopeTech-Edgar/"
else
  echo "[release] WARN: ~/.../Sentinel/../CopeTech-Edgar not found; SEC overlay Python deps may not install until you add copetech-edgar (see docs/RELEASE_CHECKLIST.md)."
fi

rsync -a \
  --exclude '.venv/' \
  --exclude '__pycache__/' \
  --exclude '.pytest_cache/' \
  --exclude '*.py[cod]' \
  "$REPO_ROOT/scripts/" "$STAGE/scripts/"

# Point bundled pyproject at in-bundle copetech when present (release layout differs from dev).
STAGED_PP="$STAGE/scripts/pyproject.toml"
if [[ -f "$STAGED_PP" ]] && [[ -d "$STAGE/third_party/CopeTech-Edgar" ]] \
  && grep -q '../../CopeTech-Edgar' "$STAGED_PP"; then
  perl -0777 -i -pe \
    's|path\s*=\s*"../../CopeTech-Edgar"|path = "../third_party/CopeTech-Edgar"|g' \
    "$STAGED_PP"
  echo "[release] Adjusted scripts/pyproject.toml copetech-edgar path for bundle layout."
fi

if command -v uv >/dev/null 2>&1; then
  echo "[release] uv sync in scripts (may take a minute)…"
  (cd "$STAGE/scripts" && uv sync) || {
    echo "[release] WARN: uv sync failed — install deps on the staging machine before zipping:"
    echo "  (cd \"$STAGE/scripts\" && uv sync)"
  }
else
  echo "[release] WARN: uv not on PATH — Python backends need: brew install uv; then run uv sync inside scripts/."
fi

LICENSE_SRC="$REPO_ROOT/LICENSE"
if [[ -f "$LICENSE_SRC" ]]; then
  cp "$LICENSE_SRC" "$STAGE/LICENSE"
fi

if [[ -f "$REPO_ROOT/scripts/release/LAUNCH_README.md" ]]; then
  cp "$REPO_ROOT/scripts/release/LAUNCH_README.md" "$STAGE/LAUNCH_README.md"
fi
if [[ -f "$REPO_ROOT/scripts/release/README_RELEASE.md" ]]; then
  cp "$REPO_ROOT/scripts/release/README_RELEASE.md" "$STAGE/README_RELEASE.md"
fi
if [[ -f "$REPO_ROOT/README-RUN.md" ]]; then
  cp "$REPO_ROOT/README-RUN.md" "$STAGE/README.md"
elif [[ -f "$REPO_ROOT/README.md" ]]; then
  cp "$REPO_ROOT/README.md" "$STAGE/README.md"
fi

# ── Binaries (create bundles under STAGE regardless of caller cwd) ───────────────
APP_NAME_GUI="Sentinel.app"
CONTENTSG="${STAGE}/${APP_NAME_GUI}/Contents"
MACOSG="${CONTENTSG}/MacOS"
mkdir -p "$MACOSG"
cp "$GUI_BIN" "$MACOSG/sentinel-gui"
chmod +x "$MACOSG/sentinel-gui"

APP_NAME_SERVER="SentinelServer.app"
CONTENTSS="${STAGE}/${APP_NAME_SERVER}/Contents"
MACOSS="${CONTENTSS}/MacOS"
mkdir -p "$MACOSS"
cp "$SERVER_BIN" "$MACOSS/sentinel-server"
chmod +x "$MACOSS/sentinel-server"

# Minimal plist (GUI)
cat > "${CONTENTSG}/Info.plist" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>sentinel-gui</string>
  <key>CFBundleIdentifier</key>
  <string>com.sentinel.gui</string>
  <key>CFBundleName</key>
  <string>Sentinel</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
  <key>NSRequiresAquaSystemAppearance</key>
  <false/>
</dict>
</plist>
PLIST

# Server plist (Qt libs via macdeployqt)
cat > "${CONTENTSS}/Info.plist" << 'PLISTS'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>sentinel-server</string>
  <key>CFBundleIdentifier</key>
  <string>com.sentinel.server</string>
  <key>CFBundleName</key>
  <string>SentinelServer</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0</string>
  <key>LSUIElement</key>
  <true/>
</dict>
</plist>
PLISTS

# ── Qt: macdeployqt ─────────────────────────────────────────────────────────────
MACDEPLOYQT=""
for candidate in "/opt/homebrew/opt/qt/bin/macdeployqt" "/usr/local/opt/qt/bin/macdeployqt" "$(which macdeployqt 2>/dev/null)"; do
  if [[ -n "${candidate:-}" && -x "$candidate" ]]; then
    MACDEPLOYQT="$candidate"
    break
  fi
done
QML_DIR_ABS="$REPO_ROOT/libs/gui/qml"
if [[ -n "${MACDEPLOYQT:-}" ]]; then
  echo "[release] macdeployqt (GUI bundle)…"
  MACDEPLOYQT_DIR="$(dirname "$MACDEPLOYQT")"
  APP_GUI_ABS="$(cd "$STAGE" && pwd)/${APP_NAME_GUI}"
  if [[ -d "$QML_DIR_ABS" ]]; then
    (cd "$MACDEPLOYQT_DIR" && ./macdeployqt "$APP_GUI_ABS" "-qmldir=$QML_DIR_ABS" -no-strip) || \
      "$MACDEPLOYQT" "$STAGE/$APP_NAME_GUI" "-qmldir=$QML_DIR_ABS" -no-strip
  else
    (cd "$MACDEPLOYQT_DIR" && ./macdeployqt "$APP_GUI_ABS" -no-strip) || \
      "$MACDEPLOYQT" "$STAGE/$APP_NAME_GUI" -no-strip
  fi
  APP_SRV_ABS="$(cd "$STAGE" && pwd)/${APP_NAME_SERVER}"
  echo "[release] macdeployqt (server bundle — Qt Network/Core)…"
  (cd "$MACDEPLOYQT_DIR" && ./macdeployqt "$APP_SRV_ABS" -no-strip) || \
    "$MACDEPLOYQT" "$STAGE/$APP_NAME_SERVER" -no-strip

  echo "[release] codesign ad-hoc (deep)…"
  codesign --force --deep --sign - "$APP_GUI_ABS"
  codesign --force --deep --sign - "$APP_SRV_ABS"
else
  echo "[release] WARN: macdeployqt not found; bundle may require Homebrew Qt on DYLD path."
fi

# ── Launcher (cwd = bundle root; logs under logs/) ─────────────────────────────
cat > "$STAGE/run.sh" << 'RUNSH'
#!/usr/bin/env bash
set -euo pipefail
# Bundle root — all relative runtime paths resolve from here (never the source repo cwd).
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
mkdir -p "${ROOT}/logs"
SERVER_LOG="${ROOT}/logs/sentinel-server.log"

echo "[Sentinel] Bundle root (cwd): ${ROOT}"
echo "[Sentinel] Server log file: ${SERVER_LOG}"
echo "[Sentinel] GUI: when started from this Terminal, Qt messages may print here; Finder/double-click has no shell log — use Terminal or Console.app for GUI diagnostics."

# Required for shipped QML outside the .app — see MANIFEST.txt for layout.
export SENTINEL_QML_PATH="${ROOT}/libs/gui/qml/DepthChartView.qml"

if command -v lsof >/dev/null 2>&1; then
  if lsof -iTCP:8080 -sTCP:LISTEN -t >/dev/null 2>&1; then
    echo "Port 8080 already in use. Stop the other sentinel-server (or quit the old process), then try again." >&2
    exit 1
  fi
fi
SERVER_EXE="${ROOT}/SentinelServer.app/Contents/MacOS/sentinel-server"
GUI_EXE="${ROOT}/Sentinel.app/Contents/MacOS/sentinel-gui"
if [[ ! -x "$SERVER_EXE" ]]; then
  echo "Missing server binary inside SentinelServer.app; re-run scripts/release_macos.sh" >&2
  exit 1
fi
if [[ ! -x "$GUI_EXE" ]]; then
  echo "Missing sentinel-gui inside Sentinel.app" >&2
  exit 1
fi

echo "[Sentinel] Starting server (background) → logging to ${SERVER_LOG}"
: >"$SERVER_LOG"
"$SERVER_EXE" >>"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "[Sentinel] ERROR: sentinel-server exited immediately. Last 80 lines of ${SERVER_LOG}:" >&2
  tail -80 "$SERVER_LOG" >&2 || true
  exit 1
fi

echo "[Sentinel] Starting GUI (foreground). Close the window to exit the client; then this script stops the server."
"$GUI_EXE" || true
echo "[Sentinel] Stopping server (PID ${SERVER_PID})…"
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
RUNSH
chmod +x "$STAGE/run.sh"

# ── Manifest (no absolute paths to the build machine repo — relative layout only) ─
GIT_FULL="$(cd "$REPO_ROOT" && git rev-parse HEAD 2>/dev/null || echo "unknown")"
GIT_SHORT="$(cd "$REPO_ROOT" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")"
BUILD_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
{
  echo "Sentinel macOS portable bundle — MANIFEST"
  echo "=========================================="
  echo "bundle_directory_name: $(basename "$STAGE")"
  echo "packaged_at_utc: ${BUILD_UTC}"
  echo "git_commit_full: ${GIT_FULL}"
  echo "git_commit_short: ${GIT_SHORT}"
  echo ""
  echo "Included binaries (.app bundles):"
  echo "  - Sentinel.app/Contents/MacOS/sentinel-gui"
  echo "  - SentinelServer.app/Contents/MacOS/sentinel-server"
  echo ""
  echo "Configs (YAML): config/server_config.yaml, config/client_config.yaml (dotfile overrides documented in LAUNCH_README.md)"
  echo ""
  echo "TLS / CA:"
  echo "  - resources/certs/ca-bundle.crt  (outbound TLS to exchanges/REST)"
  echo "  - certs/                         (listener TLS for client↔server; generate with certs/gen-certs.sh if missing keys)"
  echo ""
  echo "Qt / QML (runtime path — set by run.sh):"
  echo "  - SENTINEL_QML_PATH → libs/gui/qml/DepthChartView.qml (folder shipped at libs/gui/qml/)"
  echo ""
  echo "Python scripts (uv-managed; optional .venv inside scripts/ after uv sync during packaging):"
  echo "  - scripts/pyproject.toml, scripts/uv.lock"
  echo "  - third_party/CopeTech-Edgar/  (if present)"
  echo ""
  echo "User-facing docs in this bundle: README_RELEASE.md (start here), LAUNCH_README.md, README.md (upstream copy)"
} >"$STAGE/MANIFEST.txt"

ZIP_PATH="$(dirname "$STAGE")/$(basename "$STAGE").zip"
if [[ "$NO_ZIP" == false ]]; then
  echo "[release] Creating zip…"
  (cd "$(dirname "$STAGE")" && zip -r -y "$(basename "$ZIP_PATH")" "$(basename "$STAGE")")
  echo "[release] Artifact: $ZIP_PATH"
fi

echo "[release] Done. Run from staged folder:"
echo "  cd \"$STAGE\" && ./run.sh"
