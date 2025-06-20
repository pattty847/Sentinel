# Sentinel C++: High-Performance Market Microstructure Analysis

🚀 **Real-time cryptocurrency market data analysis with professional-grade visualizations**

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg" alt="C++17">
  <img src="https://img.shields.io/badge/Qt-6-green.svg" alt="Qt6">
  <img src="https://img.shields.io/badge/Status-Bridge%20Complete-brightgreen.svg" alt="Status">
  <img src="https://img.shields.io/badge/Performance-Sub%20100ms-orange.svg" alt="Performance">
</p>

## 🎯 Project Vision

Sentinel aims to be a **professional-grade, high-performance market microstructure analysis tool** for cryptocurrency markets, starting with BTC-USD and ETH-USD on Coinbase. The vision extends beyond a simple desktop application to a robust, 24/7 analysis engine with **rich visual analytics** inspired by tools like BookMap and aterm.

## ⚡ Current Status: **BRIDGE COMPLETE** ✅

### 🔥 **Phase 6 Achievement: StreamController Bridge**

We've successfully integrated our high-performance C++ streaming engine with Qt's GUI framework using a **bridge pattern** architecture:

- **✅ Real-time streaming** at sub-100ms latency
- **✅ Trade deduplication** using Coinbase trade IDs  
- **✅ Multi-threaded architecture** with responsive GUI
- **✅ CVD calculation** updating live
- **✅ Professional error handling** and clean shutdown

![Sentinel GUI](https://via.placeholder.com/600x400/1e1e1e/00ff00?text=Real-time+Trade+Stream+%E2%9A%A1)

## 🏗️ Architecture Overview

```
┌─────────────────────┐    ┌─────────────────────┐
│   MainWindow (Qt)   │    │  StreamController   │
│   ┌─────────────┐   │    │  ┌──────────────┐   │
│   │ Trade Log   │◄──┼────┼──┤ Qt Signals   │   │
│   │ CVD Display │   │    │  └──────────────┤   │
│   │ Alerts      │   │    │  ┌──────────────┤   │
│   └─────────────┘   │    │  │ C++ Bridge   │   │
└─────────────────────┘    │  └──────────────┤   │
                           │  ┌──────────────▼┐  │
┌─────────────────────┐    │  │CoinbaseStream │  │
│  StatisticsProcessor│◄───┼──┤    Client     │  │
│  ┌─────────────┐    │    │  └───────────────┘  │
│  │ CVD Engine  │    │    └─────────────────────┘
│  │ Rules Engine│    │
│  └─────────────┘    │    ┌─────────────────────┐
└─────────────────────┘    │   WebSocket Stream  │
                           │  ┌───────────────┐  │
                           │  │ JSON Parser   │  │
                           │  │ Trade Buffer  │  │
                           │  │ Deduplication │  │
                           │  └───────────────┘  │
                           └─────────────────────┘
```

### 🧩 Key Components

- **`CoinbaseStreamClient`**: High-performance WebSocket client with trade deduplication
- **`StreamController`**: Qt bridge that converts C++ data to Qt signals
- **`StatisticsProcessor`**: Pure C++ CVD calculation engine
- **`MainWindow`**: Qt GUI with real-time updates
- **`RuleEngine`**: Alert system for threshold monitoring

## 🚀 Quick Start

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install cmake build-essential qt6-base-dev qt6-websockets-dev
sudo apt install libssl-dev libboost-dev nlohmann-json3-dev

# Or use vcpkg for dependencies
```

### Build & Run

```bash
# Clone the repository
git clone <repo-url>
cd Sentinel

# Build the project
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run the GUI application
./sentinel

# Or run the console test
./coinbase_stream_test
```

## 📊 Features

### ✅ **Currently Working**

- **High-Frequency Streaming**: Sub-100ms trade data processing
- **Trade Deduplication**: Coinbase trade_id based duplicate prevention
- **Real-time CVD**: Cumulative Volume Delta calculation and display
- **Multi-Symbol Support**: BTC-USD and ETH-USD simultaneous streaming
- **Alert System**: Configurable threshold-based notifications
- **Thread-Safe Architecture**: Responsive GUI with background processing
- **Robust Error Handling**: Connection recovery and graceful shutdown

### 🚧 **Coming Next: Advanced Visualizations**

- **Phase 7**: Custom Qt Charts - Tick-by-tick trade plotting
- **Phase 8**: Order Book Heatmaps - Professional liquidity visualization
- **Phase 9**: Multi-Timeframe Analysis - Zoom and aggregation controls
- **Phase 10**: Performance Optimization - Efficient rendering pipeline

## 📈 Performance Metrics

- **Latency**: < 100ms trade processing
- **Throughput**: Handles full Coinbase market data stream
- **Memory**: Efficient circular buffers with configurable limits
- **CPU**: Multi-threaded design for optimal resource utilization

## 🛠️ Development

### Project Structure

```
Sentinel/
├── src/
│   ├── main.cpp                  # GUI application entry
│   ├── stream_main.cpp           # Console test entry
│   ├── coinbasestreamclient.{h,cpp}  # Core streaming engine
│   ├── streamcontroller.{h,cpp}      # Qt bridge
│   ├── mainwindow.{h,cpp}           # GUI interface
│   ├── statisticsprocessor.{h,cpp}   # CVD calculation
│   ├── ruleengine.{h,cpp}           # Alert system
│   └── tradedata.h                  # Data structures
├── docs/
│   ├── ARCHITECTURE.md          # Detailed architecture guide
│   └── PROJECT_PLAN.md          # Development roadmap
├── CMakeLists.txt               # Build configuration
└── README.md                    # This file
```

### Build System

- **CMake**: Cross-platform build configuration
- **Qt6**: Modern GUI framework with signal/slot system
- **C++17**: Modern C++ features for performance and safety
- **WebSocket++**: High-performance WebSocket client library

## 🎨 Visualization Goals

Our target is to create **professional-grade visualizations** similar to:

- **Order Book Heatmaps**: Real-time liquidity density visualization
- **Tick-by-Tick Charts**: Microsecond precision trade plotting  
- **Volume Analysis**: Aggressive vs. passive flow detection
- **Multi-Timeframe Views**: Seamless zoom from tick to minute granularity

## 🤝 Contributing

This project follows modern C++ best practices:

- **RAII**: Resource management through smart pointers
- **Thread Safety**: Qt's signal/slot system for cross-thread communication
- **Separation of Concerns**: Pure C++ logic + Qt GUI wrapper
- **Performance First**: Optimized for high-frequency data processing

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
