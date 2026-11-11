# Contributing to Sentinel

Sentinel is a market-data terminal: a recording server (`sentinel-server`, `sentinel-capture`) that
records exchange order books and trades to a journal and rolls them into columns, and a Qt Quick
GUI (`sentinel-gui`) that renders heatmaps, candles and order-book tools on the GPU. This file holds
the rules every change follows, whether a person or a coding agent writes it.

## Build and test

Dependencies, presets and platform setup are in [README.md](README.md#build-from-source).

```bash
cmake --preset mac-clang                      # or windows-msvc-vs, linux-gcc
cmake --build --preset mac-clang -j 6
ctest --test-dir build/mac-clang --output-on-failure
ctest --test-dir build/mac-clang -R '<TestName>' --output-on-failure   # one test
```

- Windows: `cmake --build --preset windows-msvc-vs`. GPU setup: [docs/WINDOWS_GPU_TESTS.md](docs/WINDOWS_GPU_TESTS.md).
- Keep `-j` moderate on a 16 GB machine: ninja's default can exhaust memory with the Qt and Boost
  translation units.

Verification ladder, smallest first:
1. Format/lint touched scope only
2. Build affected target(s)
3. Run targeted tests / targeted repro
4. The full `ctest` suite before the change lands

Reading results:
- A skipped GPU case still reports Passed. Read the output for `GPU case skipped`; llvmpipe is not GPU verification.
- Synthetic fixtures are not live-provider proof. Say which one you ran.
- Check the log header `exe=... built=...` so you do not test a stale binary.

## Core identity

- **Core stays free of GUI Qt** (QtCore is allowed: QObject/signals, QTimer, QByteArray, QString; no QtGui, QtQuick, QML or QSG in core)
- **GUI owns Qt/QML/QSG behavior**
- **Rendering is GPU-first, deterministic, and low-lag**

Prefer simpler/faster designs over preserving weak legacy patterns unless compatibility is explicitly required.

**No backward compatibility.** Once a replacement works, delete the superseded code path, format,
reader and their tests in the same change. Keep a switch only while a live A/B comparison is wanted.
Old recorded data is not a reason to keep code.

## Critical invariants

- **Viewport updates must go through `setViewport()`** so `viewportVersion` increments.
  Do not mutate viewport fields directly. If `viewportVersion` does not change, rebuild logic can fail.

- **Threading**
  - Network and data processing stay off GUI thread
  - Cross-thread communication uses `Qt::QueuedConnection`
  - QSG/render-thread code must not touch GUI `QObject` graphs

- **Render path behavior**
  - Preallocate/reuse QSG geometry and nodes where practical
  - Validate inputs (skip NaN/inf)
  - Avoid unnecessary node/geometry churn in hot paths

## Where things belong

- `libs/core`: market data transport, dispatchers, order books, DTOs, transforms (non-UI logic)
- `libs/gui`: Qt/QML/QSG, rendering strategies, window/widget behavior
- `apps/`: thin bootstraps only (no business logic)

If a change crosses these boundaries, state why before implementing it.

## Chart core: where chart behaviour goes

`UnifiedGridRenderer` (UGR) is a Qt Quick shell. It changes only for item lifecycle, window/sync wiring and `QQuickItem` overrides. Current architecture is in `docs/ARCHITECTURE.md`.

- View behaviour (camera, zoom, glide, fit, follow-live, placement, timeframe or symbol switch) goes in a policy class in `libs/gui/chart/core/` once that directory exists, never as new UGR members. `ChartViewController` only sequences policies: no arithmetic or branching on prices, times or pixels.
- `sentinel_chart_core` links QtCore only: no QtGui, QtQuick, QSG, QML or `QQuickWindow` (the build and `chart-health.sh` enforce it). It never includes `HeatmapGpuLayer.hpp`, the source controller or recording types; it talks through `ChartPorts.hpp`.
- A new chart overlay is an `IChartLayer` in `libs/gui/chart/layers/` plus one line in `ChartLayerSet.cpp`. Render-thread code reads only `ChartFrame`, its own state and its mailbox; never `GridViewState`, a `QObject` or UGR.
- New QML knobs go on the owning sub-object (`view`, `candles`, `tpo`, `layers`, `debug`); new Agent API fields go in `ChartAgentPort`; new test hooks go on the policy under test, not on UGR.
- New view behaviour ships with a Qt-free unit test of its policy that fails without the change. A GPU test alone is not enough.
- `tests/render/golden/*` (CPU view trace, Metal frame hashes) is re-blessed only in a change that declares a visible change; its description names every changed scene and why. A change labelled visually neutral with a re-bless is rejected. On macOS with Metal a missing or mismatched golden fails.
- `scripts/dev/chart-health.sh` must pass before a chart change lands. Run it on a committed tip (it diffs merge-base..HEAD, so an uncommitted diff passes unchecked). It checks only the chart and UGR files the branch changed (`libs/gui/chart/**`, `libs/gui/UnifiedGridRenderer*`), against the branch's merge-base with `main`:
  - file caps: no UGR file grows past its merge-base size; from S14, `UnifiedGridRenderer.cpp` <= 450 lines and `.h` <= 350; every `libs/gui/chart/**` file <= 500;
  - UGR `m_` member count never grows (<= 15 from S14);
  - new functions CCN <= 15, <= 60 NLOC, <= 6 parameters (`ChartViewController` CCN <= 10); a touched or moved existing function may not rise above max(15, its merge-base CCN);
  - no new jscpd clone (>= 8 lines / 60 tokens) involving `libs/gui/chart/**` or `UnifiedGridRenderer*`;
  - no QtGui/Quick/SG/QML include in `chart/core`; no `UnifiedGridRenderer.h` include outside the shell allowlist.

Reviewers of any `libs/gui/UnifiedGridRenderer*` or `libs/gui/chart/**` diff reject: new UGR members, properties or signals that aren't lifecycle or wiring; a hand-copied camera setup instead of `ChartCamera`; a new `IChartLayer` parameter for one layer's needs; changed signal emission order or reordered floating-point operands in a change labelled neutral; per-frame allocation on the render path (see Hot paths); process comments (slice, review-round or plan references) in production code.

## Hot paths (treat like live wires)

Changes here require performance caution and small diffs:
- `DataProcessor`
- `BookRecorder` (every book message on the recorder worker)
- `HeatmapGpuLayer` and `HeatmapGpuBinner` (render thread, every frame)
- QSG geometry updates

Before editing hot paths, explicitly check for per-frame risk:
- allocations
- object creation/destruction
- signal emissions
- binding churn / unnecessary recomputation

## Logs and probes (debug what the user saw)

Every run of `sentinel-gui` and `sentinel-server` writes its own log file. Read it before guessing.

Where:
- macOS: `~/Library/Logs/Sentinel/sentinel-gui-latest.log`, `~/Library/Logs/Sentinel/sentinel-server-latest.log` (symlinks to the newest run)
- Older runs: `<app>-YYYYMMDD-HHMMSS-<pid>.log` in the same dir (last 20 kept, `SENTINEL_LOG_KEEP`)
- Windows/Linux: `<GenericDataLocation>/Sentinel/logs`. `SENTINEL_LOG_DIR` overrides. stderr prints `[sentinel] log file: <path>` at startup.

Read:
- Header lines start with `#`: version, pid, `exe=... built=...`, args, cwd, `SENTINEL_*`/`QT_*`/`QSG_*` env.
- Line format: `<local time> <D/I/W/E/F> <category> <thread> <file:line> | <message>`
- Categories: `app`, `data`, `render`, `debug`, `probe`, plus Qt's own (`qt.*`, `default` for plain qDebug).
- Start with `rg ' [WEF] ' <log>`, then narrow by the time the user describes, category, and thread (`main`, `QSGRenderThread`, ...).

Probes (values for a specific behavior, off by default):
- List them: `rg -o 'sLog_Probe\("[^"]+"' libs apps | sort -u`
- Enable by name or prefix: `SENTINEL_PROBES=tpo,heatmap.window` (`all` enables every probe). A prefix enables its children (`tpo` -> `tpo.ingest`).
- If the log lacks the values you need, ask the user to rerun with the probes on, or add a probe and ask them to reproduce.

Write logs (`libs/core/SentinelLogging.hpp`):
- `sLog_App/Data/Render/Debug(...)`: every call prints. State changes and one-off events. Include identifying values as `key=value` (symbol, tf, range, counts).
- `sLog_Warning/sLog_Error(...)`: problems. Do not swallow a failure silently.
- `sLog_Probe("area.detail", "k=" << v)`: anything per frame, per message, or per bucket. Name is a string literal; checked once per call site, free when off.
- `sLog_*N(ms, ...)`: per-site time throttle, only for an always-on recurring line.
- Do not gate logging with ad-hoc env vars, and do not write side files (`/tmp/*.log`, `.cursor/debug.log`). Everything goes through Qt logging so it lands in the run log.
- Too noisy: `QT_LOGGING_RULES="sentinel.render.debug=false"` silences a category.

## Running the GUI and screenshots

Look before you claim a visual or performance result. The GUI Agent API (state, viewport, docks,
screenshots) is in [docs/AGENT_API.md](docs/AGENT_API.md).

- Run an isolated GUI: the GUI host (`scripts/dev/gui-host.py` with the client `scripts/dev/gui-shot.sh`), or launch `sentinel-gui` with `--agent-host <scratch dir>`, its own `--api-port` and its own scratch directory. Headless Linux: `scripts/dev/cloud-gui.sh`.
- Allowed targets: `window`, `heatmap`, a retained dock ID (`orderBook`, `watchlist`, `screener`, `stockChart`, `paperTrading`, `sec`, `copenet`, `telemetry`), `statusBar`, `toolbar`, `chartmenu`, `settings[:Tab]`. Show or focus a hidden dock through the dock API first.
- Never use `target=main`: it captures whatever window covers the GUI, including other apps' private content. If any shot shows something that is not Sentinel, delete it at once and report it. This includes `scripts/dev/cloud-gui.sh`: pass `window` explicitly, because its `shot` defaults to `main`.
- Never run the GUI binary with `--help` (it starts a full GUI).
- Do not add a native-format `QSettings("org","app")` to the GUI: use `QSettings(QSettings::defaultFormat(), QSettings::UserScope, "org", "app")` (a test enforces it).
- Screenshots fail with `grab_failed` while the Mac screen is locked (`ioreg -n Root -d1 -a | grep -A1 ScreenIsLocked`).

## Commits

- Commit a coherent, verified feature slice; do not sweep unrelated modified files into it. Messages say what changed and why at the feature level.
- Never commit secrets or screenshots.
- Line endings: LF everywhere (`.gitattributes` enforces it; `.ps1`/`.bat` check out as CRLF). Never write CRLF into other files.

## Where docs live

`docs/README.md` owns the documentation catalog, maintenance status and links. Keep that index in
sync when adding, moving, deleting or changing the maintenance status of a doc. Update only the
relevant file in its **Maintained documents** section when the change affects that file's contract;
a matching keyword in another doc is not a reason to edit it. Supporting references are read-only
unless a maintainer explicitly asks to change or promote them.

Update the relevant canonical doc in the same change:
- Protocol / wire / DTO semantics → `docs/MARKETDATA.md`
- Architecture / ownership / dependency direction → `docs/ARCHITECTURE.md`
- Config keys/defaults/semantics → `docs/CONFIG.md`
