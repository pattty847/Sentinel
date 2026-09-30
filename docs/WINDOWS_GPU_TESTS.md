# Windows GPU tests

How to run the GPU heatmap path (S4) on Windows and read the result. Background:
`docs/research/2026-09-gpu-heatmap-integration-plan.md`, S4.

Two things are under test:

1. **Compute in `QSGRenderNode::prepare()`** feeding the draw in the same frame.
   Test: `QsgComputeSpikeTests` (executable `test_qsg_compute_spike`).
2. **GPU binning parity and the precision self-test.** GPU cells must equal the CPU reference
   (`binColumn`) exactly, the paged entry buffers (at most 64 MiB each) must work, and the fast
   kernel must pass the runtime precision self-test on this GPU or the binner falls back to the
   precise kernel. Test: `HeatmapGpuBinnerTests` (executable `test_heatmap_gpu_binner`).

## Status (2026-09-29, RTX 4070, Qt 6.11.2, branch `windows/gpu-harness`)

| Backend | QsgComputeSpike | HeatmapGpuBinner | Lab screenshot |
|---|---|---|---|
| **d3d12** | OK | 26 OK, 1 opt-in skip | correct heatmap |
| **d3d11** (the default) | FAILED | 10 FAILED, 16 OK, 1 skip | background only |

**D3D11 does not work today. Use D3D12 on Windows.** D3D11 has two separate problems:

1. **Draw.** Qt's D3D11 backend maps every storage buffer to a UAV and allows UAVs only in
   compute shaders (`Unordered access only supported at compute stage`). The production display
   shader (`heatmap_display.frag`) and the spike read the binned cells as a storage buffer in the
   fragment shader, so the draw is dropped. Fixing it needs a display-path change (for example
   compute writes a texture that the fragment shader samples). Not done.
2. **Compute.** The kernel binds 15 UAVs. Feature level 11_0 allows 8, and Qt's default D3D11
   device never selects 11_1, so `CreateComputeShader` fails with `E_INVALIDARG`. The harness
   asks for 11_1 (`featureLevel=0xb100` in the banner). At 11_1 the pipeline builds, but both the
   fast and the precise kernel return wrong cells (384/384 self-test cells differ). Root cause not
   found yet; the D3D11 debug layer is the next step. The production app would also need to ask
   for 11_1 (`QQuickGraphicsDevice::fromAdapter(..., featureLevel)`).

## The harness picks the backend at run time

`libs/gui/lab/RhiBackend.{hpp,cpp}` is used by both GPU tests, `lab::OffscreenQuick` and
`sentinel-lab`:

- `SENTINEL_RHI_BACKEND=d3d11|d3d12|vulkan|opengl|metal` selects the backend.
- Unset: the platform default, `d3d11` on Windows, `metal` on macOS, `opengl` elsewhere.
- The backend must create a QRhi with compute support. If it cannot, every GPU case prints
  `GPU case skipped: <backend> backend: <reason>`.
- Each test executable prints one banner line first, for example:
  `[sentinel] rhi backend=d3d12 (SENTINEL_RHI_BACKEND) device="NVIDIA GeForce RTX 4070" driverApi=D3D12`

A skipped gtest case still makes ctest report **Passed**. Read the banner and the skip lines.

`QSG_RHI_BACKEND` does not affect these tests: they create their QRhi directly.

## Prerequisites

- Visual Studio 2022 with "Desktop development with C++".
- **Open-source** Qt 6.11.x for MSVC 2022 64-bit, with Qt Charts, Qt WebSockets and Qt Shader
  Tools. A commercial or education Qt install checks a license on every `moc` run and fails the
  build when that license has expired (`AutoMoc: Could not request license for qtframework`).
  Install open-source Qt in its own directory, for example `C:\QtOSS`.
