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

<img width="2559" height="1392" alt="image" src="https://github.com/user-attachments/assets/48372665-9f01-4652-95bd-3c7bc7133734" />

- Full Server Client architecture
- Entirely GPU based heatmap rasterization and rendering
- One quad, texture sampling
- **110+ FPS with live axis updates during pan/zoom**
- Zero-allocation axis rendering (fixed-capacity models, no QML object churn)
- Nice ticks with hysteresis on price and time axes
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

## Run

Server:

```
SENTINEL_HEATMAP_TF=100 ./build/linux-gcc/bin/sentinel-server
```

GUI Client:

```
SENTINEL_HEATMAP_TF=100 ./build/linux-gcc/apps/sentinel_gui/sentinel_gui
```

Client requires a server connection; `SENTINEL_HEATMAP_TF` must match on both.

---

## Configuration

Sentinel uses YAML configuration files for runtime settings. This is more convenient than setting dozens of environment variables.

**Setup:**

```bash
# Copy the template to create your config
cp sentinel.yaml.template sentinel.yaml

# (Optional) Create user-specific overrides not tracked in git
cp sentinel.yaml.template .sentinel.yaml
```

**Config Priority:**

1. CLI environment variables (highest)
2. `.sentinel.yaml` (user overrides)
3. `sentinel.yaml` (project defaults)

**Common settings:**

```yaml
heatmap:
  timeframe: 1000      # communicates to client the timeframe
  grid_width: 5120
  grid_height: 2048

server:
  default_symbols: BTC-USD,ETH-USD    # comma separated list

gui:
  api_port: 17100      # Screenshot API endpoint
  screenshot_dir: ./screenshots
```

See `sentinel.yaml.template` for all options or `docs/ENV_VARS.md` for complete reference.

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
apps/         Executables (sentinel_gui, sentinel-server)
```

Core handles market data, caching, and transforms. GUI handles all rendering and layout. They don't mix.

---

## Docs

- `docs/ARCHITECTURE.md` — dataflow and rendering pipeline
- `docs/MARKETDATA.md` — MarketDataCoreEngine pipeline
- `docs/FEATURES.md` - Outline of features for the terminal

---

## License

AGPL-3.0

