# Release packaging

## Mac

1. **Build release** (from repo root, in a terminal where `clang++` and vcpkg are available):
   ```bash
   export VCPKG_ROOT=$HOME/vcpkg
   export QT_MAC=/opt/homebrew/opt/qt
   cmake --preset mac-clang-release
   cmake --build --preset mac-clang-release -j
   ```

2. **Package** (copies binaries, config, certs, README, creates `Sentinel.app` and `run.sh`):
   ```bash
   ./scripts/release/mac-package-release.sh
   ```
   Default output: `~/Desktop/Sentinel`. Override with:
   ```bash
   ./scripts/release/mac-package-release.sh /path/to/output
   ```

3. **macdeployqt**: The script looks for `macdeployqt` (Homebrew Qt: `/opt/homebrew/opt/qt/bin/macdeployqt`) and runs it on `Sentinel.app` to bundle Qt frameworks so the app is self-contained. It then runs **`codesign --force --deep --sign - Sentinel.app`** so macOS accepts the copied frameworks (otherwise you get "Code Signature Invalid" when launching). If macdeployqt is not found, the app is still created but may need Qt in `DYLD_LIBRARY_PATH` or the user must have Qt installed. Install Qt with `brew install qt` if needed.

4. **Run**: User can `cd ~/Desktop/Sentinel && ./run.sh` to start server and client in one go.

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
