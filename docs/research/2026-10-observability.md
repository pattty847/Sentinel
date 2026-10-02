# Observability: metrics, Grafana and alerts for the always-on services

Status: research and plan, 2026-10-02. Slice 1 is implemented on branch
`lt-claude/metrics-s1`; section 4a lists what changed against this plan.
Owner ask: quick-check Grafana visuals for server performance over time, dropped
messages and "something went wrong"; logs stay the agents' detail view.

## 0. What exists today (measured, not assumed)

- `sentinel-server` (launchd `com.sentinel.recorder`, pid 56386, RSS 48 MB) already has an
  HTTP listener: a hand-rolled `QTcpServer` on `127.0.0.1:8090` answering only `GET /ping`
  (`apps/sentinel-server/SentinelServerApp.cpp:43-76`, main thread). Port 8080 is the TLS
  WebSocket stream (Beast, `libs/core/protocol/SentinelStreamServer.cpp:743-775`): it does an
  SSL handshake then `ws_.async_accept`, so it cannot serve plain HTTP.
- `sentinel-capture` (launchd `com.sentinel.capture`, pid 56421, RSS 33 MB) has no listener
  at all (`rg listen libs/core/capture apps/sentinel-capture` finds nothing).
- Recorder counters: `BookRecorder::Stats {columnsWritten, lateEvents, backwardSteps,
  queueDrops, invalidations, diskErrors}` are relaxed atomics
  (`libs/core/servermodel/BookRecorder.hpp:70-73`, `.cpp:158`), logged once a minute from the
  main-thread timer (`ServerDataModel.cpp:212-217`). 1715 such lines in `launchd-recorder.err`.
  Last value: `columns=10 late=0 backward=3 queueDrops=0 invalidations=0 diskErrors=0`.
  `RecorderStallMonitor` (`RecorderStallMonitor.hpp`) checks `watermarks(symbol, layer)
  .lastColumnMs` once a second and warns `Recording v2 stalled` once a minute per series
  (`ServerDataModel.cpp:223-231`). It fired 12 times, all 2026-10-01 00:15-00:20 (FM-127).
  It cannot see FM-139 (columns kept committing with frozen content).
- Capture counters: `Capture stats:` once a minute (`libs/core/capture/CaptureApp.cpp:200-207`):
  last line `storedFrames=4317554 storedFrameBytes=4966433191 fileBytes=628213517 blocks=321948
  connections=10 queuedBytes=0` after ~14 h for 7 products. So ~85 frames/s, ~1.1 GB/day on
  disk, 10 connection generations. `Session::queuedBytes()` takes the session mutex
  (`CaptureSession.cpp:88`); the queue limit defaults to 64 MiB (`--queue-mib`) and overflow
  fails the session (exit 1, launchd restarts). The per-symbol plan moves this to a 512 MiB
  shared queue and a per-product stats line (`BTC-USD conn=3 frames=.. queued=.. up=`).
- Upstream feed: `MarketDataCoreEngine` keeps `ProductLiveness {lastLevel2Ms, failures,
  reconnectEscalations, snapshotAccepted, ...}` per product on the `mdc-io` strand
  (`MarketDataCoreEngine.hpp:185-197`); events are log lines only: transport UP/DOWN (`:86`),
  reconnect scheduled (`:275`), book invalidated (`:323`), provider error (`:374`), ack
  missing (`:383`), heartbeat stale (`:652`), snapshot still missing (`:679`), L2 silent
  (`:689`), resnapshot (`:706/711`). Coinbase latency is a callback logged every 10 s
  (`SentinelServerApp.cpp:165-178`, probe `ws.latency`).
- Warning census (`rg ' [WEF] '`): recorder log: 106 stream-session errors (GUI clients
  disconnecting; noise), 17 `Order book invalidated product=* reason=disconnected`, 7
  `Transport down`, 12 stalled. Capture log: 27 invalidated, 1 `Heartbeat stale` (21.4 s), 1
  `no inbound WS frames within 5s of handshake`. 11 transport DOWN events in the capture log.
