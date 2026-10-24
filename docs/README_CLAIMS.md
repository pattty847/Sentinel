# README claims ledger

Evidence for every factual claim in [README.md](../README.md). One row per claim. When a README claim
changes, re-measure it and replace its row here in the same change. Remove a claim from the README
if it can no longer be backed.

Conditions for production rows: Mac mini M4 (10 cores, 16 GB), macOS 26.6.2, launchd services
`sentinel-server` (builds `2adbf88f`, then `d7ee95c4` from 2026-10-21 01:17 EDT) and
`sentinel-capture` (running since 2026-10-17), seven products, journal on a USB SSD.
Metrics: VictoriaMetrics on `127.0.0.1:8428`, Prometheus scrape interval 15 s
(`ops/monitoring/prometheus.yml:4`). `Q(expr)` means
`curl -s 127.0.0.1:8428/api/v1/query --data-urlencode 'query=<expr>'` evaluated at
2026-10-21 about 17:05 EDT; `[7d]` windows therefore end there. CPU rates are cores (1.0 = one
core).

## Performance

| README claim | Evidence | Measured |
| --- | --- | --- |
| Capture CPU median 3.7 %, p95 5.2 % | `Q(quantile_over_time(0.5, rate(process_cpu_seconds_total[5m])[7d:5m]))` capture 0.0375; same with 0.95: 0.0516 | 2026-10-21 |
| Capture resident memory median 27 MB | `Q(quantile_over_time(0.5, process_resident_memory_bytes[7d]))` capture 27,344,896 B | 2026-10-21 |
| Server CPU median 5.2 %, p95 9.1 % | same CPU queries, server 0.0522 and 0.0908 | 2026-10-21 |
| Journal about 82 frames/s | `Q(sum(rate(sentinel_capture_stored_frames_total[7d])))` 82.26 (1 h: 83.4) | 2026-10-21 |
| Journal 0.98 GB/day on disk | `Q(sum(increase(sentinel_capture_file_bytes_total[7d]))/7)` 983,031,477 B (last 24 h: 1,006,566,879) | 2026-10-21 |
| Live feed availability 99.96-99.98 % per product | `Q(avg_over_time(sentinel_live_feed_up[7d]))` per product 0.99955 (BTC) to 0.99975 (DOGE) | 2026-10-21 |
| Main-thread queue wait: per-minute p99 median 0.08 ms | `Q(quantile_over_time(0.5, sentinel_server_event_loop_lag_ms[7d]))` series `quantile="0.99"`: 0.0796 ms (gauge = 60 s window, `ops/monitoring/README.md:127`) | 2026-10-21 |
| Worst single wait 53 ms | `Q(max_over_time(sentinel_server_event_loop_lag_ms[7d]))` series `quantile="max"`: 52.73 ms | 2026-10-21 |
| No wait over 100 ms | `Q(sum(increase(sentinel_server_event_loop_late_ticks_total[7d])))` 0 (counter = probes over 100 ms) | 2026-10-21 |
| 0 mismatching buckets in about 168 hourly comparisons per product | `Q(sum(increase(sentinel_roller_shadow_mismatch_total[30d])))` 0; persisted `max by (product)` 0 for all 7; `Q(changes(sentinel_roller_shadow_last_comparison_timestamp_seconds[7d]))` 167-170 per product | 2026-10-21 |
| Restart to serving 2.6-6.7 s (3 restarts, 19-21 Oct) | `Roller serving ready products=7` vs log header `started=`: `sentinel-server-20261019-005034-20906.log` 00:50:34.041 to 00:50:36.685 (2.6 s); `-20261020-122756-60222.log` 12:27:56.690 to 12:28:03.390 (6.7 s); `-20261021-011717-53893.log` 01:17:17.632 to 01:17:20.597 (3.0 s), in `~/Library/Logs/Sentinel/` | 2026-10-21 |
| 30 min of 100 ms and 1 s candles rebuilt for all 7 products, slowest product 1.1-4.8 s | Same logs, `Candle history ready ... restored=complete` for 7/7 products in each run, `bars100` 17,981-17,999, `bars1000` 1,798-1,799; largest `readMs` per run 1131.18, 4834.51, 2038.25 (BTC-USD each time) | 2026-10-21 |
| Fan-out append to client receive p50 93-99 µs, p95 150-157 µs, worst 1.0-2.4 ms (synthetic) | `scripts/dev/build-queue.sh --label lt-claude/readme-refresh -- ./build/mac-clang/tests/capture/capture_fanout_benchmark client 65`, 3 runs at `70aa0a0c` (target built with `-j 6`): p50 95.5/92.7/99.3 µs, p95 156.9/150.0/149.5 µs, max 997/2390/1836 µs; 6,501 of 6,501 records received each run; bench = 100 records/s of 1,500 B, one local socket client (`tests/capture/fanout_benchmark.cpp`) | 2026-10-21 |
| 116 CTest tests pass with a real GPU required (Metal) | `~/Library/Logs/Sentinel/land/20261021T050812Z-lt-claude-recorder-resnapshot-34817/ctest.log`: 116/116, `GPU case skipped` count 0; land runs ctest with `SENTINEL_REQUIRE_GPU=1` (`scripts/dev/agent-worktree.sh:463`) | 2026-10-21 |

