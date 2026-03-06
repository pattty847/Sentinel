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
