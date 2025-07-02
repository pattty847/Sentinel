# Cross-Platform Build Guide

This guide covers building Sentinel on Windows, Linux, and macOS. The project is designed to be fully cross-platform compatible.

## 🌍 Platform Support

- ✅ **Windows 10/11** (MSVC 2019+ or MinGW)
- ✅ **macOS 10.15+** (Intel & Apple Silicon)
- ✅ **Linux** (Ubuntu, Fedora, Arch, etc.)

## 📋 Prerequisites

All platforms require:
- **CMake 3.16+**
- **C++17 compatible compiler**
- **vcpkg package manager**
- **Qt6** (will be installed via vcpkg)

## 🚀 Quick Start

### 1. Install vcpkg

Choose your platform:

#### Windows (PowerShell)
```powershell
# Clone vcpkg
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg

# Bootstrap vcpkg
.\bootstrap-vcpkg.bat

# Set environment variable (add to your PATH permanently)
$env:VCPKG_ROOT = "C:\vcpkg"
```

#### macOS (Terminal)
```bash
# Clone vcpkg
git clone https://github.com/Microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg

# Bootstrap vcpkg
./bootstrap-vcpkg.sh

# Add to your shell profile (.zshrc, .bash_profile)
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.zshrc
source ~/.zshrc
```

#### Linux (Terminal)
```bash
# Clone vcpkg
git clone https://github.com/Microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg

# Bootstrap vcpkg
./bootstrap-vcpkg.sh

# Add to your shell profile (.bashrc, .zshrc)
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.bashrc
source ~/.bashrc
```

### 2. Install Dependencies

Sentinel's dependencies will be automatically installed via vcpkg during the build process:
- `boost-beast` - WebSocket networking
- `nlohmann-json` - JSON parsing
- `openssl` - SSL/TLS support
- `jwt-cpp` - JWT authentication
- `qt6` - GUI framework

### 3. Build Sentinel

#### Option A: Using CMake Presets (Recommended)

```bash
# Clone Sentinel
git clone <sentinel-repo-url>
cd Sentinel

# Configure (auto-detects your platform)
cmake --preset default

# Build
cmake --build build --config Release

# Run tests
ctest --preset default
```

#### Option B: Manual CMake Configuration

```bash
# Configure
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . --config Release
```

## 🔧 Platform-Specific Instructions

### Windows Detailed Setup

#### Using Visual Studio (Recommended)
1. **Install Visual Studio 2019/2022** with C++ Desktop Development workload
2. **Install CMake** (included with VS or standalone)
3. **Install Git** for Windows
4. **Setup vcpkg** as shown above
5. **Build using preset:**
   ```powershell
   cmake --preset windows-msvc
   cmake --build build-windows --config Release
   ```

#### Using MinGW
1. **Install MSYS2** from https://www.msys2.org/
2. **Install tools:**
   ```bash
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
   ```
3. **Build normally** using the default preset

### macOS Detailed Setup

#### Prerequisites
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install Homebrew (optional, for convenience)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install CMake and Ninja via Homebrew (optional)
brew install cmake ninja
```

#### Build Commands
```bash
# Use macOS-specific preset
cmake --preset macos-clang
cmake --build build-macos --config Release
```

### Linux Detailed Setup

#### Ubuntu/Debian
```bash
# Install build tools
sudo apt update
sudo apt install build-essential cmake ninja-build git curl zip unzip tar

# Clone and build
git clone <sentinel-repo-url>
cd Sentinel
cmake --preset linux-gcc
cmake --build build-linux --config Release
```

#### Fedora/RHEL
```bash
# Install build tools
sudo dnf install gcc-c++ cmake ninja-build git curl zip unzip tar

# Build as above
```

#### Arch Linux
```bash
# Install build tools
sudo pacman -S base-devel cmake ninja git

# Build as above
```

## 🏃‍♂️ Running Sentinel

### GUI Application
```bash
# Windows
./build/apps/sentinel_gui/sentinel_gui.exe

# macOS/Linux
./build/apps/sentinel_gui/sentinel_gui
```

### CLI Streaming Client
```bash
# Windows
./build/apps/stream_cli/stream_cli.exe

# macOS/Linux  
./build/apps/stream_cli/stream_cli
```

## 🧪 Testing

Run the comprehensive test suite:
```bash
# All platforms
cd build
ctest --output-on-failure

# Or specific tests
./tests/test_fast_orderbook
./tests/test_coinbase_firehose
```

## 🛠 Development Setup

### IDE Configuration

#### Visual Studio Code
1. Install C++ extension pack
2. Open the Sentinel folder
3. Configure `.vscode/settings.json`:
```json
{
    "cmake.configurePreset": "default",
    "cmake.buildPreset": "default",
    "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools"
}
```

#### CLion
1. Open the project folder
2. CLion will automatically detect CMake configuration
3. Select appropriate preset from CMake settings

#### Visual Studio (Windows)
1. Open as CMake project: File → Open → CMake
2. Select the Sentinel folder
3. Choose build configuration from the dropdown

## 🐛 Troubleshooting

### Common Issues

#### vcpkg not found
```
Error: Vcpkg toolchain file not found
```
**Solution:** Set `VCPKG_ROOT` environment variable or specify manually:
```bash
cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake ..
```

#### Qt6 not found
```
Error: Could not find a package configuration file provided by "Qt6"
```
**Solution:** Install Qt6 via vcpkg:
```bash
vcpkg install qt6-base qt6-charts qt6-quick
```

#### Missing OpenSSL (Linux)
```
Error: Could not find OpenSSL
```
**Solution:** Install system OpenSSL development packages:
```bash
# Ubuntu/Debian
sudo apt install libssl-dev

# Fedora
sudo dnf install openssl-devel
```

### Performance Tips
- **Use Ninja generator** for faster builds: `-G Ninja`
- **Enable parallel compilation** (automatically enabled in our presets)
- **Use Release builds** for production deployment

## 📦 Packaging

### Windows
- Create installer using NSIS or WiX
- Package dependencies with the executable

### macOS
- Create .app bundle with macdeployqt
- Sign and notarize for distribution

### Linux
- Create AppImage for universal compatibility
- Generate .deb/.rpm packages for specific distributions

## 🔄 Continuous Integration

Example GitHub Actions workflow for cross-platform CI:

```yaml
name: Cross-Platform Build
on: [push, pull_request]

jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-latest, windows-latest, macos-latest]
    
    runs-on: ${{ matrix.os }}
    
    steps:
    - uses: actions/checkout@v3
    - name: Setup vcpkg
      uses: lukka/run-vcpkg@v10
    - name: Configure CMake
      run: cmake --preset default
    - name: Build
      run: cmake --build build --config Release
    - name: Test
      run: ctest --preset default
```

## 📚 Additional Resources

- [CMake Documentation](https://cmake.org/documentation/)
- [vcpkg Documentation](https://vcpkg.io/en/docs/README.html)
- [Qt6 Cross-Platform Development](https://doc.qt.io/qt-6/)
- [Boost.Beast Documentation](https://www.boost.org/doc/libs/1_80_0/libs/beast/doc/html/index.html) 