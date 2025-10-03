# Sentinel: Professional Trading Terminal

> Open-source market analysis platform with GPU-accelerated visualization

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20">
  <img src="https://img.shields.io/badge/Qt-6-green.svg" alt="Qt6">
  <img src="https://img.shields.io/badge/Architecture-GPU_Accelerated-purple" alt="GPU Accelerated">
  <img src="https://img.shields.io/badge/License-AGPL--3.0-blue" alt="License">
  <img src="https://img.shields.io/badge/Platform-Cross_Platform-lightgrey.svg" alt="Cross-Platform">
</p>

<div align="center">

**Real-time crypto market analysis with order book heatmaps and multi-timeframe aggregation**

**[🚀 Quick Start](#quick-start) • [🏗️ Architecture](#architecture) • [📚 Documentation](#documentation)**

</div>

---

## Screenshots

<table>
<tr>
<td width="50%">

**Liquidity Heatmap**
<img width="100%" alt="Order book heatmap" src="https://github.com/user-attachments/assets/fd3a14c2-80e1-47d7-93b4-99d3623ba819" />
<sub>Real-time order book visualization with density-based rendering</sub>

</td>
<td width="50%">

**Trade Flow Analysis**
<img width="100%" alt="Trade flow" src="https://github.com/user-attachments/assets/542d2497-d4e3-43d9-9663-9fbb135423e5" />
<sub>Market microstructure analysis</sub>

</td>
</tr>
</table>

---

## What is Sentinel?

Sentinel is a side project demonstrating modern C++/Qt architecture patterns and GPU-accelerated real-time visualization. It processes live cryptocurrency market data and renders order book heatmaps with multi-timeframe aggregation.

Built as a portfolio piece to showcase:
- Modern C++20 patterns and best practices
- High-performance GPU rendering with Qt Scene Graph
- Clean separation of concerns in a real-time application
- Cross-platform build systems and deployment

---

## Key Features

### Market Visualization
- **Order Book Heatmap**: Dense grid visualization with time-based intensity
- **Multi-Timeframe Aggregation**: 100ms to 10s temporal bucketing
- **Volume-at-Price Analysis**: Real-time depth visualization
- **Anti-Spoofing Detection**: Persistence ratio analysis

### Architecture
- **Modular Rendering**: Strategy pattern for pluggable visualization modes
- **GPU Acceleration**: Direct-to-hardware rendering via Qt Scene Graph
- **Lock-Free Pipelines**: SPSC queues for thread-safe data flow
- **Cross-Platform**: Windows, macOS, Linux with CMake presets

---

## Architecture

```mermaid
graph TD
    A[WebSocket] --> B[MarketDataCore]
    B --> C[DataCache]
    C --> D[LiquidityTimeSeriesEngine]
    D --> E[DataProcessor]
    E --> F[IRenderStrategy]
    F --> G[GPU]
    H[GridViewState] --> E

    subgraph "Core Layer (libs/core)"
    B
    C
    D
    end

    subgraph "GUI Layer (libs/gui)"
    E
    F
    H
    I[UnifiedGridRenderer]
    end
```

### Design Principles
- **libs/core/**: Pure C++ business logic (Qt-independent except QtCore)
- **libs/gui/**: Qt/QML adapters that delegate to core
- **Strategy Pattern**: Pluggable rendering without code changes
- **300 LOC Limit**: Files capped at 300 lines to enforce modularity
- **RAII Everywhere**: Modern C++20 with smart pointers, no manual memory management

For detailed architecture:
- [System Architecture](docs/SYSTEM_ARCHITECTURE.md)
- [V2 Rendering Architecture](docs/V2_RENDERING_ARCHITECTURE.md)
- [Data Flow](docs/DATA_FLOW.md)

---

## Quick Start

### Prerequisites

**macOS:**
```bash
xcode-select --install
brew install qt cmake ninja
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt update
sudo apt install build-essential cmake ninja-build qt6-base-dev \
    qt6-declarative-dev libgl1-mesa-dev libssl-dev
```

**Windows:**
- Install [MSYS2](https://www.msys2.org/)
- Install [Qt 6.5+](https://www.qt.io/download-qt-installer)
- In MSYS2: `pacman -S cmake ninja mingw-w64-x86_64-gcc`

### Build

```bash
git clone https://github.com/pattty847/Sentinel.git
cd Sentinel

# Configure (choose your platform)
cmake --preset mac-clang      # macOS
cmake --preset linux-gcc      # Linux
cmake --preset windows-mingw  # Windows

# Build
cmake --build --preset mac-clang      # macOS
cmake --build --preset linux-gcc      # Linux
cmake --build --preset windows-mingw  # Windows

# Run
./build-mac-clang/apps/sentinel_gui/sentinel_gui
```

### Configuration

Create `key.json` in the project root for Coinbase API access:
```json
{
  "key": "organizations/YOUR_ORG/apiKeys/YOUR_KEY_ID",
  "secret": "-----BEGIN EC PRIVATE KEY-----\nYOUR_PRIVATE_KEY\n-----END EC PRIVATE KEY-----\n"
}
```

### Logging

Control log verbosity with `QT_LOGGING_RULES`:

```bash
# Clean output (production)
export QT_LOGGING_RULES="*.debug=false"

# Debug rendering
export QT_LOGGING_RULES="sentinel.render.debug=true"

# Debug data/network
export QT_LOGGING_RULES="sentinel.data.debug=true"
```

See [Logging Guide](docs/LOGGING_GUIDE.md) for details.

---

## Documentation

### Core Documentation
- [System Architecture](docs/SYSTEM_ARCHITECTURE.md) - Overall design and components
- [V2 Rendering Architecture](docs/V2_RENDERING_ARCHITECTURE.md) - GPU rendering pipeline
- [Data Flow](docs/DATA_FLOW.md) - How data moves through the system
- [Logging Guide](docs/LOGGING_GUIDE.md) - Using the logging system
- [Live Architecture](docs/LIVE_ARCHITECTURE.md) - Current implementation status

### External References
- [Coinbase WebSocket API](https://docs.cloud.coinbase.com/exchange/docs/websocket-overview)
- [Qt Scene Graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)

---

## Future Potential

- Multi-symbol support (ETH, SOL, etc.)
- Custom indicator framework
- Alert system with webhooks
- AI-powered market analysis (experimental/research)

---

## Contributing

Modern C++ best practices:
- **C++20 Standard**: Ranges, concepts, smart pointers
- **RAII**: Automatic resource management
- **Lock-Free Design**: Zero-malloc data pipelines
- **Separation of Concerns**: Clean business logic and rendering layers

Pull requests welcome! Please ensure:
1. Code follows existing style (300 LOC limit per file)
2. Tests pass: `cd build-<platform> && ctest --output-on-failure`
3. Build succeeds on your platform

---

## License

Licensed under **GNU Affero General Public License v3.0 (AGPL-3.0)**.

See [LICENSE](LICENSE) for details.

---

<div align="center">

**Built for professional market analysis**

</div>
