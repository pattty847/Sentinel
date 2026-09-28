# Release packaging

## Mac

1. **Build release** (from repo root, same env as CMake preset `mac-clang-release`):
   ```bash
   export VCPKG_ROOT=$HOME/vcpkg
   export QT_MAC=/opt/homebrew/opt/qt
   cmake --preset mac-clang-release
   cmake --build --preset mac-clang-release -j
   ```

2. **Package** — primary entry points:
   ```bash
   ./scripts/release_macos.sh
   ```
   - Default staging: **`dist/Sentinel-macos-YYYYMMDD/`** (+ zip sibling).
   - Optional: `./scripts/release_macos.sh [--build] [--no-zip] [STAGING_DIR]`  
     (`scripts/release/mac-package-release.sh` delegates here for backwards compatibility.)

3. **Contents:** GUI **`Sentinel.app`**, **`SentinelServer.app`**, **`config/`**, **`certs/`**, **`resources/certs/ca-bundle.crt`**, **`libs/gui/qml`**, **`scripts/`** (+ `uv sync` when **`uv`** is installed), **`LICENSE`**, **`README_RELEASE.md`** (user onboarding), **`MANIFEST.txt`** (git revision + bundled layout hints), **`LAUNCH_README.md`**, copy of **`README.md`**, **`run.sh`** (sets **`cwd`**, **`SENTINEL_QML_PATH`**, server **`logs/sentinel-server.log`**).

4. **macdeployqt**: Looks for `/opt/homebrew/opt/qt/bin/macdeployqt`, then `$PATH`; runs against both app bundles so **GUI and server ship Qt frameworks**. Ad-hoc `codesign --force --deep --sign - …` follows (required after copying frameworks).

5. **Smoke tests:** `./scripts/smoke_macos.sh [STAGED_DIR]` (default newest `dist/Sentinel-macos-*`). **Clean-room unzip:** `./scripts/smoke_macos.sh --clean-room [/path/to/zip]` — expands under `/tmp/Sentinel-release-test.*`; fails if bundle text/binary embed the **checkout path** (`SENTINEL_IGNORE_BINARY_PATH_SCAN=1` optional). Details: **`docs/RELEASE_CHECKLIST.md`**.

6. **Run:** From the unpacked folder, **`README_RELEASE.md`** first, then `./run.sh` (or launchers under `SentinelServer.app` / `Sentinel.app` per **`LAUNCH_README.md`**).
## Windows

1. **Build release** (from repo root, in a VS Developer PowerShell or terminal with MSVC env):
   ```powershell
   cmake --preset windows-msvc-vs
   cmake --build --preset windows-msvc-vs --config Release
   ```

2. **Package** (copies binaries, runtime DLLs/plugins, config, certs, QML, and creates launchers):
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/release/windows-package-release.ps1
   ```
   Default output: `~/Desktop/Sentinel` (Windows: `C:\Users\<you>\Desktop\Sentinel`).

   Optional one-command build + package:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/release/windows-package-release.ps1 -Build
   ```

   Optional custom output folder:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/release/windows-package-release.ps1 -TargetDir "C:\Releases\Sentinel"
   ```

3. **Run package**:
   ```powershell
   cd "$HOME\Desktop\Sentinel"
   .\run.cmd
   ```
