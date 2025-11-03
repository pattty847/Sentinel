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
<img width="2559" height="1385" alt="image" src="https://github.com/user-attachments/assets/27a969f2-1a02-4e69-aee6-ff6b26411779" />

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

The system is designed for performance and modularity, with a clean separation between the C++ business logic (`libs/core`) and the Qt-based GUI (`libs/gui`). It uses a multi-threaded data pipeline to process real-time WebSocket data and render it efficiently on the GPU via the Qt Scene Graph.

Key design principles include:
- **Strict Separation of Concerns**: `core` has no GUI dependencies, `gui` contains no business logic.
- **Strategy Pattern**: Pluggable rendering strategies allow for adding new visualizations without altering the core pipeline.
- **Modern C++20**: Enforces memory safety and clean code with RAII, smart pointers, and concepts.
- **Modularity**: A 300-line-of-code limit per file prevents monolithic classes.

For a complete overview of the components and data flow, see the main **[Architecture Document](docs/ARCHITECTURE.md)**.

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

Control log verbosity with `QT_LOGGING_RULES`. See the **[Logging Guide](docs/LOGGING_GUIDE.md)** for detailed instructions.

```bash
# Example: Clean output for production use
export QT_LOGGING_RULES="*.debug=false"

# Example: Debug rendering issues
export QT_LOGGING_RULES="sentinel.render.debug=true"
```

---

## Documentation

Project documentation is kept concise and focused.

- **[Architecture](docs/ARCHITECTURE.md)**: A detailed explanation of the system's design, components, and data flow.
- **[Logging Guide](docs/LOGGING_GUIDE.md)**: Instructions for using the categorized logging system for debugging.
- **[Coinbase WebSocket API](https://docs.cloud.coinbase.com/exchange/docs/websocket-overview)**: Official API documentation.
- **[Qt Scene Graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)**: The underlying technology for GPU-accelerated rendering.

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
