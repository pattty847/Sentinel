# Sentinel: Ultra-High-Performance GPU Financial Charting

🚀 **Real-time cryptocurrency market analysis with direct-to-GPU visualization capable of 52 MILLION operations/second.**

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg" alt="C++17">
  <img src="https://img.shields.io/badge/Qt-6-green.svg" alt="Qt6">
  <img src="https://img.shields.io/badge/Renderer-Direct_to_GPU-purple.svg" alt="Direct-to-GPU">
  <img src="https://img.shields.io/badge/FastOrderBook-52M_ops/sec-red.svg" alt="FastOrderBook Performance">
  <img src="https://img.shields.io/badge/Latency-19.2ns-orange.svg" alt="Latency">
  <img src="https://img.shields.io/badge/Status-Production_Ready-brightgreen.svg" alt="Status">
</p>

<div align="center">
  Legacy Heatmap: 
  <img width="1101" height="801" alt="Screenshot 2025-07-18 at 1 22 08 AM" src="https://github.com/user-attachments/assets/542d2497-d4e3-43d9-9663-9fbb135423e5" />

  2D-Grid Heatmap:
  <img width="1400" height="927" alt="Screenshot 2025-07-24 at 10 52 36 AM" src="https://github.com/user-attachments/assets/fd3a14c2-80e1-47d7-93b4-99d3623ba819" />
</div>

## ✨ Key Features

### 🎯 Grid-Based Market Microstructure Analysis
- **Professional Liquidity Heatmap**: Dense grid orderbook visualization with 2D coordinate aggregation
- **Multi-Timeframe Analysis**: 100ms → 10s temporal aggregation for professional market depth analysis
- **Anti-Spoofing Detection**: Persistence ratio analysis to filter out fake liquidity
- **Volume-at-Price Analysis**: Real-time depth visualization with GPU acceleration

## 🚀 Quick Start

### Prerequisites
- **C++ Compiler**: Xcode Command Line Tools (macOS) or MSVC (Windows)
- **Qt 6**: Must be in system PATH
- **vcpkg**: Package manager for dependencies

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)/vcpkg
```

### Build & Run
```bash
# Clone and build
git clone <repo-url>
cd Sentinel

# Configure with vcpkg auto-install
cmake --preset=default

# Build project
cmake --build build

# Run application
./build/apps/sentinel_gui/sentinel
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
[ ] Orderbook Heatmap Volume Filter

## 🤝 Contributing

Modern C++ best practices with performance-first architecture:
- **RAII**: Smart pointers and modern ownership semantics
- **Lock-Free Design**: Zero-malloc data pipeline
- **GPU-First**: Direct hardware acceleration via Qt Scene Graph
- **Separation of Concerns**: Clean business logic and rendering layers

## 📝 License

Licensed under GNU Affero General Public License v3.0 (AGPL-3.0).

The AGPL-3.0 ensures all modifications remain open source. Compatible with Qt's LGPL/GPL licensing.

See [LICENSE](LICENSE) for details.

## 🔗 Links

- **[GPU Architecture Details](docs/02_ARCHITECTURE.md)**
- **[Grid Architecture Overview](docs/GRID_ARCHITECTURE.md)**
- **[Smart Logging Guide](docs/logging_usage_guide.md)**
- [Coinbase WebSocket API](https://docs.cloud.coinbase.com/exchange/docs/websocket-overview)

---

<p align="center">
  <strong>Built with ⚡ for professional market analysis</strong>
</p>