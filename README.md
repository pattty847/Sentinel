# **Sentinel: GPU-Accelerated Trading Terminal**

*Sub-millisecond market visualization built with C++20 and Qt 6.*

<div align="center">
  <img src="https://img.shields.io/badge/C++-20-blue" />
  <img src="https://img.shields.io/badge/Qt-6-green" />
  <img src="https://img.shields.io/badge/GPU-Accelerated-purple" />
  <img src="https://img.shields.io/badge/Platform-Cross--Platform-lightgrey" />
  <img src="https://img.shields.io/badge/License-AGPL--3.0-blue" />
</div>

---

# 📸 Screenshots

| Dockable Layout *(WIP)*                                                                                  | Liquidity Heatmap *(Main Branch)*                                                                        |
| -------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| <img src="https://github.com/user-attachments/assets/9d3ac4b5-eedb-44d3-855c-b03a7d6ac66b" width="600"/> | <img src="https://github.com/user-attachments/assets/27a969f2-1a02-4e69-aee6-ff6b26411779" width="600"/> |

---

# 🧭 Why Sentinel Exists

Order-book visualizers either run in browsers, lag under load, or choke when rendering dense liquidity.
Sentinel is a **native GPU renderer** optimized for:

* market microstructure analysis
* heatmap depth visualization
* low-latency order flow rendering
* multi-threaded live pipelines

It’s built to **feel institutional**, not retail.

---

# 🏛️ Architecture Overview

Sentinel follows a strict three-layer architecture:

### **1. Core (`libs/core/`)**

* Coinbase WebSocket transport
* Tick-to-timeframe LOD aggregation
* O(1) order-book engine optimized for 5M+ levels
* DataCache & IDataAccessor interfaces
* Zero-allocation hot paths

### **2. GUI (`libs/gui/`)**

* Qt Quick Scenegraph
* GPU heatmap renderer (QSGGeometryNode)
* Triple-buffered VBO pipeline
* Render strategies (Heatmap, Trades, Bubbles, etc.)
* Thread-safe DataProcessor → Renderer handoff

### **3. Apps (`apps/`)**

* `sentinel_gui` — the full terminal
* `stream_cli` — headless streamer for future distributed setups

This separation keeps GUI code out of Core, and Core free of Qt GUI dependencies.

---

# ⚡ Performance at a Glance

| Metric             | Before   | After            | Gain                |
| ------------------ | -------- | ---------------- | ------------------- |
| Paint time         | ~1500 ms | **0.7–1.7 ms**   | 2000×               |
| Cache lookup       | 1.1 s    | **20–130 µs**    | 10k×                |
| Time-slice density | 8–9      | **54+**          | 6×                  |
| Frame throughput   | stalls   | **vsync-stable** | huge                |
| GPU utilization    | low      | **30–35%**       | actual acceleration |

Sentinel uses persistent-mapped VBOs, preallocated buffers, and zero-copy dataflow through the renderer.

---

# 🚀 Quick Start (User-Facing)

If you just want to build and run Sentinel:

```bash
git clone https://github.com/pattty847/Sentinel.git
cmake --preset windows-msvc
cmake --build --preset windows-msvc -j
```

Then run:

```
build/windows-msvc/apps/sentinel_gui/Release/sentinel_gui.exe
```

**For macOS/Linux:**
See `docs/cross_compatibility.md` for the full updated platform guide.

---

# 🧱 Requirements (Developer-Facing)

Sentinel now uses a **unified preset-driven build system.**

### **Shared**

* CMake 3.22+
* vcpkg (with `VCPKG_ROOT` set)
* Git
* Ninja (recommended)

### **Windows**

* Visual Studio 2022 Build Tools (MSVC)
* Qt 6.9+ (msvc2022_64)
* Env vars:

  ```
  setx QT_MSVC C:\Qt\6.9.3\msvc2022_64
  setx VCPKG_ROOT C:\dev\vcpkg
  ```

### **macOS**

```
brew install qt cmake ninja
export QT_MAC=/opt/homebrew/opt/qt
export VCPKG_ROOT=$HOME/vcpkg
```

### **Linux**

```
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-declarative-dev
export QT_LINUX=/usr/lib/qt6
export VCPKG_ROOT=$HOME/vcpkg
```

---

# 🏗️ Building via Presets

Sentinel is now built **exclusively via CMakePresets.json**.

### Configure

```
cmake --preset windows-msvc
# or mac-clang / linux-gcc
```

### Build

```
cmake --build --preset windows-msvc -j
```

### Test

```
cd build/windows-msvc && ctest --output-on-failure
```

---

# 🔧 GPU Backend Selection

Sentinel auto-selects optimized backends:

```cpp
#ifdef Q_OS_WIN
qputenv("QSG_RHI_BACKEND", "d3d11");
#elif defined(Q_OS_MACOS)
qputenv("QSG_RHI_BACKEND", "metal");
#else
qputenv("QSG_RHI_BACKEND", "opengl");
#endif
```

---

# 🔐 Configuration (API Keys)

Add `key.json` to project root for your Coinbase API:

```json
{
  "key": "...",
  "secret": "-----BEGIN EC PRIVATE KEY-----\n...\n-----END EC PRIVATE KEY-----\n"
}
```

---

# 📚 Additional Documentation

* `docs/CROSS_COMPATABILITY.md` — cross-platform build guide
* `docs/ARCHITECTURE.md` — dataflow, scene graph, performance model
* `docs/LOGGING_GUIDE.md` — categorized Qt logging
* `scripts/bootstrap-windows.ps1` — minimal Windows env checker
* `scripts/bootstrap-unix.sh` — minimal macOS/Linux env checker

---

# 🧪 Contribution Standards

* C++20 only
* <500 LOC per file enforced
* RAII everywhere
* No manual `delete`
* No blocking mutexes in hot paths
* Core must have zero GUI dependencies

**Workflow**

```
git checkout -b feature/my-change
cmake --preset <platform>
cmake --build --preset <platform>
ctest
```

---

# 📄 License

Sentinel is licensed under **AGPL-3.0**.
Network-facing forks must remain open-source.