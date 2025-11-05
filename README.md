# Sentinel: GPU-Accelerated Trading Terminal

> High-performance market analysis platform with sub-millisecond rendering  
> This project is licensed under the GNU AGPL v3—open source with strong copyleft.

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20">
  <img src="https://img.shields.io/badge/Qt-6-green.svg" alt="Qt6">
  <img src="https://img.shields.io/badge/Architecture-GPU_Accelerated-purple" alt="GPU Accelerated">
  <img src="https://img.shields.io/badge/Platform-Cross_Platform-lightgrey.svg" alt="Cross-Platform">
  <img src="https://img.shields.io/badge/License-AGPL--3.0-blue" alt="License">
</p>

<div align="center">

**Real-time cryptocurrency market visualization with GPU-accelerated order book heatmaps**

**[🚀 Quick Start](#quick-start) • [⚡ Performance](#performance) • [🏗️ Architecture](#architecture) • [📚 Documentation](#documentation)**

</div>

---

## Screenshots

<table>
<tr>
<td width="50%">

**Liquidity Heatmap**
<img width="2559" height="1385" alt="image" src="https://github.com/user-attachments/assets/27a969f2-1a02-4e69-aee6-ff6b26411779" />

</td>
</tr>
</table>

---

## What is Sentinel?

Sentinel is a high-performance trading terminal built with modern C++20 and Qt 6, demonstrating professional-grade architecture patterns and GPU-accelerated visualization. It processes live cryptocurrency market data from Coinbase Advanced Trade API and renders order book heatmaps with multi-timeframe aggregation at institutional-grade speeds.

Built to showcase:
- **Production-Ready Performance**: Sub-millisecond rendering with GPU acceleration
- **Modern C++20 Patterns**: Concepts, ranges, RAII, and zero-allocation hot paths
- **Clean Architecture**: Strict separation between core business logic and presentation layers
- **Cross-Platform Excellence**: Native GPU backends on Windows (D3D11), macOS (Metal), and Linux (OpenGL)

---

## Performance

### Rendering Metrics

| Metric | Before Optimization | After GPU Acceleration | Improvement |
|--------|---------------------|------------------------|-------------|
| **Paint Time** | ~1,500 ms | 0.7–1.7 ms | **2,000x faster** |
| **Cache Lookup** | ~1,100,000 µs | 20–130 µs | **10,000x faster** |
| **Slice Coverage** | 8–9 time slices | 54+ time slices | **6x more data** |
| **Frame Rate** | Constant stalls | 28–30 Hz stable | **Smooth 30 FPS** |
| **Cell Rendering** | N/A | 6,000+ cells/frame | **Zero stalls** |

### System Efficiency

- **CPU Temperature**: 70°C → 45–55°C (24% reduction)
- **GPU Utilization**: 0% → 15–30% (proper hardware acceleration)
- **Memory**: Lock-free pipelines, zero malloc in hot paths
- **Latency**: Non-blocking async snapshot handoff between threads

### GPU Backend Selection

Platform-optimized rendering backends automatically selected in `main.cpp`:

```cpp
#ifdef Q_OS_WIN
    qputenv("QSG_RHI_BACKEND", "d3d11");    // Windows: Direct3D 11
#elif defined(Q_OS_MACOS)
    qputenv("QSG_RHI_BACKEND", "metal");    // macOS: Metal
#else
    qputenv("QSG_RHI_BACKEND", "opengl");   // Linux: OpenGL
#endif
```

---

## Key Features

### Market Visualization
- **Order Book Heatmap**: Real-time liquidity visualization with temporal intensity decay
- **Multi-Timeframe Aggregation**: 100ms to 10s temporal bucketing for pattern recognition
- **Volume-at-Price Analysis**: Dense price-level depth visualization
- **O(1) Order Book**: 5M+ price level capacity with constant-time lookups

### Technical Architecture
- **GPU-Accelerated Rendering**: Qt Scene Graph with platform-native backends (Metal/D3D11/OpenGL)
- **Append-Only Scene Graph**: Incremental updates, no full rebuilds
- **Lock-Free Data Pipelines**: SPSC queues for zero-contention cross-thread communication
- **Async Snapshot Handoff**: Non-blocking data transfer between processing and render threads
- **Chunked Geometry**: Overcomes ANGLE vertex limits on Windows (fixes rendering artifacts)

### Cross-Platform Support
- **Windows**: Direct3D 11 backend, MSYS2/MinGW build chain
- **macOS**: Metal backend, Clang toolchain
- **Linux**: OpenGL backend, GCC toolchain
- **CMake Presets**: One-command configuration and build per platform

---

## Architecture

Sentinel follows a strict modular architecture with three primary layers:

### Core Layer (`libs/core`)
Pure C++20 business logic with **zero Qt dependencies** (QtCore only for types):
- **Market Data Pipeline**: WebSocket transport, message dispatch, authentication, caching
- **Order Book Engine**: O(1) lookup with 5M price level capacity
- **Time Series Engine**: Multi-timeframe aggregation (100ms to 10s)
- **Performance Monitor**: Unified latency and throughput tracking

### GUI Layer (`libs/gui`)
Qt-based adapters and rendering strategies:
- **UnifiedGridRenderer**: Facade for GPU-accelerated scene graph rendering
- **DataProcessor**: Snapshot publisher on worker thread, async handoff to renderer
- **Render Strategies**: Pluggable visualization modes (Heatmap, Candles, Trade Flow)
- **QML Integration**: Declarative UI with C++ backend bindings

### App Layer (`apps/`)
Minimal bootstrap entry points:
- **sentinel_gui**: Main trading terminal application
- **stream_cli**: Headless market data streaming utility

### Design Principles

- **Separation of Concerns**: Core has no GUI dependencies; GUI contains no business logic
- **Strategy Pattern**: Pluggable rendering strategies for extensibility
- **RAII + Smart Pointers**: Automatic resource management, no manual memory handling
- **500 LOC Limit**: Enforced file size limit prevents monolithic classes

For detailed component diagrams and data flow, see **[Architecture Document](docs/ARCHITECTURE.md)**.

---

## Quick Start

### Prerequisites

<details>
<summary><strong>macOS</strong></summary>

```bash
xcode-select --install
brew install qt cmake ninja
```
</details>

<details>
<summary><strong>Linux (Ubuntu/Debian)</strong></summary>

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build qt6-base-dev \
    qt6-declarative-dev libgl1-mesa-dev libssl-dev
```
</details>

<details>
<summary><strong>Windows</strong></summary>

- Install [MSYS2](https://www.msys2.org/)
- Install [Qt 6.5+](https://www.qt.io/download-qt-installer)
- In MSYS2 terminal:
```bash
pacman -S cmake ninja mingw-w64-x86_64-gcc
```
</details>

### Build & Run

```bash
# Clone repository
git clone https://github.com/pattty847/Sentinel.git
cd Sentinel

# Configure (choose your platform preset)
cmake --preset mac-clang      # macOS
cmake --preset linux-gcc      # Linux
cmake --preset windows-mingw  # Windows

# Build with all CPU cores
cmake --build --preset mac-clang -j$(sysctl -n hw.ncpu)      # macOS
cmake --build --preset linux-gcc -j$(nproc)                  # Linux
cmake --build --preset windows-mingw -j$(nproc)              # Windows

# Run tests
cd build-mac-clang && ctest --output-on-failure

# Launch GUI
./build-mac-clang/apps/sentinel_gui/sentinel_gui       # macOS/Linux
./build-windows-mingw/apps/sentinel_gui/sentinel_gui   # Windows
```

### Configuration

Create `key.json` in the project root with your Coinbase Advanced Trade API credentials:

```json
{
  "key": "organizations/YOUR_ORG/apiKeys/YOUR_KEY_ID",
  "secret": "-----BEGIN EC PRIVATE KEY-----\nYOUR_PRIVATE_KEY\n-----END EC PRIVATE KEY-----\n"
}
```

> **Note**: Get API keys from [Coinbase Cloud](https://cloud.coinbase.com/)

### Logging

Control log verbosity using Qt's categorized logging system. See **[Logging Guide](docs/LOGGING_GUIDE.md)** for details.

```bash
# Production: minimal output
export QT_LOGGING_RULES="*.debug=false"

# Debug rendering performance
export QT_LOGGING_RULES="sentinel.render=true;sentinel.render.debug=true"

# Debug data processing
export QT_LOGGING_RULES="sentinel.data=true;sentinel.data.debug=true"
```

---

## Documentation

- **[Architecture](docs/ARCHITECTURE.md)**: System design, components, and data flow
- **[Logging Guide](docs/LOGGING_GUIDE.md)**: Categorized logging system usage
- **[Code Analysis Tools](scripts/README_CODE_ANALYSIS.md)**: Function extraction and overview utilities
- **[Coinbase WebSocket API](https://docs.cloud.coinbase.com/exchange/docs/websocket-overview)**: Official API reference
- **[Qt Scene Graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)**: GPU rendering technology

---

## Contributing

We follow modern C++20 best practices and enforce strict quality standards:

### Code Standards
- **C++20 Standard**: Use concepts, ranges, structured bindings, and smart pointers
- **RAII Everywhere**: No manual resource management
- **Lock-Free Hot Paths**: Zero malloc/contention in rendering and data processing loops
- **500 LOC Limit**: Files over 450 lines require justification; 500+ triggers mandatory refactor
- **Separation of Concerns**: Business logic in `core`, GUI adapters in `gui`, apps are thin shells

### Contribution Workflow

1. **Fork & Branch**: Create a feature branch from `main`
2. **Code**: Follow existing patterns and file organization
3. **Test**: Ensure all tests pass
   ```bash
   cd build-<platform>
   ctest --output-on-failure
   ```
4. **Build**: Verify clean builds on your platform
5. **PR**: Submit with clear description of changes and performance impact (if applicable)

### Recent Achievements

Major refactors that improved performance by orders of magnitude:

- ✅ **GPU Acceleration**: Append-only scene graph rendering (2,000x paint speedup)
- ✅ **Async Snapshots**: Non-blocking handoff between data processor and renderer
- ✅ **Chunked Geometry**: Fixed Windows ANGLE vertex limit (eliminated rendering artifacts)
- ✅ **Platform Backends**: Native D3D11/Metal/OpenGL selection per OS
- ✅ **Lifecycle Management**: Graceful shutdown, thread cleanup, no QThread warnings

---

## License

Licensed under **GNU Affero General Public License v3.0 (AGPL-3.0)**.

This ensures that any network service built with Sentinel must also be open-sourced.

See [LICENSE](LICENSE) for full terms.

---

<div align="center">

**Built for institutional-grade market analysis**

*Sub-millisecond rendering • Lock-free pipelines • GPU acceleration*

</div>
