#!/bin/bash
# Build SentinelBar.app: a menu bar panel for the build queue, worktrees, services and AI budget.
#
#   tools/SentinelBar/build-app.sh             # -> tools/SentinelBar/build/SentinelBar.app
#   tools/SentinelBar/build-app.sh --install   # also copy to ~/Applications and launch it
#
# Ad-hoc signed (codesign -s -). It never uses the "Sentinel Local Code Signing" identity.
set -euo pipefail
cd "$(dirname "$0")"
../../scripts/dev/build-queue.sh --label SentinelBar-release -- swift build -c release
bin="$(swift build -c release --show-bin-path)/SentinelBar"
app=build/SentinelBar.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/SentinelBar"
cp Sources/SentinelBar/Resources/*.png "$app/Contents/Resources/"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>SentinelBar</string>
  <key>CFBundleIdentifier</key><string>com.sentinel.bar</string>
  <key>CFBundleName</key><string>SentinelBar</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSUIElement</key><true/>
</dict></plist>
PLIST
codesign --force --sign - "$app"
echo "built $PWD/$app"
if [ "${1:-}" = --install ]; then
    pkill -x SentinelBar 2>/dev/null && sleep 1 || true   # LaunchServices -600 if reopened too soon
    rm -rf "$HOME/Applications/SentinelBar.app"
    mkdir -p "$HOME/Applications"
    cp -R "$app" "$HOME/Applications/"
    open "$HOME/Applications/SentinelBar.app"
    echo "installed ~/Applications/SentinelBar.app"
fi
