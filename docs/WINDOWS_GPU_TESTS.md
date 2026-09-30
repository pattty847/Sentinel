# Windows GPU tests

How to run the GPU heatmap path (S4, and the B1 tile path) on Windows and read the result.
Background: `docs/research/2026-09-gpu-heatmap-integration-plan.md`, S4.

What is under test:

1. **Compute in `QSGRenderNode::prepare()`** feeding the draw in the same frame.
   Test: `QsgComputeSpikeTests` (executable `test_qsg_compute_spike`).
2. **GPU binning parity and the precision self-test.** GPU cells must equal the CPU reference
   (`binColumn`) exactly, the paged entry buffers (at most 64 MiB each) must work, and the fast
   kernel must pass the runtime precision self-test on this GPU or the binner falls back to the
   precise kernel. Test: `HeatmapGpuBinnerTests` (executable `test_heatmap_gpu_binner`).
3. **B1 tiles.** Test: `HeatmapTileGpuTests` (executable `test_heatmap_tiles_gpu`).

## Backends on Windows

**Direct3D 12 is the Windows backend.** It is the default for the tests, `sentinel-lab` and
`sentinel-gui`. Vulkan also passes and is a supported, tested backend.

**Direct3D 11 is unsupported for the GPU heatmap**, and the binner refuses it (one
`sLog_Error` "Direct3D 11 is unsupported ... use Direct3D 12", then no heatmap draw). Why:
- Qt's D3D11 backend maps every storage buffer to a UAV and allows UAVs only in compute shaders
  (`Unordered access only supported at compute stage`). The display shader reads the binned cells
  as a storage buffer in the fragment stage, so the draw is dropped.
- The kernel binds 15 UAVs. Feature level 11_0 allows 8 (`CreateComputeShader` fails with
  `E_INVALIDARG`). At feature level 11_1 the kernel builds but returns wrong cells (about 97 % of
  cells come back "no data", i.e. the kernel sees a wrong time window). Validation on Vulkan and
  D3D12 found no hazard in our code that explains it (see below); root cause not investigated
  further, by decision.

Status (2026-09-29, RTX 4070, driver 2786, Qt 6.11.2, RelWithDebInfo):

| Backend | QsgComputeSpike | HeatmapGpuBinner | HeatmapTileGpu | Self-test |
|---|---|---|---|---|
| **d3d12** (default) | OK | 26 OK, 1 opt-in skip | 8 OK | fast kernel |
| **vulkan** | OK | 26 OK, 1 opt-in skip | 8 OK | fast kernel |
| d3d11 | fails (fragment UAV) | refused by the binner | not run | n/a |

`ctest` (D3D12 default): `100% tests passed, 0 tests failed out of 54`. Skipped inside passing
suites: POSIX-only (`CaptureApplication.Sigterm...`, two chmod permission fixtures), debug-build
only (`StoreTest.ReaderAssertsWorkerOwnershipInDebugBuilds`), opt-in real data or bench
(`HeatmapGpuParity.RealRecordingOptIn`, `HeatmapModelReal.*`, `ChunkBench.*`).

## The harness picks the backend at run time

`libs/gui/lab/RhiBackend.{hpp,cpp}` is used by the GPU tests, `lab::OffscreenQuick` and
`sentinel-lab`:

- `SENTINEL_RHI_BACKEND=d3d11|d3d12|vulkan|opengl|metal` selects the backend.
- Unset: the platform default, `d3d12` on Windows, `metal` on macOS, `opengl` elsewhere.
- The backend must create a QRhi with compute support. Otherwise every GPU case prints
  `GPU case skipped: <backend> backend: <reason>`. A skipped gtest case still makes ctest report
  **Passed**: read the banner and the skip lines.
- Each test executable prints one banner line first, for example
  `[sentinel] rhi backend=d3d12 (platform default) device="NVIDIA GeForce RTX 4070" driverApi=D3D12`.
- `SENTINEL_RHI_DEBUG=1`: D3D11/D3D12 debug layer, Vulkan validation layer. `SENTINEL_RHI_DEBUG=2`
  adds D3D12 GPU-based validation (slow). D3D12 messages print to stderr as `[d3d12 ERROR #id] ...`.
