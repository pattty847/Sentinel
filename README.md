# Sentinel v1.0.0-alpha

GPU-accelerated trading terminal. C++20, Qt 6, GPU-resident rendering.
<div align="center">
  <img src="https://img.shields.io/badge/C++-20-blue" />
  <img src="https://img.shields.io/badge/Qt-6-green" />
  <img src="https://img.shields.io/badge/GPU-Accelerated-purple" />
  <img src="https://img.shields.io/badge/Platform-Cross--Platform-lightgrey" />
  <img src="https://img.shields.io/badge/License-AGPL--3.0-blue" />
</div>


## 📸 NEW PHOTOS COMING SOON!

### New Entirely GPU Based Heatmap w/ Server Client Architecture
*zero missing slices as before*
<img width="1913" height="921" alt="image" src="https://github.com/user-attachments/assets/2d712c0a-9ead-41ea-9ee6-2443662d0aaf" />

---

## High-Performance Architecture

Sentinel has been rebuilt from the ground up as a distributed **Client-Server** system. The legacy monolithic and CPU-bound rendering paths have been gutted in favor of a GPU-first pipeline.

- **Distributed Core**: A headless server handles 24/7 ingestion, persistence, and TWAP aggregation.
- **GPU Heatmap**: Zero CPU overhead per-cell. Renders 8192x8192 grids (67M+ cells) at 60+ FPS via single-quad GPU shading.
- **Pure Streaming**: Real-time binary/JSON protocol connects the server to lightweight visualization clients.

---

## Build

**Windows:**
```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc -j
```

**Linux:**
```bash
cmake --preset linux-gcc
cmake --build --preset linux-gcc -j
```

---

## Running Sentinel

1. **Start the Data Server**:
   `./build/linux-gcc/apps/sentinel-server/sentinel-server`

2. **Start the GUI Client**:
   `./build/linux-gcc/apps/sentinel_gui/sentinel_gui`

*Note: Environment variables like `SENTINEL_GPU_HEATMAP=1` and `SENTINEL_REMOTE=1` are now the defaults for the optimized path.*

---

## Project Structure

```
libs/core/    Headless data layer, persistence, and streaming protocols.
libs/gui/     GPU-resident rendering strategies (no CPU loops), Qt Quick components.
apps/         sentinel-server (Daemon) & sentinel_gui (Visualizer).
```

---

## Docs

- `docs/ARCHITECTURE.md` — The new distributed dataflow.
- `docs/CLIENT-SERVER.md` — Protocol and aggregation details.
- `docs/LOGGING_GUIDE.md` — Monitoring the sentinel.* channels.

---

## License


AGPL-3.0
