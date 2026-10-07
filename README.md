# Sentinel

**GPU-accelerated trading terminal** built with **C++20** and **Qt 6**.

A desktop workstation for viewing market structure, order flow, and real-time data. A headless server ingests and records Coinbase market data. A Qt client draws it on the GPU. Orders are simulated: Sentinel has paper trading only and sends no real orders.

<div align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue" />
  <img src="https://img.shields.io/badge/Qt-6-green" />
  <img src="https://img.shields.io/badge/GPU-Accelerated-purple" />
  <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey" />
  <img src="https://img.shields.io/badge/License-AGPL--3.0-blue" />
</div>

---

## Showcase

### GPU Heatmap Rendering

Order book heatmap with live updates, pan, and zoom.

![Heatmap render demo](docs/assets/gifs/heatmap-render.gif)

---

### Stock Chart + SEC Insider Signals

Equity charting with insider transaction overlays and signals from SEC filings.

![Stock chart insider signals demo](docs/assets/gifs/stock-chart-insiders.gif)

---

### Screener Workflow

Screener rows open on the charts: crypto pairs on the main chart, stocks on the Stock Chart.

![Stock screener demo](docs/assets/gifs/stock-screener.gif)

---

## Overview

Sentinel is built around a single constraint:

> dense market data should remain fluid, interactive, and interpretable under load

The system combines:

- GPU-first rendering
- Real-time streaming data pipelines
- Recorded order book history
- Desktop workstation UI (Qt Widgets docks, QML, Qt Scene Graph)
- Paper trading and simulation

---

## Core Capabilities

### Rendering & Charting

- GPU order book heatmap with recorded history and a live edge
- Heatmap binned on the GPU at the chart's tick size (Auto or Manual)
- Liquidity range slider (sizes in the base asset) and labels on heatmap cells in Asset or USD units
- Candle, hollow, and line chart modes
- Trade bubbles for executions
- Footprint, TPO, and Volume Profile layers, switched on from the chart toolbar
- Axis label models of fixed size, so pan and zoom do not add or remove label items
- Consistent viewport mapping across overlays

---

### Market Tools

- Heatmap chart with candle overlays aligned to the heatmap mapping
- Stock Chart dock (daily candles for equities)
- SEC insider signal overlays (Form 4 filings from SEC EDGAR)
- Watchlists: crypto pairs open on the main chart, stocks on the Stock Chart
- TradingView screener dock (crypto and stocks)
- Order book (DOM) ladder dock

---

### Trading & Simulation

- Paper trading on the server: market and limit orders, positions, and PnL. No real orders are sent.
- Trade hotkeys (B, S, F, C) and a Paper Trading dock
- TP/SL brackets on an open position, held and triggered by the server
- Backtesting: `sentinel-backtest` replays a trade file (CSV or binary trade log) through the same simulation core. The Paper Trading dock has a Backtest tab for it.
- Algo: a market-making algo (Avendella MM) runs in paper mode. Start and stop it from the Paper Trading dock.

---

### Platform Architecture

- Client/server split
- Custom stream protocol over WebSocket
- Client and server talk over TLS (WSS). The server listens on 127.0.0.1 by default.
- Config-driven runtime (YAML server and client configs)

---

## Recent Platform Expansion

Changes that landed in September and October 2026:

- The GPU heatmap is the only heatmap renderer. The client bins recorded and live order book data on the GPU and draws it. The earlier renderer is removed from the GUI and client.
- `sentinel-capture` is a separate process that records a raw Coinbase level 2 journal, with one connection per product. `sentinel-roll` builds heatmap history files from that journal.
- Candle history loads from Coinbase in pages of 350 bars as you pan back. The server caches closed bars.
- Chart controls: Auto or Manual tick size, a liquidity range slider, liquidity labels, trade bubbles, and an auto price scale toggle.
- Footprint, TPO, and Volume Profile are drawn in the GUI. Switch them on with the chart toolbar buttons. TPO and Volume Profile replace the heatmap while they are on.

---

## Quick Start

### Run a Release (recommended)

