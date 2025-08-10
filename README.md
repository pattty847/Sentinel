# Sentinel: Ultra-High-Performance GPU Financial Charting

🚀 **Real-time cryptocurrency market analysis with direct-to-GPU visualization capable of 52 MILLION operations/second.**

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20">
  <img src="https://img.shields.io/badge/Qt-6-green.svg" alt="Qt6">
  <img src="https://img.shields.io/badge/Renderer-Direct_to_GPU-purple.svg" alt="Direct-to-GPU">
  <img src="https://img.shields.io/badge/FastOrderBook-52M_ops/sec-red.svg" alt="FastOrderBook Performance">
  <img src="https://img.shields.io/badge/Latency-19.2ns-orange.svg" alt="Latency">
  <img src="https://img.shields.io/badge/Status-Production_Ready-brightgreen.svg" alt="Status">
  <img src="https://img.shields.io/badge/Platform-Cross_Platform-lightgrey.svg" alt="Cross-Platform">
</p>

<div align="center">
  Legacy Heatmap: 
  <img width="1101" height="801" alt="Screenshot 2025-07-18 at 1 22 08 AM" src="https://github.com/user-attachments/assets/542d2497-d4e3-43d9-9663-9fbb135423e5" />

  2D-Grid Heatmap:
  <img width="1400" height="927" alt="Screenshot 2025-07-24 at 10 52 36 AM" src="https://github.com/user-attachments/assets/fd3a14c2-80e1-47d7-93b4-99d3623ba819" />
</div>

## ✨ Key Features

### 🎯 V2 Modular Grid Architecture
- **Professional Liquidity Heatmap**: Dense grid orderbook visualization with 2D coordinate aggregation
- **Modular Render System**: Strategy-pattern rendering (Heatmap, TradeFlow, Candle modes)
- **Composable QML Components**: NavigationControls, TimeframeSelector, VolumeFilter extracted for reusability
- **Multi-Timeframe Analysis**: 100ms → 10s temporal aggregation for professional market depth analysis
- **Anti-Spoofing Detection**: Persistence ratio analysis to filter out fake liquidity
- **Volume-at-Price Analysis**: Real-time depth visualization with GPU acceleration

### 🏗️ Clean Architecture
- **UnifiedGridRenderer**: Slim QML adapter delegating to V2 modular system
- **GridViewState**: Pure viewport & gesture handling without Qt dependencies
- **IRenderStrategy**: Pluggable rendering strategies for different visualization modes
- **Component-Based QML**: Extracted reusable controls with clean interfaces

## 🚀 Quick Start

### Professional Cross-Platform Build System

Modern CMake preset-based build system that works seamlessly across Windows, macOS, and Linux.

#### Prerequisites

**Windows:**
- **MSYS2/MinGW64**: [https://www.msys2.org/](https://www.msys2.org/)
- **Qt 6.9.1**: [Qt Installer](https://www.qt.io/download-qt-installer)
- **CMake & Ninja**: `pacman -S cmake ninja` (in MSYS2)

**macOS:**
- **Xcode Command Line Tools**: `xcode-select --install`  
- **Homebrew**: `/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"`
- **Qt & Build Tools**: `brew install qt cmake ninja`

**Linux:**
- **Build Tools**: `sudo apt install build-essential cmake ninja-build qt6-base-dev`

#### Build Commands

**Configure:**
```bash
# Windows
cmake --preset windows-mingw

# macOS  
cmake --preset mac-clang

# Linux
cmake --preset linux-gcc
```

**Build:**
```bash
# Windows
cmake --build --preset windows-mingw

# macOS
cmake --build --preset mac-clang

# Linux  
cmake --build --preset linux-gcc
```

**One-liner setup:**
```bash
# Clone and build in one go
git clone https://github.com/your-repo/Sentinel.git && cd Sentinel
cmake --preset [windows-mingw|mac-clang|linux-gcc]
cmake --build --preset [windows-mingw|mac-clang|linux-gcc]
```

## 🎛️ Smart Logging System

Professional categorized logging for focused debugging:

```bash
# Load logging modes
source scripts/log-modes.sh

# Common modes
log-production    # Clean logs for demos (5-10 lines)
log-clean         # Daily development (~50 lines)
log-trading       # Debug trade processing (~100 lines)
log-rendering     # Debug visual issues (~150 lines)
log-performance   # Performance metrics (~30 lines)
log-network       # WebSocket connectivity (~40 lines)
log-development   # All categories (200+ lines)
```

## 🗺️ Roadmap

[X] Grid System
[X] Cross-Platform Build System
[ ] Orderbook Heatmap Volume Filter

## 🤝 Contributing

Modern C++ best practices with performance-first architecture:
- **RAII**: Smart pointers and modern ownership semantics
- **Lock-Free Design**: Zero-malloc data pipeline
- **GPU-First**: Direct hardware acceleration via Qt Scene Graph
- **Separation of Concerns**: Clean business logic and rendering layers
- **Cross-Platform**: Professional build system for Windows, macOS, and Linux

## 📝 License

Licensed under GNU Affero General Public License v3.0 (AGPL-3.0).

The AGPL-3.0 ensures all modifications remain open source. Compatible with Qt's LGPL/GPL licensing.

See [LICENSE](LICENSE) for details.

## 🔗 Links

- **[Cross-Platform Setup Guide](docs/CROSS_PLATFORM_SETUP.md)**
- **[System Architecture Details](docs/SYSTEM_ARCHITECTURE.md)**
- **[V2 Rendering Architecture Overview](docs/V2_RENDERING_ARCHITECTURE.md)**
- **[Smart Logging Guide](docs/logging_usage_guide.md)**
- [Coinbase WebSocket API](https://docs.cloud.coinbase.com/exchange/docs/websocket-overview)

---

<p align="center">
  <strong>Built with ⚡ for professional market analysis</strong>
</p>