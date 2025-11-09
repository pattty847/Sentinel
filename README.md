# **Sentinel: GPU-Accelerated Trading Terminal**

> Sub-millisecond market visualization powered by modern C++20 and Qt 6.
> Licensed under the **GNU AGPL v3** — open source with strong copyleft.

---

## 🖥️ Screenshots

| Dockable Widget Layout *(branch: dockable-widgets)* | Liquidity Heatmap *(branch: main)* |
|:-----------------------------------------:|:--------------------------------:|
| <img src="https://github.com/user-attachments/assets/9d3ac4b5-eedb-44d3-855c-b03a7d6ac66b" width="600"/> | <img src="https://github.com/user-attachments/assets/27a969f2-1a02-4e69-aee6-ff6b26411779" width="600"/> |

> *Sneak peek of the new dockable UI framework — modular panels, tabbing, and detachable layouts like professional trading terminals.*

---

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20">
  <img src="https://img.shields.io/badge/Qt-6-green.svg" alt="Qt6">
  <img src="https://img.shields.io/badge/Architecture-GPU_Accelerated-purple" alt="GPU Accelerated">
  <img src="https://img.shields.io/badge/Platform-Cross_Platform-lightgrey.svg" alt="Cross-Platform">
  <img src="https://img.shields.io/badge/License-AGPL--3.0-blue" alt="License">
  <img src="https://img.shields.io/badge/status-under_development-orange" alt="Status">
</p>

<div align="center">