- `Exchange clock offset out of range, ignored: offsetMs=10077` appears once per process
  start (`ServerDataModel.cpp:75-91`, guard is 10 s). `sntp time.apple.com` says the Mac
  clock is +20 ms, so the first exchange timestamp after connect is ~10 s old (snapshot
  build time), not a clock fault. Worth a gauge, not an alert.
- GUI: `HeatmapTelemetryDock` and `GET /api/v1/heatmap/state` already expose live data age
  p50/p95, publish-to-draw, frame p50/p95, GPU bytes (`docs/AGENT_API.md`). The GUI is not
  always on, so it is a later scrape target, not part of the health screen.
- Host: T7 is 931 GiB with 683 GiB free (27% used). 16 GiB RAM. No Grafana, Prometheus,
  VictoriaMetrics or Alloy installed.
- The temporary session watchdog is a shell loop tailing `Recording v2 stats: columns=` and
  kickstarting launchd; FM-127 showed its kickstarts cannot help when T7 I/O is frozen.

## 1. Metric catalogue

Prefix `sentinel_`. Labels: `product` (7-8 values), `layer` (near/deep), plus Prometheus
`job`/`instance`. Total cardinality ~120 series. "Source" is where the value already lives;
"new" means a new relaxed atomic. Rows marked [H] go on the one-screen health dashboard.

