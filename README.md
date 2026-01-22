# Sentinel v2.1.0-alpha

Open Source GPU-accelerated Orderbook Heatmap trading terminal. Written in C++20, Qt 6, and with sub-millisecond rendering.

<div align="center">
  <img src="https://img.shields.io/badge/C++-20-blue" />
  <img src="https://img.shields.io/badge/Qt-6-green" />
  <img src="https://img.shields.io/badge/GPU-Accelerated-purple" />
  <img src="https://img.shields.io/badge/Platform-Cross--Platform-lightgrey" />
  <img src="https://img.shields.io/badge/License-AGPL--3.0-blue" />
</div>

## Server-Client Main Branch Recent Changes

<img width="1918" height="1030" alt="image" src="https://github.com/user-attachments/assets/945eca03-91f2-4af7-899a-70a594847cf3" />

- Full Server Client architecture
- Entirely GPU based heatmap rasterization and rendering
- One quad, texture sampling
- 60 FPS, easily higher
- 8192 x 8192 - 67M Cells, easily can push higher
- TWAP Heatmap Cell Aggregation
- Full Docking Framework
- Qt RHI backend for any OS

---

## Build

**Windows:**

```
setx QT_MSVC C:\Qt\6.9.3\msvc2022_64
setx VCPKG_ROOT C:\dev\vcpkg

git clone https://github.com/pattty847/Sentinel.git
cd Sentinel
cmake --preset windows-msvc
cmake --build --preset windows-msvc -j
```

Run: `build/windows-msvc/apps/sentinel_gui/Release/sentinel_gui.exe`

**macOS:**

```
brew install qt cmake ninja
export QT_MAC=/opt/homebrew/opt/qt
export VCPKG_ROOT=$HOME/vcpkg

cmake --preset mac-clang
cmake --build --preset mac-clang -j
```

**Linux:**

```
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-declarative-dev
export QT_LINUX=/usr/lib/qt6
export VCPKG_ROOT=$HOME/vcpkg

cmake --preset linux-gcc
cmake --build --preset linux-gcc -j
```

---

## API Keys

Drop a `key.json` in project root:

```json
{
  "key": "your-coinbase-api-key",
  "secret": "-----BEGIN EC PRIVATE KEY-----\n...\n-----END EC PRIVATE KEY-----\n"
}
```

---

## Project Structure

```
libs/core/    Pure C++ data layer (no Qt GUI)
libs/gui/     Qt Quick, QSG rendering, widgets
apps/         Executables (sentinel_gui, stream_cli)
```

Core handles market data, caching, and transforms. GUI handles all rendering and layout. They don't mix.

---

## Docs

- `docs/ARCHITECTURE.md` — dataflow and rendering pipeline
- `docs/CROSS_COMPATABILITY.md` — platform-specific notes

---

## License

AGPL-3.0
