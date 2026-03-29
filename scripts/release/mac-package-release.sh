#!/usr/bin/env bash
# mac-package-release.sh — Package Mac release into a folder (e.g. ~/Desktop/Sentinel).
# Run from repo root after: cmake --preset mac-clang-release && cmake --build --preset mac-clang-release
#
# Usage: scripts/release/mac-package-release.sh [TARGET_DIR]
# Default TARGET_DIR: ~/Desktop/Sentinel

set -euo pipefail
SENTINEL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${SENTINEL_ROOT}/build/mac-clang-release"
TARGET_DIR="${1:-$HOME/Desktop/Sentinel}"

GUI_BIN="${BUILD_DIR}/apps/sentinel-gui/Release/sentinel-gui"
SERVER_BIN="${BUILD_DIR}/apps/sentinel-server/Release/sentinel-server"

if [[ ! -f "$GUI_BIN" ]]; then
  echo "Missing $GUI_BIN — run release build first:"
  echo "  cmake --preset mac-clang-release && cmake --build --preset mac-clang-release"
  exit 1
fi
if [[ ! -f "$SERVER_BIN" ]]; then
  echo "Missing $SERVER_BIN — run release build first."
  exit 1
fi

echo "Packaging Mac release into: $TARGET_DIR"
mkdir -p "$TARGET_DIR"
cd "$TARGET_DIR"

# Config and certs
cp -R "${SENTINEL_ROOT}/config" .
cp -R "${SENTINEL_ROOT}/certs" .
cp "${SENTINEL_ROOT}/certs/gen-certs.sh" certs/
chmod +x certs/gen-certs.sh

# CA bundle for outgoing TLS (Coinbase REST/WS). Server and MarketDataCore look for resources/certs/ca-bundle.crt
mkdir -p resources/certs
CA_BUNDLE=""
for candidate in "/etc/ssl/cert.pem" "$(brew --prefix 2>/dev/null)/etc/openssl/cert.pem" "$(brew --prefix 2>/dev/null)/opt/openssl/etc/openssl/cert.pem"; do
  if [[ -n "$candidate" && -f "$candidate" ]]; then
    CA_BUNDLE="$candidate"
    break
  fi
done
if [[ -n "$CA_BUNDLE" ]]; then
  cp "$CA_BUNDLE" resources/certs/ca-bundle.crt
else
  echo "Warning: No system CA bundle found; create resources/certs/ca-bundle.crt (e.g. copy from /etc/ssl/cert.pem) or REST may fail."
fi

# QML: copy into package so app can load from CWD when SENTINEL_QML_PATH is set (run.sh sets it)
mkdir -p libs/gui
cp -R "${SENTINEL_ROOT}/libs/gui/qml" libs/gui/
if [[ -f "${SENTINEL_ROOT}/libs/gui/qmldir" ]]; then
  cp "${SENTINEL_ROOT}/libs/gui/qmldir" libs/gui/
fi

# Server binary
cp "$SERVER_BIN" .
chmod +x sentinel-server

# Client: create .app bundle and run macdeployqt so Qt is bundled
APP_NAME="Sentinel.app"
APP_CONTENTS="${APP_NAME}/Contents"
APP_MACOS="${APP_CONTENTS}/MacOS"
mkdir -p "$APP_MACOS"
cp "$GUI_BIN" "${APP_MACOS}/sentinel-gui"
chmod +x "${APP_MACOS}/sentinel-gui"

# Minimal Info.plist so the app is valid and runs from the right cwd
cat > "${APP_CONTENTS}/Info.plist" << 'PLIST'
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

# macdeployqt: bundle Qt frameworks/plugins so the app is self-contained
MACDEPLOYQT=""
for candidate in "/opt/homebrew/opt/qt/bin/macdeployqt" "/usr/local/opt/qt/bin/macdeployqt" "$(which macdeployqt 2>/dev/null)"; do
  if [[ -n "$candidate" && -x "$candidate" ]]; then
    MACDEPLOYQT="$candidate"
    break
  fi
done
QML_DIR_ABS="${SENTINEL_ROOT}/libs/gui/qml"
if [[ -n "$MACDEPLOYQT" ]]; then
  echo "Running macdeployqt to bundle Qt frameworks and QML plugins..."
  MACDEPLOYQT_DIR="$(dirname "$MACDEPLOYQT")"
  APP_ABS="$(pwd)/${APP_NAME}"
  if [[ -d "$QML_DIR_ABS" ]]; then
    (cd "$MACDEPLOYQT_DIR" && ./macdeployqt "$APP_ABS" "-qmldir=$QML_DIR_ABS" -no-strip) || \
    "$MACDEPLOYQT" "$APP_NAME" "-qmldir=$QML_DIR_ABS" -no-strip
  else
    (cd "$MACDEPLOYQT_DIR" && ./macdeployqt "$APP_ABS" -no-strip) || \
    "$MACDEPLOYQT" "$APP_NAME" -no-strip
  fi
  echo "Ad-hoc signing app bundle (required so macOS accepts copied frameworks)..."
  codesign --force --deep --sign - "$APP_NAME"
else
  echo "macdeployqt not found (optional). Install Qt via Homebrew (brew install qt) or set PATH so macdeployqt is available, then re-run this script to bundle Qt. The app may need DYLD_LIBRARY_PATH set to your Qt libs otherwise."
  # Sign the bare .app anyway so the main binary is valid
  codesign --force --deep --sign - "$APP_NAME" 2>/dev/null || true
fi

# README
if [[ -f "${SENTINEL_ROOT}/scripts/release/LAUNCH_README.md" ]]; then
  cp "${SENTINEL_ROOT}/scripts/release/LAUNCH_README.md" ./LAUNCH_README.md
fi
if [[ -f "${SENTINEL_ROOT}/README-RUN.md" ]]; then
  cp "${SENTINEL_ROOT}/README-RUN.md" ./README.md
elif [[ -f "${SENTINEL_ROOT}/README.md" ]]; then
  cp "${SENTINEL_ROOT}/README.md" ./README.md
fi

# Launcher: start server in background, then run client (same shell so CWD = package dir for config/certs)
# SENTINEL_QML_PATH makes the main QML view load from package libs/gui/qml (avoids build-machine path when distributed)
cat > run.sh << RUNSH
#!/usr/bin/env bash
cd "\$(dirname "\$0")"
if lsof -i :8080 -sTCP:LISTEN -t >/dev/null 2>&1; then
  echo "Port 8080 is in use. Stop any existing server: killall sentinel-server"
  echo "Then run ./run.sh again."
  exit 1
fi
export SENTINEL_QML_PATH="\$(pwd)/libs/gui/qml/DepthChartView.qml"
echo "Starting Sentinel server in background..."
./sentinel-server &
SERVER_PID=\$!
sleep 1
echo "Starting Sentinel client (close the client window when done; Ctrl+C here stops the server)."
./Sentinel.app/Contents/MacOS/sentinel-gui
echo "Client exited. Server still running (PID \$SERVER_PID). Press Ctrl+C to stop server, or leave this window open."
wait \$SERVER_PID
RUNSH
chmod +x run.sh

echo "Done. Contents of $TARGET_DIR:"
ls -la
echo ""
echo "To run: cd $TARGET_DIR && ./run.sh"
echo "Or run ./sentinel-server in one terminal and ./Sentinel.app/Contents/MacOS/sentinel-gui in another (from this folder)."