- Vulkan needs a real platform plugin (the offscreen plugin cannot create a Vulkan instance): set
  `QT_QPA_PLATFORM=windows`. The tests and the headless lab keep `QT_QPA_PLATFORM` when it is set
  and use `offscreen` otherwise.

`sentinel-gui` selects D3D12 with `QQuickWindow::setGraphicsApi` when `QSG_RHI_BACKEND` is unset;
`QSG_RHI_BACKEND` still overrides it. `QSG_RHI_BACKEND` does not affect the tests or the lab.

## Validation (2026-09-29)

- **D3D12 GPU-based validation** found out-of-bounds reads in `heatmap_bin.comp` (46,244 per suite
  run): `heatmap.page0` up to 1.33x its size and `heatmap.bucketSlots` at byte -8. Loads inside a
  ternary arm or behind `&&` were evaluated unconditionally. Fixed (select the index, clamp, load
  once); the suites are now silent under GBV. Under GBV the precise kernel misses 128/384
  self-test cells (GBV instrumentation drops the float-float guarantees); without GBV it is exact.
- **Vulkan validation** found pipelines whose creation-time descriptor set layout was destroyed
  (bindings replaced per source, crossfade and tile). Fixed: pipelines use binner-owned layout
  templates. Remaining messages are Qt's (device layers, `VK_KHR_create_renderpass2`
  dependencies, API 1.0 capability notes, descriptor-pool growth), not our bindings.
- Buffers carry debug names (`heatmap.page0`, `heatmap.bucketSlots`, ...) so messages name them.

## Prerequisites

- Visual Studio 2022 with "Desktop development with C++".
- **Open-source** Qt 6.11.x for MSVC 2022 64-bit, with Qt Charts, Qt WebSockets and Qt Shader
  Tools. A commercial or education Qt install checks a license on every `moc` run and fails the
  build when that license has expired (`AutoMoc: Could not request license for qtframework`).
  Install open-source Qt in its own directory, for example `C:\QtOSS`.
- vcpkg.
- For Vulkan runs: the LunarG Vulkan SDK (headers at configure time, validation layer at run time).
- The environment variables the `windows-msvc-vs` preset reads (open a new shell afterwards):
  ```powershell
  setx QT_MSVC C:\QtOSS\6.11.2\msvc2022_64
  setx VCPKG_ROOT C:\dev\vcpkg
  ```

Note: `README.md` names a preset `windows-msvc`. That preset does not exist. Use `windows-msvc-vs`.

## Build and run

PowerShell, repository root:

```powershell
git rev-parse --short HEAD
cmake --preset windows-msvc-vs            # add --fresh after changing QT_MSVC or installing Vulkan
cmake --build --preset windows-msvc-vs --config RelWithDebInfo
$env:PATH = "$env:QT_MSVC\bin;$env:PATH"
$env:QT_FORCE_STDERR_LOGGING = "1"        # without a console Qt logs to OutputDebugString
ctest --test-dir build\windows-msvc-vs -C RelWithDebInfo -j 8
```

The ctest test preset runs Debug; pass `-C RelWithDebInfo` for this build.

GPU suites on one backend:

```powershell
$env:SENTINEL_RHI_BACKEND = "vulkan"; $env:QT_QPA_PLATFORM = "windows"; $env:SENTINEL_RHI_DEBUG = "1"
ctest --test-dir build\windows-msvc-vs -C RelWithDebInfo -R "QsgComputeSpikeTests|HeatmapGpuBinnerTests|HeatmapTileGpuTests" -V
```

Precision self-test: `HeatmapGpuSelfTest.ShippedFastKernelPassesOnThisDevice`, and the log line
`heatmap gpu: fast kernel passed|failed the precision self-test on <backend>/<device>/...`. The
bench and the lab screenshot JSON report the chosen kernel as `"kernel": "fast"|"precise"`.

### Lab and bench

```powershell
.\build\windows-msvc-vs\apps\sentinel-lab\RelWithDebInfo\sentinel-lab.exe --bench --synthetic 10000000
.\build\windows-msvc-vs\apps\sentinel-lab\RelWithDebInfo\sentinel-lab.exe --synthetic 10000000 --screenshot screenshots\lab-synthetic.png
```