1. Download the latest **[release](https://github.com/pattty847/Sentinel-Trading-Terminal/releases)** artifact for your platform. Maintainers publish macOS archives from `./scripts/release_macos.sh` (under `dist/`; see **`docs/RELEASE_CHECKLIST.md`**) and Windows folders via **`scripts/release/windows-package-release.ps1`** (see **`scripts/release/README.md`**).
2. **macOS Zip:** unzip, then `./run.sh` from that folder. This starts **`SentinelServer.app`** plus **`Sentinel.app`** (`SENTINEL_QML_PATH` is set by the launcher). **Windows package:** **`run.cmd`** from the unpacked folder (**`scripts/release/README.md`**).

   No API keys are required for **live Coinbase public** market data (`BTC-USD` is configured by default).

3. Pick a Coinbase spot symbol and stream from the GUI (magnifying glass / symbol entry) — the fastest demo path is the **public** websocket (no signup).

Automated sanity for a staged folder (maintainers): `./scripts/smoke_macos.sh`

**See it in motion:** Showcase GIFs are in **[Showcase](#showcase)** below (heatmap, stock chart + SEC insiders, screener).

**After launch — what should work**

| Item | Notes |
|------|--------|
| Server starts | `./run.sh` or `scripts/smoke_macos.sh`; health pings `127.0.0.1` (default HTTP health port configurable via **`SENTINEL_HEALTH_PORT`**) |
| GUI window opens | If it does not, check TLS certs and `resources/certs/ca-bundle.crt` |
| Coinbase spot stream | Public symbols (**no credentials**) — subscribe in client |
| Python-backed features | SEC overlays, candle fetch via `uv`-managed `scripts/` — run **`brew install uv`**, **`(cd scripts && uv sync)`** inside the unpacked folder if overlays fail |

Maintainers: reproducible bundles from source use **`cmake --preset mac-clang-release`**, **`./scripts/release_macos.sh`** (see **`scripts/release/README.md`**).

---

### Build from Source

#### Windows

```powershell
setx QT_MSVC C:\Qt\6.9.3\msvc2022_64
setx VCPKG_ROOT C:\dev\vcpkg

git clone https://github.com/pattty847/Sentinel.git
cd Sentinel
cmake --preset windows-msvc
cmake --build --preset windows-msvc -j
```

Run:

```powershell
build/windows-msvc/apps/sentinel-gui/Release/sentinel-gui.exe
```

---

#### macOS

```bash
brew install qt cmake ninja
export QT_MAC=/opt/homebrew/opt/qt
export VCPKG_ROOT=$HOME/vcpkg

git clone https://github.com/pattty847/Sentinel.git
cd Sentinel
cmake --preset mac-clang
cmake --build --preset mac-clang -j
```

---

#### Linux

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-declarative-dev
export QT_LINUX=/usr/lib/qt6
export VCPKG_ROOT=$HOME/vcpkg

git clone https://github.com/pattty847/Sentinel.git
cd Sentinel
cmake --preset linux-gcc
cmake --build --preset linux-gcc -j
```

---

## Run (from source build tree)

CMake uses output name **`sentinel-gui`** and **`sentinel-server`** under **`build/<preset>/apps/sentinel-gui/<Config>/`** and **`apps/sentinel-server/<Config>/`** (exact path depends on platform and preset). Adjust paths accordingly — example:

```bash
./build/linux-gcc/apps/sentinel-server/Release/sentinel-server
./build/linux-gcc/apps/sentinel-gui/Release/sentinel-gui
```

Start **server before client**, from the repository root.

The stream server uses TLS with a per-machine self-signed pair that is never committed. Generate it once before the first run:

```bash
bash certs/gen-certs.sh
```

(Windows: `certs/gen-certs.ps1`.) Without it the server logs `SentinelStreamServer start failed` and clients cannot connect, but the process keeps recording. A port already in use has the same behaviour. Invalid `server.bind_address` configuration instead exits before recording or feeds start.

For **macOS bundles** produced off this repo see **`scripts/release_macos.sh`** and **`LAUNCH_README.md`** bundled with releases.

---

## Configuration

```bash
cp config/server_config.yaml config/.server_config.yaml
cp config/client_config.yaml config/.client_config.yaml
```

Example:

```yaml
heatmap:
  timeframe: 1000
  grid_width: 2048
  grid_height: 1024

server:
  default_symbols: BTC-USD

gui:
  api_port: 17100
```

---

## Architecture

```
libs/core/    Data layer (protocol, market data, simulation)
libs/gui/     Qt rendering + UI system
apps/         Client, server, tools
```

Single authoritative viewport shared across all rendering layers.

---

## License

AGPL-3.0