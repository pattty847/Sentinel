# Release checklist

Operational notes for Sentinel release bundles (portable folders + archives). Targets are scripted; this document captures what to verify manually and known gaps.

## macOS

1. **Environment** (maintainer machine): Xcode CLT or full Xcode for `codesign`; Homebrew Qt (`brew install qt`) so `macdeployqt` bundles frameworks; CMake + Ninja via Homebrew if not already installed.

2. **Build + package:**
   ```bash
   export VCPKG_ROOT=$HOME/vcpkg
   export QT_MAC=/opt/homebrew/opt/qt
   cmake --preset mac-clang-release
   cmake --build --preset mac-clang-release -j
   ./scripts/release_macos.sh
   ```

3. **Default output:** `dist/Sentinel-macos-YYYYMMDD/`, **`MANIFEST.txt`**, **`README_RELEASE.md`** (bundled onboarding), **`run.sh`** (sets **`cwd`** to bundle root, exports **`SENTINEL_QML_PATH`**, server → **`logs/sentinel-server.log`**), plus `dist/Sentinel-macos-YYYYMMDD.zip`.

4. **Python backends:** Packaging copies `scripts/` (without `.venv/`, `data/` or `dev/`), builds a `copetech-edgar` wheel from the CopeTech-Edgar release tag (`COPETECH_EDGAR_REF`, default `v0.2.0`; repo from `COPETECH_EDGAR_REPO`, default `../CopeTech-Edgar`) into `third_party/wheels/`, points the staged `scripts/pyproject.toml` at it, and runs `uv sync` if `uv` is on PATH. If the tag is missing, packaging warns and SEC overlays will not install.

5. **Automated sanity (staged folder inside `dist/`):**
   ```bash
   ./scripts/smoke_macos.sh
   ```
   **Clean-room — simulates a user download:** unzips the newest `dist/Sentinel-macos-*.zip` under **`/tmp/Sentinel-release-test.XXXXXX`** (or pass an explicit `.zip`), runs smoke from that extraction only, checks that staged **text/configs** do not contain the builder’s **`$PWD` Sentinel clone path** and **`strings`** on binaries by default (**`SENTINEL_IGNORE_BINARY_PATH_SCAN=1`** skips Mach-O probe if your Release still embeds `CMAKE_CURRENT_SOURCE_DIR`):
   ```bash
   ./scripts/smoke_macos.sh --clean-room [/path/to/Sentinel-macos-YYYYMMDD.zip]
   ```
## Windows

1. Package script (maintainer docs): `scripts/release/windows-package-release.ps1` — see `scripts/release/README.md`.
2. **Placeholder:** Formal Windows release QA steps mirror macOS smoke (adapt paths to `.exe` and launcher `run.cmd`).

## Manual sanity checks (any platform)

After extracting a packaged folder:

| Check | Expected |
|--------|-----------|
| `config/server_config.yaml`, `config/client_config.yaml` present | Yes |
| `resources/certs/ca-bundle.crt` present | Yes (REST/TLS paths) |
| `certs/sentinel-server.crt` / `.key` or generate via bundled script | Yes or gen-certs succeeds |
| `MANIFEST.txt` + git hash | Present (producer metadata) |
| `README_RELEASE.md` | Bundled onboarding |

Run server then GUI (or platform launcher). Subscribe to **`BTC-USD`** (Coinbase **public** stream — **no credential** onboarding path).

---

## Feature smoke matrix

| Capability | Scripted check | Manual |
|-------------|-----------------|--------|
| Server boots | `scripts/smoke_macos.sh`; optional `./scripts/smoke_macos.sh --clean-room [zip]` (health ping; fail-fast log tails) | — |
| GUI boots | Not automated headless (`run.sh` is interactive window) | Open app |
| Coinbase stream connects | REST sanity in smoke; WebSocket lifecycle not scripted | Subscribe to `BTC-USD` in GUI |
| Heatmap renders | — | Visible grid after subscribe |
| Stock chart opens | — | Workspace / chart dock |
| Screener opens | — | Screener UI + server invokes `scripts/screener/` |
| SEC overlay loads | Depends on Python + `copetech-edgar`; may need CopeTech-Edgar | Stock chart ticker + SEC widgets |
| DOM / order book view opens | — | DOM dock |
| Config load/save | — | Settings / persisted client config |

## Known gaps (documented TODOs)

1. **Headless GUI smoke:** Not automated via CI without a harness (Qt/macOS lifecycle, GPU). Covered by `./run.sh` manual run.
2. **Coinbase WebSocket:** Smoke uses **public REST** only; authenticated channels are unchanged from main app semantics.
3. **Python `copetech-edgar`:** Development layout uses `{ path = "../../CopeTech-Edgar", editable = true }` (sibling of repo root). Releases bundle a wheel built from a tagged CopeTech-Edgar release in **`third_party/wheels/`** and patch the staged `scripts/pyproject.toml` to install it; the working tree of the sibling checkout is never copied.
