# Sentinel C++: High-Performance Market Microstructure Analysis

🚀 **Real-time cryptocurrency market data analysis with professional-grade visualizations**

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg" alt="C++17">
  <img src="https://img.shields.io/badge/Qt-6-green.svg" alt="Qt6">
  <img src="https://img.shields.io/badge/dependencies-vcpkg-blue.svg" alt="vcpkg">
  <img src="https://img.shields.io/badge/networking-Boost.Beast-orange.svg" alt="Boost.Beast">
  <img src="https://img.shields.io/badge/Status-Refactor%20Complete-brightgreen.svg" alt="Status">
</p>

<div align="center">
  <img src="https://github.com/user-attachments/assets/f8691b82-a576-4888-aa94-4b096abc5360" alt="Sentinel GUI" width="600"/>
</div>

## 🎯 Project Vision

Sentinel aims to be a **professional-grade, high-performance market microstructure analysis tool** for cryptocurrency markets, starting with BTC-USD and ETH-USD on Coinbase. The vision extends beyond a simple desktop application to a robust, 24/7 analysis engine with **rich visual analytics** inspired by tools like BookMap and aterm.

## ⚡ Current Status: **Modernization Complete** ✅

We have successfully refactored the entire project to use a modern C++ toolchain, enhancing stability, portability, and developer experience.

- **✅ Modern Dependency Management** with `vcpkg`.
- **✅ High-Performance Networking** with `Boost.Beast`.
- **✅ Reproducible Builds** using `CMakePresets`.
- **✅ Decoupled Architecture** with `libs/` and `apps/` structure.
- **✅ Real-time CVD calculation** and live charting.

## 🏗️ Architecture Overview

The project is built on a modular, multi-threaded architecture that separates the core logic from the user interface, enabling portability and high performance.

- **`apps/`**: Contains the executables.
    - `sentinel_gui`: The main Qt-based desktop application.
    - `stream_cli`: A headless command-line tool for the streaming client.
- **`libs/`**: Contains the core functionality, compiled into reusable libraries.
    - `sentinel_core`: Pure C++ logic for networking (`Boost.Beast`), data processing, and analysis.
    - `sentinel_gui_lib`: Qt-specific components for the GUI (`MainWindow`, `TradeChartWidget`).
- **`vcpkg.json`**: The manifest file declaring all C++ dependencies.

## 🚀 Quick Start

### Prerequisites

1.  **C++ Compiler**: Xcode Command Line Tools on macOS, or MSVC on Windows.
2.  **Qt 6**: Must be installed and available in your system's `PATH`.
3.  **vcpkg**: 
    ```bash
    git clone https://github.com/microsoft/vcpkg.git
    ./vcpkg/bootstrap-vcpkg.sh
    # Set VCPKG_ROOT environment variable
    export VCPKG_ROOT=$(pwd)/vcpkg
    ```

### Build & Run

The project uses CMake Presets for simple, one-command builds.

```bash
# Clone the repository
git clone <repo-url>
cd Sentinel

# Configure the project (vcpkg will auto-install dependencies)
cmake --preset=default

# Build the project
cmake --build build

# Run the GUI application
./build/apps/sentinel_gui/sentinel
```

## 📊 Features

### ✅ **Currently Working**

- **High-Frequency Streaming**: Sub-100ms trade data processing via Boost.Beast.
- **Modern Dependency Management**: All C++ libraries managed by vcpkg.
- **Real-time CVD**: Cumulative Volume Delta calculation and display.
- **Multi-Symbol Support**: BTC-USD and ETH-USD simultaneous streaming.
- **Thread-Safe Architecture**: Responsive GUI with background networking.
- **Live Charting**: Custom-drawn, real-time trade and price visualization.

### 🚧 **Coming Next: Advanced Visualizations**

- **Phase 8**: Order Book Heatmaps - Professional liquidity visualization.
- **Phase 9**: Multi-Timeframe Analysis - Zoom and aggregation controls.
- **Phase 10**: Performance Optimization - Further rendering pipeline enhancements.


## 🛠️ Development

### Build System

- **CMake**: Cross-platform build configuration.
- **vcpkg**: C++ library management.
- **Qt6**: Modern GUI framework with signal/slot system.
- **Boost.Beast**: High-performance, asynchronous networking.
- **C++17**: Modern C++ features for performance and safety.

## 🤝 Contributing

This project follows modern C++ best practices:
- **RAII**: Resource management through smart pointers and modern ownership semantics.
- **Thread Safety**: Qt's signal/slot system for cross-thread communication.
- **Separation of Concerns**: Pure C++ logic (`sentinel_core`) completely decoupled from the Qt GUI (`sentinel_gui_lib`).
- **Performance First**: Asynchronous networking and optimized data handling.

## 📝 License

[Add your license here]

## 🔗 Links

- [Architecture Documentation](docs/ARCHITECTURE.md)
- [Project Roadmap](docs/PROJECT_PLAN.md)
- [Coinbase WebSocket API](https://docs.cloud.coinbase.com/exchange/docs/websocket-overview)

---

<p align="center">
  <strong>Built with ⚡ for high-frequency market analysis</strong>
</p>