| # | Metric | Type | Labels | Source | Catches | Alert |
|---|--------|------|--------|--------|---------|-------|
| **Recorder health** |
| 1 [H] | `recorder_last_column_timestamp_seconds` | gauge | product, layer | `BookRecorder::watermarks().lastColumnMs` (read at `ServerDataModel.cpp:226`) | FM-121 self-invalidation stall, FM-127 frozen I/O, FM-138 | `time()-x > 240` while connected |
| 2 | `recorder_columns_written_total` | counter | - | `Stats.columnsWritten` | rate drop | - |
| 3 | `recorder_invalidations_total` | counter | - (per product: new) | `Stats.invalidations` (`BookRecorder.cpp:304`) | FM-121 resnapshot loops | `increase(1h) > 5` |
| 4 | `recorder_queue_drops_total`, `recorder_disk_errors_total` | counter | - | `Stats.queueDrops/diskErrors` (`.cpp:198,245`) | overload, T7 write failure | any increase |
| 5 | `recorder_late_events_total`, `recorder_backward_steps_total` | counter | - | `Stats.lateEvents/backwardSteps` (`.cpp:843-845`) | timestamp disorder after reconnect | - |
| 6 | `recorder_live_publish_drops_total` | counter | - | probe `recording.live.drop` (`ServerDataModel.cpp:184`) -> new counter | FM-137 cadence problems | - |
| **Upstream feed (per product)** |
| 7 [H] | `mdc_last_l2_timestamp_seconds` | gauge | product | `ProductLiveness.lastLevel2Ms` (strand) -> new atomic mirror | FM-139 silent product behind a healthy socket | `time()-x > 60` while connected |
| 8 [H] | `mdc_connected` | gauge 0/1 | product (after per-symbol plan; today one) | transport status callback (`MarketDataCoreEngine.cpp:86`) | outages, reconnect storms | `== 0` for 2 min |
| 9 | `mdc_transport_down_total`, `mdc_reconnects_total` | counter | product | new counters at `:86` and `:275` | 9 drops in 13 h (capture log) | `increase(1h) > 3` |
| 10 | `mdc_book_invalidated_total` | counter | product, reason | new at `:323` (reason: disconnected, sequence, one-sided, resnapshot) | FM-121 vs upstream causes | - |
| 11 | `mdc_silence_recoveries_total`, `mdc_snapshot_missing_total` | counter | product | new at `:689`, `:679` | per-product recovery churn (FM-139 guardrail working or spinning) | `increase(1h) > 2` |
| 12 | `mdc_ws_latency_ms` | gauge | - | `onLatency` callback (`SentinelServerApp.cpp:165`) | upstream delay (also feeds data age) | - |
| 13 | `mdc_frames_total` | counter | product | capture: `WriterStats.frames`; server: new at ingest | silent product is rate -> 0, visible as a line | - |
| 14 | `exchange_clock_offset_ms` | gauge | - | `m_exchangeOffsetMs` (`ServerDataModel.hpp:114`) | clock drift that would skew every age metric | abs > 2000 |
| **Capture** |
| 15 [H] | `capture_queued_bytes` and `capture_queue_limit_bytes` | gauge | - (per product after plan) | `Session::queuedBytes()`, `m_limit` | FM-127 (disk worker blocked -> queue grows before the process dies) | `> 50% limit` for 2 min |
| 16 | `capture_frames_total`, `capture_file_bytes_total`, `capture_blocks_total` | counter | product | `WriterStats` (`RawCapture.hpp:62`) | write rate, per-product silence | - |
| 17 | `capture_connection_generation` | gauge | product | `connection.load()` (`CaptureApp.cpp:205`) | reconnect count since start | - |
| 18 | `capture_failure_markers_total` | counter | product | new at `CaptureSession.cpp:246` | partial/incomplete archive | any increase |
| 19 | `capture_verify_exit_code`, `capture_verify_trade_gaps` | gauge | - | nightly `sentinel-capture --verify` (exit 0/2/3, `CaptureApp.cpp:45-46`) pushed by a launchd script to VM `/api/v1/import/prometheus` | silent archive corruption | `== 2` |
| **Disk / T7** |
| 20 [H] | `node_filesystem_avail_bytes{mountpoint="/Volumes/T7"}` | gauge | - | node_exporter (darwin `filesystem` collector) | T7 full or unmounted (series absent) | `< 50 GB` or absent 5 min |
| 21 | `node_filesystem_avail_bytes{mountpoint="/"}` | gauge | - | node_exporter | log dir / VM data dir full | `< 20 GB` |
| **Process resources** |
| 22 [H] | `up{job=~"sentinel-.*"}` | gauge | job | the scrape itself | process dead, crash loop, exit 75 (T7 unmounted) | `== 0` 2 min |
| 23 | `process_resident_memory_bytes`, `process_start_time_seconds`, `sentinel_build_info{version,built}` | gauge | job | `task_info` (macOS) / `/proc/self/statm` (Linux); log header values | leaks, restart loops (`changes(start_time[1h])`), stale binary after deploy | RSS > 2 GB |
| 24 | `node_cpu_seconds_total`, `node_memory_*` | counter/gauge | - | node_exporter (darwin `cpu`, `meminfo`) | 16 GB box under agent load (FM machine-load memory) | - |
| **GUI / live latency (later, GUI not always on)** |
| 25 | `gui_live_data_age_ms{q="p50|p95"}`, `gui_publish_to_draw_ms`, `gui_frame_ms{q}` | gauge | q | already in `/api/v1/heatmap/state`; expose `/metrics` on 17100 | FM-137 regression | p95 age > 1500 |

Health screen = rows 1, 7, 8, 15, 20, 22 (six panels: last column age max over series, per-product
L2 age, connected, capture queue, T7 free, up). Everything else is a second "detail" dashboard.

## 2. Collection: options compared, recommendation

| Option | Code change | Extra services, RAM (16 GB Mac) | Survives T7 unmount | Multi-node / Pi | Agents read |
|---|---|---|---|---|---|
| (a) native `/metrics` text endpoint, pulled | ~150 lines core + 2 routes | TSDB 50-150 MB + Grafana ~200 MB | yes (TSDB on internal disk) | scrape the Pi over LAN, or Pi `vmagent` remote_write | PromQL over HTTP, or `curl /metrics` |
| (b) scrape the stats log lines (Loki/Promtail or Alloy) | none | Loki 6-7 GB and VictoriaLogs 1.3 GB in a 500 GB/7 d bench [1]; Alloy ~30% more RAM than Prometheus [2] | yes | ship logs | LogQL/LogsQL; `rg` already works |
| (c) push to a TSDB from the process | ~150 lines + HTTP client in both processes | same TSDB + Grafana | yes | push works across NAT | PromQL |
| (d) SQLite/CSV sampler + Grafana SQLite plugin | shell sampler parsing logs | Grafana ~200 MB + plugin 4.0.6 (May 2026; path-bypass fixed in 4.0.4) [3] | yes | copy files | SQL |