**[🚀 Quick Start](#quick-start) • [⚡ Performance](#performance) • [🏗️ Architecture](#architecture) • [📚 Documentation](#documentation)**

</div>

---

## 🧭 Why Sentinel Exists

Existing crypto visualizers are either laggy, CPU-bound, or stuck in browsers.
Sentinel pushes GPU-accelerated rendering to its limits — native, real-time, and extensible.
It’s a sandbox for exploring lock-free architectures, high-frequency visualization, and next-gen UI design.

---

## ⚙️ What Is Sentinel?

A professional-grade trading terminal built in **C++20 / Qt 6**, streaming live market data from Coinbase Advanced Trade API and rendering dense order-book heatmaps at institutional speed.

**Highlights**

* **Sub-ms Rendering:** GPU-driven scene graph with triple buffering
* **Modern C++:** Concepts, ranges, and zero-allocation hot paths
* **Threaded Pipelines:** Lock-free cross-thread handoff between data + renderer
* **Cross-Platform:** D3D11 (Windows), Metal (macOS), OpenGL (Linux)

---

## 🚀 TL;DR Quickstart

```bash
git clone https://github.com/pattty847/Sentinel.git
cmake --preset windows-mingw && cmake --build --preset windows-mingw -j
./build-windows-mingw/apps/sentinel_gui/sentinel_gui
```

> Detailed setup for macOS, Linux, and Windows below.

---

## 📦 Quick Start (Detailed)

<details><summary><b>macOS</b></summary>

```bash
xcode-select --install
brew install qt cmake ninja
```

</details>

<details><summary><b>Linux (Ubuntu/Debian)</b></summary>

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build qt6-base-dev \
    qt6-declarative-dev libgl1-mesa-dev libssl-dev
```

</details>

<details><summary><b>Windows</b></summary>

1. Install [MSYS2](https://www.msys2.org/)
2. Install [Qt 6.5+](https://www.qt.io/download-qt-installer)
3. In MSYS2 terminal:

```bash
pacman -S cmake ninja mingw-w64-x86_64-gcc
```

</details>

---

## 📊 Performance

| Metric         | Before   | After GPU Acceleration | Δ              |
| -------------- | -------- | ---------------------- | -------------- |
| Paint Time     | ~1500 ms | **0.7–1.7 ms**         | 2 000× faster  |
| Cache Lookup   | 1.1 s    | **20–130 µs**          | 10 000× faster |
| Slice Coverage | 8–9      | **54+**                | 6× denser      |
| Frame Rate     | Stalls   | **28–30 Hz**           | Vsynced Match  |
| Cells Rendered | —        | **6 000 +/frame**      | Zero stalls    |

**System Impact**

* GPU Utilization ↑ to 30 % (true acceleration)
* Zero mallocs in hot paths
* Asynchronous thread-safe snapshots

---

## 🧠 Architecture Overview

Sentinel uses a **three-layer modular architecture**:

### 1. Core (`libs/core`)

Pure C++ business logic (no Qt GUI deps)

* Market data transport + cache
* O(1) order-book engine (5 M levels, *adjustable*)
* Dyanmic Time-series LOD aggregation (100 ms → 10 s, *configurable*)
* Unified performance monitor

### 2. GUI (`libs/gui`)

Qt6-based architecture + render strategies

* GPU scene-graph renderer
* DataProcessor thread handoff
* Pluggable render modes (Heatmap, Trades, Flow)
* Future: dockable panels, AI commentary, multi-chart layouts

### 3. Apps (`apps/`)

Thin launchers:

* **sentinel_gui:** full trading terminal
* **stream_cli:** headless data streamer for an upcoming client/server architecture

### 4. Upcoming Module: SEC Filing Viewer

> **Status:** In Development
> *Real-time insider trading, filings, and financial data integrated directly into Sentinel’s AI and dockable layout.*

The **SEC Filing Viewer** will bring corporate fundamentals into the same interface as market data — letting you visualize **insider Form 4 trades**, **earnings filings**, and **company financials** right beside live price action.
It’s powered by the U.S. SEC EDGAR API and a custom async fetch/cache pipeline (see [SEC API README](https://github.com/pattty847/Sentinel/blob/main/sec/SEC_API_README.md)).

**Planned Capabilities**

* **Form 4 Insider Transactions** — visualized chronologically or overlaid on the chart
* **Recent Filings** — timeline markers for 10-K, 8-K, S-1, etc.
* **Financial Summaries** — key metrics (EPS, revenue, assets) cached and AI-readable
* **AI Integration** — natural-language summaries, “why did insiders buy?”-type explanations

```text
Sentinel Core ↔ SEC Viewer Dock ↔ AI Commentary Module
```

**Design Principles**

* Separation of concerns
* Strategy pattern for render modes
* RAII + smart pointers everywhere
* <500 LOC per file enforcement

---

## 🧩 GPU Backend Selection

```cpp
#ifdef Q_OS_WIN
qputenv("QSG_RHI_BACKEND", "d3d11");
#elif defined(Q_OS_MACOS)
qputenv("QSG_RHI_BACKEND", "metal");
#else
qputenv("QSG_RHI_BACKEND", "opengl");
#endif
```

Platform-optimized graphics backends are automatically selected at runtime.

---

## 🔐 Configuration

Create a `key.json` file in the project root with your Coinbase Advanced Trade API credentials:

```json
{
  "key": "organizations/YOUR_ORG/apiKeys/YOUR_KEY_ID",
  "secret": "-----BEGIN EC PRIVATE KEY-----\nYOUR_PRIVATE_KEY\n-----END EC PRIVATE KEY-----\n"
}
```

---

## 🧰 Logging & Debugging

Use Qt’s categorized logging:

```bash
export QT_LOGGING_RULES="sentinel.render=true;sentinel.data.debug=true"
```

See [docs/LOGGING_GUIDE.md](docs/LOGGING_GUIDE.md) for full details.

---

## 📚 Documentation

* [Architecture](docs/ARCHITECTURE.md) — detailed component flow
* [Logging Guide](docs/LOGGING_GUIDE.md) — categorized Qt logging
* [Code Analysis Tools](scripts/README_CODE_ANALYSIS.md)
* [Coinbase WebSocket API](https://docs.cloud.coinbase.com/exchange/docs/websocket-overview)
* [Qt Scene Graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)

---

## 🧪 Contribution Standards

* **C++ 20 only** — concepts, ranges, structured bindings
* **RAII everywhere** — no manual delete
* **Lock-free hot paths** — zero contention
* **File limit:** 500 LOC hard cap
* **Core ≠ GUI** — strict separation

**Workflow**

1. Fork + feature branch
2. Implement & test
3. Run `ctest`
4. Submit PR with performance notes

**Recent Wins**

* ✅ GPU scene graph acceleration (2000× paint speedup)
* ✅ Async snapshots (no frame blocking)
* ✅ Chunked geometry for Windows ANGLE limit
* ✅ Platform-native backends (Metal/D3D11/OpenGL)
* ✅ Graceful shutdown / thread cleanup

---

## 📄 License

Licensed under **GNU Affero General Public License v3.0 (AGPL-3.0)**.
Any network service built with Sentinel must remain open-source.

See [LICENSE](LICENSE) for full text.

---

<div align="center">

**Built for institutional-grade market analysis**
*Sub-millisecond rendering • Lock-free pipelines • GPU acceleration*

</div>