D3D12, RTX 4070, 200 bin passes per grid; bin times are GPU timestamps, upload is separate:

| synthetic | entries | pages | GPU bytes | upload | 1920x1080 p50 / p95 / max | 3840x2160 p50 / p95 / max |
|---|---|---|---|---|---|---|
| 10,000,000 | 9,485,160 | 2 | 85 MB | 39 ms | 0.020 / 0.477 / 0.561 ms | 0.022 / 0.427 / 0.501 ms |
| 30,000,000 | 28,540,008 | 4 | 248 MB | 116 ms | 0.080 / 1.528 / 2.108 ms | 0.041 / 1.358 / 1.675 ms |

100,000,000 does not run: `--synthetic` accepts at most 30,000,000, and a source holds at most
8 pages of 64 MiB (`kMaxEntryPages`), about 67.1 M compact entries.

B1 hybrid (`--b1-bench out.json --b1-quick --b1-modes hybrid`) needs a recording; synthetic data
always uses the full path. It ran on D3D12 against 5 minutes of live recording: time to first
view about 0.75 s, pan and tick-change p95 about 31 ms, 1 to 3 MB of GPU memory. Too little data
to compare with the Mac.

### Real-data parity (opt-in)

The recording root is `SENTINEL_RECORDING_ROOT`, else where the server records: `recording.dir`
in `config/server_config.yaml` (and `.server_config.yaml`), or `recording.fallback_dir` while that
`/Volumes/<name>` volume is not mounted (always on Windows), read relative to the working
directory as the server does.

1. Copy the whole `BTC-USD` recording directory from the Mac. The test compares the **previous
   closed UTC day** at the time it runs, so copy fresh data and run it the same UTC day.
2. Run:
   ```powershell
   $env:SENTINEL_RECORDING_ROOT = "D:\sentinel-data\recording"
   $env:SENTINEL_HEATMAP_REAL_PARITY = "1"
   .\build\windows-msvc-vs\tests\render\RelWithDebInfo\test_heatmap_gpu_binner.exe --gtest_filter=HeatmapGpuParity.RealRecordingOptIn
   ```
   It skips with a reason when `SENTINEL_HEATMAP_REAL_PARITY` is not `1`, when there is no
   recording root, or when `BTC-USD` is missing under it.

Not run yet on Windows (no recording copied).

### sentinel-gui

The stream server needs its dev certificate once: `.\certs\gen-certs.ps1` (the bash script
cannot drive the native Windows OpenSSL). Then start `sentinel-server` and `sentinel-gui` from
the repository root.

The Agent API screenshot `target=main` does not capture the chart on D3D12: the chart is a
`QQuickView` in a window container, and the D3D12 swapchain is not visible to that window grab
(it shows stale widget content). Use `target=heatmap` for the chart.

## Pass

- The banner names the backend you meant to test and a real device.
- `QsgComputeSpike.ComputeInRenderNodePrepareFeedsRenderInSameFrame` is `[       OK ]`.
- Every `HeatmapGpuParity.*`, `HeatmapGpuSelfTest.*`, `HeatmapRenderNodeScene.*`,
  `HeatmapGpuAnchoring.*`, `HeatmapTickPolicyNode.*` and tiles case is `[       OK ]`. Only
  `RealRecordingOptIn` may skip.
- With `SENTINEL_RHI_DEBUG=1`: no `[d3d12 ERROR` line, and no Vulkan message that names a
  `heatmap.*` buffer or a pipeline layout.

## Fail

- Any `[  FAILED  ]`, or a GPU case `[  SKIPPED ]` on d3d12 or vulkan.
- `ShippedFastKernelPassesOnThisDevice` fails while the parity cases pass: the fast kernel is not
  exact on this GPU. The binner keeps the precise kernel; report the GPU and driver.
- `Failed to create compute shader`, a `QRhi` creation error or a crash.

## What to send back

1. `git rev-parse --short HEAD`.
2. GPU and driver version (the banner, or `dxdiag` > Display).
3. Qt version (the folder under `QT_MSVC`).
4. The full `ctest ... -V` output per backend you ran.
5. The real parity output, if run.
6. Build errors, verbatim.
