# Sentinel - Cross-Platform High-Performance Trading System

[![Cross-Platform CI](https://github.com/your-org/sentinel/actions/workflows/cross-platform-ci.yml/badge.svg)](https://github.com/your-org/sentinel/actions/workflows/cross-platform-ci.yml)
[![Windows](https://img.shields.io/badge/Windows-10%2F11-blue?logo=windows)](docs/CROSS_PLATFORM_BUILD.md#windows-detailed-setup)
[![macOS](https://img.shields.io/badge/macOS-10.15%2B-blue?logo=apple)](docs/CROSS_PLATFORM_BUILD.md#macos-detailed-setup)
[![Linux](https://img.shields.io/badge/Linux-Ubuntu%2FFedora%2FArch-blue?logo=linux)](docs/CROSS_PLATFORM_BUILD.md#linux-detailed-setup)

> **Real-time cryptocurrency market data streaming and analysis platform with sub-millisecond latency performance**

Sentinel is a high-performance, cross-platform trading system built with modern C++17, designed to run identically on Windows, macOS, and Linux. Featuring a proven 83% code reduction architecture with 0.0003ms average latency [[memory:876671]].

## 🌍 Cross-Platform Compatibility

- ✅ **Windows 10/11** - MSVC 2019+, MinGW support
- ✅ **macOS 10.15+** - Intel & Apple Silicon native
- ✅ **Linux** - Ubuntu, Fedora, Arch, and more

## 🚀 Quick Start (Any Platform)

### Prerequisites
- CMake 3.16+
- C++17 compiler
- vcpkg package manager

### One-Line Setup
```bash
# Clone and build on any platform
git clone <repo-url> && cd Sentinel && cmake --preset default && cmake --build build
```

### Platform-Specific Quick Start

#### Windows (PowerShell)
```powershell
# Install vcpkg
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"

# Build Sentinel
cmake --preset windows-msvc
cmake --build build-windows --config Release
```

#### macOS (Terminal)
```bash
# Install vcpkg
git clone https://github.com/Microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$HOME/vcpkg"

# Build Sentinel
cmake --preset macos-clang
cmake --build build-macos --config Release
```

#### Linux (Terminal)
```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install build-essential cmake ninja-build

# Install vcpkg
git clone https://github.com/Microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$HOME/vcpkg"

# Build Sentinel
cmake --preset linux-gcc
cmake --build build-linux --config Release
```

## 🏗️ Architecture Highlights

### Proven Performance [[memory:876671]]
- **40ns latency** in FastOrderBook optimization
- **20-30M operations/second** throughput
- **HFT-grade capability** with institutional-grade scaling
- **Sub-millisecond** end-to-end processing

### Cross-Platform Design
- **Single Codebase** - No platform-specific branches
- **Standard C++17** - Modern, portable code
- **CMake Build System** - Universal build configuration
- **vcpkg Dependencies** - Consistent package management

### Component Architecture
```
┌─────────────────┬─────────────────┬─────────────────┐
│     Windows     │      macOS      │      Linux      │
├─────────────────┼─────────────────┼─────────────────┤
│   Qt6 GUI       │    Qt6 GUI      │    Qt6 GUI      │
├─────────────────┼─────────────────┼─────────────────┤
│ Boost.Beast WS  │ Boost.Beast WS  │ Boost.Beast WS  │
├─────────────────┼─────────────────┼─────────────────┤
│ OpenSSL/Crypto  │ OpenSSL/Crypto  │ OpenSSL/Crypto  │
├─────────────────┼─────────────────┼─────────────────┤
│  JWT-CPP Auth   │  JWT-CPP Auth   │  JWT-CPP Auth   │
└─────────────────┴─────────────────┴─────────────────┘
```

## 🔧 Features

### Real-Time Market Data
- **WebSocket Streaming** - Coinbase Advanced Trade API
- **Multi-Product Support** - BTC-USD, ETH-USD, and more
- **Level 2 Order Book** - Full depth market data
- **Trade Stream** - Real-time execution data

### High-Performance Processing
- **Ring Buffer Cache** - O(1) concurrent data access
- **Lock-Free Architecture** - Minimal contention design
- **Worker Thread Model** - Separate networking and GUI threads
- **Memory Efficient** - Bounded memory usage patterns

### Modern GUI Rendering
- **GPU-Accelerated Charts** - OpenGL-based visualization
- **Trade Scatter Plots** - Real-time price action
- **Order Book Heatmaps** - Depth visualization
- **Multi-Timeframe Candles** - OHLC chart support

## 📦 Applications

### GUI Application (`sentinel_gui`)
Full-featured desktop application with real-time charts and market data visualization.

### CLI Streaming Client (`stream_cli`)
Command-line tool for high-speed data streaming and analysis.

## 🧪 Testing & Quality

### Comprehensive Test Suite
- **12+ Test Cases** - Full functionality coverage
- **Performance Benchmarks** - Latency and throughput validation
- **Integration Tests** - End-to-end data flow verification
- **Cross-Platform CI** - Automated testing on all platforms

### Code Quality
- **Modern C++17** - Type safety and performance
- **RAII Patterns** - Automatic resource management
- **Thread Safety** - Proven concurrent design
- **Static Analysis** - Automated code quality checks

## 📖 Documentation

- **[Cross-Platform Build Guide](docs/CROSS_PLATFORM_BUILD.md)** - Detailed setup for all platforms
- **[Architecture Overview](docs/02_ARCHITECTURE.md)** - System design and patterns
- **[Development Workflows](docs/04_WORKFLOWS.md)** - Contribution guidelines

## 🚀 Performance Metrics [[memory:876671]]

| Component | Metric | Value |
|-----------|--------|-------|
| FastOrderBook | Latency | 40ns |
| FastOrderBook | Throughput | 20-30M ops/sec |
| Processing | End-to-End | <1ms |
| WebSocket | Connection | Sub-second |
| GUI Updates | Frequency | 60 FPS |

## 🛠️ Development

### IDE Support
- **Visual Studio** (Windows) - Native CMake support
- **Visual Studio Code** - Cross-platform with C++ extensions
- **CLion** - Full CMake integration
- **Xcode** (macOS) - CMake project support

### Build Presets
```bash
# Platform-optimized builds
cmake --preset windows-msvc    # Windows Visual Studio
cmake --preset macos-clang     # macOS Clang
cmake --preset linux-gcc       # Linux GCC
cmake --preset debug           # Debug build (any platform)
```

## 🤝 Contributing

1. **Fork** the repository
2. **Create** a feature branch
3. **Test** on your target platform(s)
4. **Submit** a pull request

All contributions are automatically tested across Windows, macOS, and Linux.

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🔗 Links

- **GitHub Issues** - Bug reports and feature requests
- **Discussions** - Community support and ideas
- **Releases** - Pre-built binaries for all platforms

---

**Built with ❤️ using modern C++17 and designed to run everywhere**
