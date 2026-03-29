# Sentinel

High-performance **GPU-accelerated trading terminal** built with **C++20** and **Qt 6**.

A desktop workstation for visualizing market structure, order flow, and real-time data — powered by a custom rendering pipeline and a client/server architecture designed for speed.

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

High-density order book visualization with real-time updates and smooth interaction.

![Heatmap render demo](docs/assets/gifs/heatmap-render.gif)

---

### Stock Chart + SEC Insider Signals

Integrated equity charting with insider transaction overlays and contextual signals.

![Stock chart insider signals demo](docs/assets/gifs/stock-chart-insiders.gif)

---

### Screener Workflow

Fast symbol discovery and routing into the charting system.

![Stock screener demo](docs/assets/gifs/stock-screener.gif)

---

## Overview

Sentinel is built around a single constraint:

> dense market data should remain fluid, interactive, and interpretable under load

The system combines:

- GPU-first rendering (no per-frame allocations)
- Real-time streaming data pipelines
- Desktop workstation UI (Qt + QML + Scene Graph)
- Trading and simulation infrastructure

---

## Core Capabilities

### Rendering & Charting

- GPU-accelerated heatmap (single-quad texture sampling)
- ~110+ FPS during pan/zoom with live axis updates
- Zero-allocation axis rendering
- Candle, hollow, and line chart modes
- Consistent viewport mapping across overlays
- High-density grid support

---

### Market Tools

- Live heatmap chart
- Candle overlays aligned to heatmap mapping
- Stock chart workspace
- SEC insider signal overlays
- Watchlists with smart routing
- TradingView screener integration
- Order book / DOM ladder

---

### Trading & Simulation

- Paper trading infrastructure
- TP/SL bracket handling (server-backed)
- Replay and backtesting groundwork
- Shared simulation core
- Algo integration foundation

---

### Platform Architecture

- Client/server split
- Custom stream protocol
- TLS / WSS transport support
- Config-driven runtime
- Deterministic render/data flow

---

## Recent Platform Expansion

The latest integration introduced a major expansion across rendering, trading, and system architecture. :contentReference[oaicite:0]{index=0}

Highlights:

- Unified chart rendering architecture
- Major `UnifiedGridRenderer` refactor
- Paper trading + simulation vertical slice
- Stock chart + SEC overlays
- Watchlist, screener, and DOM improvements
- Transport security (TLS / WSS)

TPO, Footprint, and Volume Profile are implemented at a foundational level but remain disabled in the UI until complete.

---

## Quick Start

### Run a Release

Download the latest build:

https://github.com/pattty847/Sentinel-Trading-Terminal/releases

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
````

Run:

```powershell
build/windows-msvc/apps/sentinel_gui/Release/sentinel_gui.exe
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

## Run

Start server:

```bash
./build/linux-gcc/bin/sentinel-server
```

Start GUI:

```bash
./build/linux-gcc/apps/sentinel_gui/sentinel_gui
```

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