Recommendation: **(a)**, served by the listener the server already has, scraped by
**single-node VictoriaMetrics** with its built-in scraper, viewed in **Grafana**, both installed
with brew and run by launchd.

Why not (b): the stats lines are once a minute, cumulative, and carry no per-product field;
the thing missing today (a silent product behind a healthy socket, FM-139) is not in any log
line until it is already a warning. A log store would add the heaviest component in this
table to recover less information than ten atomics. Logs stay logs; agents already read them
with `rg`. (VictoriaLogs for W/E lines in Grafana is an optional later add, not a dependency.)
Why not (c): push makes "no data" ambiguous (process dead or push failed?). Pull gives
`up{job}` for free, which is metric 22 and the alert that covers exit 75, crash loops and the
TCC freeze of the main thread. Why not (d): it still needs Grafana, still needs a sampler
that parses logs, and gives no `rate()`, no absence alerting and a plugin with a recent
security history.

Why VictoriaMetrics over Prometheus 3.15.0 (both one static binary, both `brew install`,
both have Apple Silicon bottles [4][5]): VM scrapes targets itself via `-promscrape.config`
(a `prometheus.yml`), accepts the Prometheus text format on `/api/v1/import/prometheus` (for
the nightly verify push, row 19), accepts `remote_write` (the Pi path), serves `/api/v1/query`
and `/api/v1/query_range`, and Grafana uses the plain **Prometheus datasource type** against
it [6]. Retention is one flag (`-retentionPeriod`, default 31 days [6]); at ~120 series every
15 s, "store everything" for years is well under 1 GB. Dashboards and PromQL are identical
either way, so switching later costs nothing. Honest limit: VM's docs give no RAM baseline
for tiny workloads (the published benchmarks are 1.3-4.3 GB RSS at production scale [7]); at
this size expect tens of MB, but measure after a day. Prometheus' own guidance is ~3-8 KiB
per series in the head [8], so either tool is ~50 MB here.

Layout:
- `/metrics` on `127.0.0.1:8090` (server, extend the existing `QTcpServer` route) and
  `127.0.0.1:8091` (capture, same helper class), plus `node_exporter` on 9100
  (darwin collectors `cpu`, `meminfo`, `filesystem`, `diskstats`, `loadavg` [9]).
- VictoriaMetrics data under `~/Library/Application Support/sentinel-metrics/` on the
  **internal** disk, never on T7: the monitor must keep running while T7 is the thing that
  is broken. Scrape interval 15 s.
- launchd: one `com.sentinel.metrics.plist` for VM (brew's service block cannot take the
  `-promscrape.config` / `-retentionPeriod` flags), Grafana via `brew services start grafana`
  (13.2.3 in homebrew-core [10]) with `grafana.ini [paths] provisioning` pointed at the repo.
  Both are user agents like the recorder; a reboot without login stops all of them equally.
- Everything in git under `ops/monitoring/`: `prometheus.yml`, VM plist, Grafana
  datasource + dashboard provisioning JSON, alert rules, README. No clicking in the Grafana UI
  that is not then exported back to the repo.
- Agents and owner see one truth: `curl -s 'http://127.0.0.1:8428/api/v1/query?query=<promql>'`
  (and `query_range`), `vmui` at `:8428/vmui` for ad-hoc, `curl 127.0.0.1:8090/metrics` for the
  instant value. Add a 4c section to AGENTS.md with three example queries.