## Behaviour and features

| README claim | Evidence |
| --- | --- |
| Version `3.0.0-alpha` | `sentinel_build_info{version="3.0.0-alpha"}` for server and capture; log header `version=3.0.0-alpha` |
| Records seven products around the clock | `config/server_config.yaml:48` `roller_shadow.products` (7); `Q(count(sentinel_live_feed_up))` 7 |
| One WebSocket per product; level 2, trades, heartbeats; no API key | `docs/RAW_CAPTURE.md:3-9`; `libs/core/capture/CaptureApp.cpp` `key-file` "public channels need no key" |
| Compressed, checksummed journal | `--zstd-level` default 3 (`CaptureApp.cpp`); CRC in `libs/core/capture/RawCapture.cpp:38` |
| Local socket streams records to the server | `CaptureApp.cpp` `fanout-socket`; `libs/core/roller/ShadowRoller.cpp:848` |
| Server's only live source is the journal | `docs/CONFIG.md:105-106` and `config/server_config.yaml:41` |
| One-minute columns, hourly rollups, gaps stored as gaps | `ops/monitoring/README.md:114-115`; `Q(sum(increase(sentinel_recorder_columns_written_total[24h])))` 20,160 = 7 products x 2 layers x 1,440; `docs/ARCHITECTURE.md` (`bucketState` `Gap`) |
| Hourly self-check from the same journal, alerted | `ops/monitoring/README.md:348-353`; alert `sentinel-roller-shadow-mismatch` in `ops/monitoring/grafana/provisioning/alerting/rules.yaml:324` |
| Resnapshot request with grace, liveness and backoff | `libs/core/roller/ResnapshotPolicy.hpp:8-23`; `ShadowRoller.cpp:575-628`; merge `d7ee95c4` on `main` |
| Restart rebuild of 30 min of 100 ms and 1 s candles | `config/server_config.yaml:6` (`history_bars {100: 18000}`); startup logs above |
| Prometheus on 127.0.0.1:8090 and :8091 | server log `Health and metrics endpoint listening on 127.0.0.1:8090`; `CaptureApp.cpp` `metrics-port` 8091 |
| GPU binning equals the CPU reference exactly | `tests/render/test_heatmap_gpu_binner.cpp:367` `HeatmapGpuParity.SyntheticMatchesBinColumnExactly`; passed in the land log above |
| Candle, Hollow, Line | `libs/gui/widgets/TopToolbar.cpp:206` |
| Timeframes 100 ms to 1 d | `config/server_config.yaml:5`; all are served (`libs/core/protocol/SentinelStreamServer.cpp:132-142`) |
| 1 m and longer from Coinbase in pages of 350, cached by the server | `libs/gui/datasources/CandleBackfillState.hpp:85`; `CoinbaseRestClient.cpp:363`; `CandleHistoryCache` in `CoinbaseRestClient.hpp:22-25` |
| Footprint, TPO, Volume Profile, trade bubbles; MSDF text | `docs/ARCHITECTURE.md` (`registerChartLayers`: heatmap, footprint, VP, TPO, text; `TradeBubbleFeed`) |
| Metal; D3D12 or Vulkan on Windows; D3D11 refused | `docs/WINDOWS_GPU_TESTS.md:15-21` |
| DOM, Stock Chart, SEC insider overlays, screener, watchlists | `libs/gui/widgets/OrderBookDock.cpp`, `StockChartDock.cpp`, `SecFilingDock.cpp`, `ScreenerDock.cpp`, `WatchlistDock.cpp` |
| Hotkeys B, S, F, C | `libs/gui/TradeInputManager.cpp:18-21` |
| Backtest tab, AvendellaMM | `libs/gui/widgets/PaperTradingDock.cpp:143-145, 292` |
| Agent API on 127.0.0.1:17100, no authentication | `docs/AGENT_API.md:3`; `docs/TODO.md` Security |
| Stream on 127.0.0.1:8080 without client auth | `config/server_config.yaml:2`; server log `streamPort=8080`; `docs/CONFIG.md:24-25` |
| `SentinelStreamServer start failed` without TLS pair | `docs/CONFIG.md:18-20`; `SentinelStreamServer.cpp:1739` |
| Only release is `v2.1.0-alpha` (March 2026) | `gh release list -R pattty847/Sentinel`: one pre-release, 2026-03-06 |
| Build commands and binary paths | `CMakePresets.json` presets `mac-clang`, `windows-msvc-vs`, `linux-gcc`; output names in `apps/*/CMakeLists.txt` |
| Screenshots are from a March 2026 build | `git log -- docs/assets/gifs`: added `b1fb997f` 2026-03-13; frames dated Mar 12 |

Not measured, so not claimed: client frame rate or frame time (no repeatable GUI benchmark), server
resident memory (macOS RSS in this window ranged from 80 MB to 1.5 GB and needs a separate look),
candle late-trade counts.
