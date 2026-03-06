# Sentinel v2.1.0-alpha

Open source GPU-accelerated order book heatmap trading terminal. Written in C++20 and Qt 6, with a server/client architecture and GPU-first rendering.

<div align="center">
  <img src="https://img.shields.io/badge/C++-20-blue" />
  <img src="https://img.shields.io/badge/Qt-6-green" />
  <img src="https://img.shields.io/badge/GPU-Accelerated-purple" />
  <img src="https://img.shields.io/badge/Platform-Cross--Platform-lightgrey" />
  <img src="https://img.shields.io/badge/License-AGPL--3.0-blue" />
</div>

## Project Status

The repository `main` branch is currently behind the latest runnable preview build.

Active development is happening on `tpo-footprint-v1`, which has been a long-running feature branch for the new TPO / footprint work and related server/client improvements.

If you are a **non-developer** and just want to run Sentinel, use the latest pre-release from the GitHub Releases page:

- [Download the latest release](https://github.com/pattty847/Sentinel-Trading-Terminal/releases/tag/Alpha-Testing)
- [View all releases](https://github.com/pattty847/Sentinel-Trading-Terminal/releases)

The current public pre-release includes:

- Bundled dependencies for end users
- A runnable server/client package
- A simple launch script
- Live heatmap rendering
- Live candles chart
- Dockable UI layout
- TradingView screener integration
- Heatmap tuning controls

Work in progress on the feature branch:

- TPO / footprint chart
- Additional heatmap and candle timeframes beyond the current 1s flow
- Stock candles widget
- SEC filing viewer

`main` remains the stable landing page for the project, but the latest public preview build currently comes from `tpo-footprint-v1`.

## Main Branch Overview

<img width="2559" height="1388" alt="Screenshot 2026-02-02 141503" src="https://github.com/user-attachments/assets/b0d32701-816d-4508-aa1b-9753eeb7b601" />
<img width="2559" height="1392" alt="image" src="https://github.com/user-attachments/assets/4e100ad3-4da9-42d1-b75b-22f71b9f0c07" />

### Features
- Full Server Client architecture
- Entirely GPU based heatmap rasterization and rendering
- One quad, texture sampling
- **110+ FPS with live axis updates during pan/zoom**
- Zero-allocation axis rendering (fixed-capacity models, no QML object churn)
- Nice ticks with hysteresis on price and time axes
- 8192 x 8192 - 67M Cells, easily can push higher
- TWAP Heatmap Cell Aggregation
- Full Docking Framework
- Qt RHI backend for any OS

---

## Build

**Windows:**

```
setx QT_MSVC C:\Qt\6.9.3\msvc2022_64
setx VCPKG_ROOT C:\dev\vcpkg

git clone https://github.com/pattty847/Sentinel.git
cd Sentinel
cmake --preset windows-msvc
cmake --build --preset windows-msvc -j
```

Run: `build/windows-msvc/apps/sentinel_gui/Release/sentinel_gui.exe`

**macOS:**

```
brew install qt cmake ninja
export QT_MAC=/opt/homebrew/opt/qt
export VCPKG_ROOT=$HOME/vcpkg

cmake --preset mac-clang
cmake --build --preset mac-clang -j
```

**Linux:**

```
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-declarative-dev
export QT_LINUX=/usr/lib/qt6
export VCPKG_ROOT=$HOME/vcpkg

cmake --preset linux-gcc
cmake --build --preset linux-gcc -j
```

---

## Run

Server:

```
./build/linux-gcc/bin/sentinel-server
```

GUI Client:

```
./build/linux-gcc/apps/sentinel_gui/sentinel_gui
```

---

## Configuration

Sentinel uses separate YAML configs for server-authoritative settings and client-only UI preferences.

**Setup:**

```bash
# Server config (authoritative)
cp config/server_config.yaml config/.server_config.yaml

# Client config (local UI prefs)
cp config/client_config.yaml config/.client_config.yaml
```

**Config Priority:**

1. `config/.server_config.yaml` overrides `config/server_config.yaml`
2. `config/.client_config.yaml` overrides `config/client_config.yaml`

**Common settings:**

```yaml
heatmap:
  timeframe: 1000
  grid_width: 2048
  grid_height: 1024

server:
  default_symbols: BTC-USD

gui:
  api_port: 17100
  screenshot_dir: ./screenshots
```

See `config/server_config.yaml` and `config/client_config.yaml` for all options.

---

## API Keys

Depending on the branch/build you are using, API key requirements may differ.

For the latest public preview release from `tpo-footprint-v1`, public market data is available without API keys. Future private/account features will still require authenticated credentials.

Older development flows may still reference a `key.json` in the project root:

```json
{
  "key": "your-coinbase-api-key",
  "secret": "-----BEGIN EC PRIVATE KEY-----\n...\n-----END EC PRIVATE KEY-----\n"
}
```

---

## Project Structure

```
libs/core/    Pure C++ data layer (no Qt GUI)
libs/gui/     Qt Quick, QSG rendering, widgets
apps/         Executables (sentinel_gui, sentinel-server)
```

Core handles market data, caching, and transforms. GUI handles all rendering and layout. They don't mix.

---

## Docs

- `docs/ARCHITECTURE.md` — dataflow and rendering pipeline
- `docs/MARKETDATA.md` — MarketDataCoreEngine pipeline
- `docs/FEATURES.md` - Outline of features for the terminal

---

## License

AGPL-3.0