- Pi / second server later: same binary, same `/metrics`; either VM on the Mac scrapes it over
  LAN (`instance` label distinguishes) or the Pi runs `vmagent` with `remote_write` to the Mac.
  No design change now.

## 3. Alerting

Four alerts earn a phone notification; everything else is a dashboard colour.

| Alert | Expression (sketch) | Replaces / catches |
|---|---|---|
| A1 recorder stalled | `max by (product,layer) (time() - sentinel_recorder_last_column_timestamp_seconds) > 240 and on() sentinel_mdc_connected == 1` for 1 m | the shell watchdog; FM-121, FM-127, FM-138 |
| A2 product silent while connected | `time() - sentinel_mdc_last_l2_timestamp_seconds > 60 and sentinel_mdc_connected == 1` for 1 m | FM-139 (5 h 34 m frozen, no warning) |
| A3 service down or absent | `up{job=~"sentinel-.*"} == 0` for 2 m; `absent(node_filesystem_avail_bytes{mountpoint="/Volumes/T7"})` for 5 m | exit 75 crash loop, T7 unmounted, TCC freeze of the main thread |
| A4 capture backing up / disk | `sentinel_capture_queued_bytes > 0.5 * sentinel_capture_queue_limit_bytes` for 2 m; `node_filesystem_avail_bytes{T7} < 50e9`; `increase(sentinel_recorder_disk_errors_total[10m]) > 0` | FM-127 (queue grows before the process dies), T7 full |

Mechanism: Grafana's built-in alerting with a **webhook contact point to ntfy**; ntfy has a
built-in `template=grafana` (`X-Template: grafana`) that formats firing/resolved payloads [11],
so no relay process. Phone: the ntfy iOS/Android app on a random topic (on ntfy.sh the topic
name is the only secret) or a self-hosted ntfy. Desktop: the `ntfy subscribe` CLI as a launchd
agent running `osascript -e 'display notification ...'` per message, if wanted. Not vmalert:
it is a separate binary and routes through Alertmanager [12], a third service for four rules.
Not the shell loop: it reads one log line, cannot alert on absence, and FM-127 showed its
restarts do harm (they queue behind the same frozen I/O).

## 4. First slice, then later

First slice (one agent session, ~1-2 h of agent work, 15 min owner): a Grafana page with the
recorder half of the health screen plus alerts A1 and A3.
1. `libs/core/MetricsRegistry.{hpp,cpp}` (~120 lines, QtCore only): named counters/gauges
   over `std::atomic`, label support, `render()` to Prometheus text. Hand-rolled; no
   prometheus-cpp (3 libs + civetweb) and no need for prometheus-cpp-lite [13] for 25 metrics.
2. `SentinelServerApp.cpp`: add `GET /metrics` beside `/ping`. Register rows 1-6, 8, 9, 12, 14,
   22-23 (stream sessions count from `m_sessions.size()` is a free extra).
   Hot-path rule: nothing new in the recorder worker or the `mdc-io` strand except relaxed
   atomic stores that already exist as log sites; rendering runs on the main thread once per
   scrape. Row 7 (per-product L2 age) needs the atomic mirror and lands with the per-symbol
   connections branch, which rewrites that code anyway (see risk R1).
3. `ops/monitoring/`: `prometheus.yml` (8090, 8091, 9100 @15 s), `com.sentinel.metrics.plist`,
   Grafana provisioning (datasource, health dashboard JSON, alert rules A1/A3, ntfy contact
   point with the topic read from an env file that is gitignored), README with the four brew
   and launchctl commands.
4. Deploy with `scripts/dev/deploy-runtime.sh server` (signed binaries keep FDA since
   2026-10-01). One restart = one short recording gap; bundle it with the next planned deploy.
5. Verify: `curl 127.0.0.1:8090/metrics`, `up` is 1 in vmui, dashboard renders, A3 fires when
   the owner stops VM's target (not the recorder) in a test.