- vcpkg.
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
cmake --preset windows-msvc-vs            # add --fresh after changing QT_MSVC
cmake --build --preset windows-msvc-vs --config RelWithDebInfo
$env:PATH = "$env:QT_MSVC\bin;$env:PATH"
$env:QT_FORCE_STDERR_LOGGING = "1"        # Qt warnings (shader, UAV) go to stderr, not the debugger
```

GPU tests on one backend (the ctest test preset runs Debug, so pass `-C` for this build):

```powershell
$env:SENTINEL_RHI_BACKEND = "d3d12"
ctest --test-dir build\windows-msvc-vs -C RelWithDebInfo -R "QsgComputeSpikeTests|HeatmapGpuBinnerTests" -V
```

Or run the executables directly (in `build\windows-msvc-vs\tests\render\RelWithDebInfo\`). They set
`QT_QPA_PLATFORM=offscreen` themselves.

Precision self-test: `HeatmapGpuSelfTest.ShippedFastKernelPassesOnThisDevice`, and the run log line
`heatmap gpu: fast kernel passed|failed the precision self-test on <backend>/<device>/...`. The
bench and the lab screenshot JSON report the chosen kernel as `"kernel": "fast"|"precise"`.

### Lab

```powershell
$env:SENTINEL_RHI_BACKEND = "d3d12"
.\build\windows-msvc-vs\apps\sentinel-lab\RelWithDebInfo\sentinel-lab.exe --bench --synthetic 10000000
.\build\windows-msvc-vs\apps\sentinel-lab\RelWithDebInfo\sentinel-lab.exe --synthetic 10000000 --screenshot screenshots\lab-synthetic.png
```

`--synthetic 10000000` builds about 9.49 M entries, 2 entry pages (at most 64 MiB each), about
85 MB of GPU buffers. It allocates and bins on D3D11 and D3D12; results are correct on D3D12 only.

Bench on D3D12 (GPU timestamps, 200 passes per grid):

| grid | p50 | p95 | max |
|---|---|---|---|
| 1920x1080 | 0.047 ms | 0.647 ms | 0.828 ms |
| 3840x2160 | 0.039 ms | 0.634 ms | 0.807 ms |

The D3D11 bench numbers (p50 0.008 ms) are not meaningful: the kernel returns wrong cells there.

### Real-data parity (opt-in)

1. Copy the whole `BTC-USD` recording directory from the Mac. The test compares the **previous
   closed UTC day** at the time it runs, so copy fresh data and run it the same UTC day.
2. Point the harness at the directory that holds `BTC-USD\` and enable the case:
   ```powershell
   $env:SENTINEL_RECORDING_ROOT = "D:\sentinel-data\recording"
   $env:SENTINEL_HEATMAP_REAL_PARITY = "1"
   .\build\windows-msvc-vs\tests\render\RelWithDebInfo\test_heatmap_gpu_binner.exe --gtest_filter=HeatmapGpuParity.RealRecordingOptIn
   ```
   It skips with a reason when `SENTINEL_HEATMAP_REAL_PARITY` is not `1`, when
   `SENTINEL_RECORDING_ROOT` is unset, or when `BTC-USD` is missing under it. The lab's real-data
   mode reads the same variable.

Not run yet on Windows (no recording copied).

## Pass

- The banner names the backend you meant to test and a real device.
- `QsgComputeSpike.ComputeInRenderNodePrepareFeedsRenderInSameFrame` is `[       OK ]`, not skipped.
- Every `HeatmapGpuParity.*`, `HeatmapGpuSelfTest.*`, `HeatmapRenderNodeScene.*`,
  `HeatmapGpuAnchoring.*` and `HeatmapTickPolicyNode.*` case is `[       OK ]`. Only
  `RealRecordingOptIn` may skip.
- `HeatmapGpuParity.EntriesSplitAcrossPagesMatchSinglePage` is OK: the paged entry buffers work.
- Real parity: every `real ...` line and `real GPU parity total: ...` show
  `mismatches state=0 code=0 side=0 validity=0`.

## Fail

- Any `[  FAILED  ]`, or a GPU case `[  SKIPPED ]` on a backend that should work.
- `ShippedFastKernelPassesOnThisDevice` fails while the parity cases pass: the fast kernel is not
  exact on this GPU. The binner keeps the precise kernel; report the GPU and driver.
- `Failed to create compute shader`, `Unordered access only supported at compute stage`, a
  `QRhi` creation error or a crash.

## Other Windows ctest failures (not GPU)

On 2026-09-29, with the default backend, these suites also fail on Windows:

- Recording store (`Hmc2StoreTests`, `BookRecorderTests`, `RecordingChunkTests`,
  `RecordingPageTests`, `RecordingLiveTests`, `HeatmapModelTests`, `StorageProbeTests`,
  `RecordingServerStopTests`): `Hmc2Store: sync path=C:\ error=5`. `mkdirs` fsyncs every ancestor
  directory up to the drive root, and a normal user cannot open `C:\` for write. A policy
  decision (which directories must be synced) is needed.
- `BacktestCoreTests`, `HeatmapTwapStreamerTests`: `remove_all` fails because a file is still
  open (Windows does not delete open files).
- `StorageProbeCliTests.KilledProcessLeavesMinuteCheckpoint`: kill semantics differ on Windows.
- `RecordingDataProcessorTests`: passed in the first run of the day, then failed in every later
  run (no request issued). Not investigated; appears time dependent.

## What to send back

1. `git rev-parse --short HEAD`.
2. GPU and driver version (the banner, or `dxdiag` > Display).
3. Qt version (the folder under `QT_MSVC`).
4. The full `ctest ... -V` output per backend you ran.
5. The real parity output, if run.
6. Build errors, verbatim.
