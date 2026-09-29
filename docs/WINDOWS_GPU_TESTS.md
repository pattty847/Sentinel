# Windows D3D11 GPU tests

This is the procedure for the owner to check the GPU heatmap path (S4) on Windows with the
Direct3D 11 backend. Background: `docs/research/2026-09-gpu-heatmap-integration-plan.md`, S4.

Two things are under test:

1. **Compute in `QSGRenderNode::prepare()`**. This is proven on Metal. On D3D11 it has never run.
   Test: `QsgComputeSpikeTests` (executable `test_qsg_compute_spike`).
2. **GPU binning parity and the precision self-test.** GPU cells must equal the CPU reference
   (`binColumn`) exactly, the paged entry buffers must work under the D3D11 buffer limits, and
   the fast kernel must pass the runtime precision self-test on your GPU.
   Test: `HeatmapGpuBinnerTests` (executable `test_heatmap_gpu_binner`).

## Read this first: the harness is Metal-only today

On current `main`, both tests create a **Metal** QRhi only:
- `tests/render/test_heatmap_gpu_binner.cpp` (`struct Headless`) calls `QRhi::create(QRhi::Metal, ...)`
  inside `#ifdef Q_OS_MACOS`.
- `tests/render/test_qsg_compute_spike.cpp` uses `lab::OffscreenQuick`
  (`libs/gui/lab/OffscreenQuick.cpp`). It creates a Metal QRhi and calls
  `QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal)`. `lab::metalDeviceAvailable()`
  returns `false` on every platform except macOS.
- `sentinel-lab` (`apps/sentinel-lab/main.cpp`) also forces Metal, and `--bench` is Metal-only
  (`libs/gui/lab/Bench.cpp`).

So on Windows every GPU case prints `[  SKIPPED ]` with `No MTLDevice`, and ctest still reports
the test as **Passed**. **That is not a D3D11 result.**

`QSG_RHI_BACKEND=d3d11` does not change this. That Qt variable selects the backend of a Qt Quick
window that Qt creates itself. These tests create their QRhi directly, and the lab sets the
graphics API in code. D3D11 is already the Qt Quick default on Windows.

**Step 0 (a code slice, before Part B).** The orchestrator dispatches a small harness change:
- In `Headless` and in `OffscreenQuick::create`, under `Q_OS_WIN`, create `QRhi::D3D11` with
  `QRhiD3D11InitParams`. In `OffscreenQuick`, select `QSGRendererInterface::Direct3D11`.
- Make the skip messages name the backend.
- Make the real-data recording root configurable. Today it is hard-coded as
  `/Volumes/T7/sentinel-data/recording` (in the test and in `libs/gui/lab/LabSources.hpp`
  `kRecordingRoot`).

Until that change is on `main`, run Part A only.

## Prerequisites

- Visual Studio 2022 with the "Desktop development with C++" workload.
- Qt for MSVC 2022 64-bit, with the Qt Shader Tools module. S4 was verified with Qt 6.11.2 on
  the Mac, so use 6.11.x if you can.
- vcpkg.
- These environment variables, which the `windows-msvc-vs` preset in `CMakePresets.json` reads:
  ```powershell
  setx QT_MSVC C:\Qt\6.11.2\msvc2022_64   # your Qt kit path
  setx VCPKG_ROOT C:\dev\vcpkg            # your vcpkg path
  ```
  Open a new PowerShell after `setx`.

Note: `README.md` names a preset `windows-msvc`. That preset does not exist. Use `windows-msvc-vs`.

## Part A: build and smoke run (works today)

From the repository root in PowerShell:

```powershell
git checkout main
git pull
git rev-parse --short HEAD
cmake --preset windows-msvc-vs
cmake --build --preset windows-msvc-vs --config Debug --target test_qsg_compute_spike test_heatmap_gpu_binner
$env:PATH = "$env:QT_MSVC\bin;$env:PATH"
ctest --preset windows-msvc-vs -R "QsgComputeSpikeTests|HeatmapGpuBinnerTests" -V
```

- The test preset `windows-msvc-vs` runs the Debug configuration. The build step builds Debug to match.
- The executables are in `build\windows-msvc-vs\tests\render\Debug\`.
- The tests set `QT_QPA_PLATFORM=offscreen` themselves. If Qt reports that it cannot find the
  `offscreen` platform plugin, also set `$env:QT_PLUGIN_PATH = "$env:QT_MSVC\plugins"`.

Expected today:
- Both targets compile and link with MSVC. This also compiles the HLSL 5.0 variants of every
  heatmap shader through `qsb`.
- The CPU-only cases pass: `HeatmapGpuSourceCpu.*`, `HeatmapBinGrid.*` and
  `HeatmapGpuSelfTest.FixtureOracleIsSelfConsistent`.
- Every GPU case is `[  SKIPPED ]` with `No MTLDevice`.

A compile error, a link error or a crash is a real finding. Send it back.

## Part B: the D3D11 run (after step 0 is on main)

Run the same commands as Part A. Then run the real-data parity:

1. Copy the recording from the Mac: the whole `BTC-USD` directory from
   `/Volumes/T7/sentinel-data/recording/`. The test compares the **previous closed UTC day**
   at the time it runs, so copy fresh data and run it the same UTC day.
2. Put the directory where the step 0 change says the root is.
3. Enable the opt-in case and run the executable directly:
   ```powershell
   $env:SENTINEL_HEATMAP_REAL_PARITY = "1"
   .\build\windows-msvc-vs\tests\render\Debug\test_heatmap_gpu_binner.exe --gtest_filter=HeatmapGpuParity.RealRecordingOptIn
   ```
   Without `SENTINEL_HEATMAP_REAL_PARITY=1`, the case skips with
   `set SENTINEL_HEATMAP_REAL_PARITY=1 to compare against the real recording`. Without the data,
   it skips with `recording directory absent`.

### Pass

- `QsgComputeSpike.ComputeInRenderNodePrepareFeedsRenderInSameFrame` is `[       OK ]`, not
  skipped. It checks that the compute output reaches the pixels in the same frame, that the rest
  of the scene is intact, and that a second frame dispatches again.
- Every `HeatmapGpuParity.*`, `HeatmapGpuSelfTest.*` and `HeatmapRenderNodeScene.*` case is
  `[       OK ]`. No GPU case is skipped.
- `HeatmapGpuSelfTest.ShippedFastKernelPassesOnThisDevice` is OK. That means the fast kernel is
  exact on your GPU and driver under D3D11.
- `HeatmapGpuParity.EntriesSplitAcrossPagesMatchSinglePage` is OK. That means the paged entry
  buffers work.
- Real parity: every `real ...` line and the final `real GPU parity total: ...` line show
  `mismatches state=0 code=0 side=0 validity=0`.

### Fail

- Any `[  FAILED  ]`, or any GPU case still `[  SKIPPED ]` after step 0.
- `ShippedFastKernelPassesOnThisDevice` fails: the fast kernel is not exact on this GPU. The
  binner then keeps the slower `precise` kernel. Correctness holds, but report the GPU.
- Any non-zero mismatch count in the real parity lines.
- A `QRhi` creation error, a shader load error, or a crash.

## What to send back

1. The output of `git rev-parse --short HEAD`.
2. The GPU and driver version (Device Manager, or `dxdiag` > Display).
3. The Qt version (the folder name under `QT_MSVC`).
4. The full `ctest ... -V` output from Part A or Part B.
5. For Part B, the full output of the real parity run.
6. Any build errors, verbatim.