### 4a. Slice 1 as built (2026-10-02, branch `lt-claude/metrics-s1`)

The branch differs from items 1-5 above in these points:
- Code: `libs/core/metrics/` contains these parts:
  - `MetricsRegistry`: plain C++. A counter or gauge update is one relaxed atomic.
    Scrape-time samplers run in `render()`. No histogram yet, because no slice-1 metric
    needs one.
  - `MetricsHttpServer`: QtNetwork, 127.0.0.1 only, `/ping` and `/metrics`. It is the
    helper that the capture reuses in slice 2. `sentinel_core` now links `Qt6::Network`
    PRIVATE. This is not a GUI module, so the core rule holds.
  - `ProcessMetrics`.
- `SentinelServerApp`: the inline `/ping` handler is replaced by `MetricsHttpServer` on the
  same port (`SENTINEL_HEALTH_PORT`, default 8090). There is no new config key.
- Metric names that changed:
  - Row 1 also exports `sentinel_recorder_column_overdue_seconds{product,layer}`. This is
    the stall monitor's own deadline: last column bucket or connect minute, + 2 min +
    lateness. A1 uses it (`> 60` for 1 m) instead of `time() - last_column > 240`. The
    reason: the raw age misfires after a long disconnect and before the first column.
  - `last_column_timestamp_seconds` is absent until the first column.
  - Row 9 is `sentinel_mdc_transport_up_total` / `_down_total`. These are counted from the
    connection-status callback in `ServerDataModel`, because `libs/core/marketdata/` was
    not changed (feeds-core owns it). Reconnects = up - 1. There is no per-product label
    yet.
  - Free extras: `sentinel_recorder_running` and `sentinel_stream_sessions`.
- Alerts: A1, A3 and two additions:
  - A1b "upstream disconnected 5 min". A1 is silent while disconnected, so this alert
    covers that gap.
  - A3b "T7 absent".
  - A3 evaluates `up{job=~"sentinel-server|node"} or (absent(up{job="sentinel-server"}) - 1)
    or (absent(up{job="node"}) - 1)`, so a job whose `up` series is missing fires on its
    own. A plain `up{...}` query still returns data for the other job, so its no-data
    state would never trigger.
  - All four go to one ntfy webhook. The topic comes from `SENTINEL_NTFY_TOPIC` or the
    gitignored `ops/monitoring/ntfy.env`.
- Runtime layout:
  - The VictoriaMetrics data directory is `~/Sentinel-runtime/monitoring/vmdata`, not
    `~/Library/Application Support/sentinel-metrics/`.
  - Grafana runs from its own launchd plist (`com.sentinel.grafana`), not
    `brew services`. It needs `SENTINEL_REPO` and `SENTINEL_NTFY_URL` in its environment.
  - node_exporter is `com.sentinel.node-exporter`.
  - `ops/monitoring/install.sh` loads all three. The orchestrator runs it.
- TODO (with the per-symbol connections / feeds-core branch): row 7
  `sentinel_mdc_last_l2_timestamp_seconds{product}` from an atomic mirror of
  `ProductLiveness.lastLevel2Ms`. Make rows 8-11 per product. Then add alert A2 and its
  health panel.
- Slice 2: capture `/metrics` on `127.0.0.1:8091` (rows 13, 15-18) with
  `MetricsHttpServer`. It was not done in slice 1 because the per-symbol branch rewrites
  `CaptureApp` stats (R1). After it lands, enable the scrape job in
  `ops/monitoring/prometheus.yml` and the capture queue panel.

Later, in order: (b) capture `/metrics` with per-product rows 13, 15-18 (with or after the
per-symbol branch); (c) A2 and A4; (d) nightly verify push (row 19) via a launchd script;
(e) GUI `/metrics` on 17100 (row 25) and a second "live latency" dashboard; (f) VictoriaLogs
for W/E lines if the owner wants log search in Grafana; (g) Pi node.

## 5. Risks and open questions

R1 Branch conflict: the per-symbol connections work (owner-approved, `docs/research/2026-10-
per-symbol-connections.md`) rewrites `MarketDataCoreEngine`, `CaptureApp` stats and
`SentinelServerApp`. Metrics touching those files must wait for it or be confined to new files
plus `ServerDataModel.cpp`; the first slice above is confined on purpose.
R2 Thread safety on the always-on recorder: `/metrics` is served on the main thread, where the
stats timer already runs; it reads relaxed atomics and `watermarks()` (already called once a
second). `Session::queuedBytes()` takes the disk-worker mutex; one lock per 15 s is fine, but a
mirror atomic is cheaper and avoids any chance of a scrape waiting on a blocked disk thread
(exactly the FM-127 case). Never read the strand-owned `ProductLiveness` map cross-thread.
R3 No hot-path cost: every new counter is one relaxed increment at an existing log site; no
allocation, no signal, no string formatting outside the scrape.
R4 Network exposure: bind `127.0.0.1` only (same as `/ping` and the GUI API); no auth, so never
put key material, peer addresses or file paths with secrets into labels. ntfy messages carry
product names and counters only.
R5 TCC: node_exporter reads mount stats via `getmntinfo`, not files on T7, so no Full Disk
Access prompt is expected; verify on first run while the owner is at the Mac (FM-127 rule).
R6 Monitor on the same box: if the Mac is wedged, nothing alerts. Acceptable until the Pi
exists; the Pi then becomes the external `up` check for the Mac.
R7 Deploying the metrics build restarts the recorder (gap) and must go through
`deploy-runtime.sh`; never a bare `sentinel-server`.

Open questions for the owner (real decisions only):
1. ntfy.sh hosted (zero infra, random topic as the only secret) or self-hosted ntfy?
2. Retention and location: default 31 days, or `-retentionPeriod=10y` ("store everything",
   <1 GB) on the internal disk? (T7 is ruled out for the reason in section 2.)
3. Order: metrics first slice now (confined files) or after the per-symbol connections
   branch lands, paying one recorder restart for both?
4. Desktop notifications too (`ntfy subscribe` launchd agent), or phone only?

Sources: [1] https://www.truefoundry.com/blog/victorialogs-vs-loki ; [2]
https://github.com/grafana/alloy/issues/2047 ; [3]
https://github.com/fr-ser/grafana-sqlite-datasource/blob/main/CHANGELOG.md ; [4]
https://formulae.brew.sh/formula/prometheus (3.15.0) ; [5]
https://formulae.brew.sh/formula/victoriametrics (1.143.0; tap 1.152.0) ; [6]
https://docs.victoriametrics.com/victoriametrics/single-server-victoriametrics/ ; [7]
https://valyala.medium.com/prometheus-vs-victoriametrics-benchmark-on-node-exporter-metrics-4ca29c75590f ;
[8] https://www.robustperception.io/how-much-ram-does-my-prometheus-need-for-ingestion/ ; [9]
https://github.com/prometheus/node_exporter ; [10]
https://github.com/Homebrew/homebrew-core/pull/314219 ; [11] https://docs.ntfy.sh/publish/ ;
[12] https://docs.victoriametrics.com/victoriametrics/vmalert/ ; [13]
https://github.com/biaks/prometheus-cpp-lite/ ; Grafana idle ~200 MB:
https://last9.io/blog/grafana-memory-usage/

## Owner decisions (2026-10-02, approved)

1. Alerts via hosted ntfy.sh (random topic is the only secret; kept out of git).
2. Metrics retention 1 year on the internal disk.
3. Build the first slice now (confined files); the per-product L2 age metric arrives with the per-symbol connections work.
4. Phone notifications only for now.
5. Phase 2 (owner idea): an alert hook that launches a READ-ONLY Codex investigation (`codex exec -s read-only`, approval_policy never) with the alert, recent metrics and the relevant run logs, and sends a short root-cause summary to the same ntfy topic. It never restarts, deploys or edits anything; remediation stays with the owner/orchestrator.
