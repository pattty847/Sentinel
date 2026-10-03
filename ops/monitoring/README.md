# Sentinel monitoring

Metrics, dashboards and phone alerts for the always-on services. The plan and the owner
decisions are in `docs/research/2026-10-observability.md`. Logs remain the detailed view
(`AGENTS.md` section 4a). Metrics answer the quick questions: is the service up, is it
recording, and did anything drop.

## What runs

| Service | launchd label | Listens | Data | Logs |
|---|---|---|---|---|
| sentinel-server `/metrics`, `/ping` | `com.sentinel.recorder` (already there) | `127.0.0.1:8090` | - | `~/Library/Logs/Sentinel/sentinel-server-latest.log` |
| sentinel-capture `/metrics`, `/ping` | `com.sentinel.capture` (already there) | `127.0.0.1:8091` | - | `~/Library/Logs/Sentinel/sentinel-capture-latest.log` |
| VictoriaMetrics (scrapes every 15 s, keeps 1 year) | `com.sentinel.metrics` | `127.0.0.1:8428` | `~/Sentinel-runtime/monitoring/vmdata` | `~/Library/Logs/Sentinel/monitoring-victoriametrics.err` |
| Grafana (dashboards, alerting) | `com.sentinel.grafana` | `127.0.0.1:3000` | `~/Sentinel-runtime/monitoring/grafana/` | `~/Library/Logs/Sentinel/monitoring-grafana.err` |
| node_exporter (CPU, memory, filesystems, disk I/O, load) | `com.sentinel.node-exporter` | `127.0.0.1:9100` | - | `~/Library/Logs/Sentinel/monitoring-node-exporter.err` |

- Every service listens on 127.0.0.1 only and has no auth. Do not put secrets, peer
  addresses or paths in labels.
- The server endpoint accepts at most 8 open connections. A local flood of unfinished
  requests can hold all 8 sockets, so a scrape is refused while the flood lasts (each
  socket is closed 5 s after accept). This is accepted, because the endpoint is
  loopback-only and A3 pages on a failed scrape.
- The data is on the internal disk, never on T7. The monitor must keep running when T7 is
  the component that failed.
- VictoriaMetrics reads `ops/monitoring/prometheus.yml`. Grafana reads
  `ops/monitoring/grafana/provisioning/` (datasource, dashboards, alerts) from the main
  checkout. A change in git takes effect after 60 s (scrape config), after 30 s
  (dashboards), or when Grafana restarts (alerts and datasource).
- The server's `/metrics` port follows `SENTINEL_HEALTH_PORT` (default 8090), the same as
  `/ping`. There is no config key. The capture's port is `sentinel-capture --metrics-port`
  (default 8091, 0 = no listener); the launchd plist does not pass it.
- `sentinel-capture` is a required A3 job. Its scrape job is active in `prometheus.yml`,
  so landing the change makes VictoriaMetrics scrape 8091 within 60 s. Deploy the capture
  build that serves `/metrics` right after landing, and only then restart Grafana so it
  loads the new rules (see "Capture metrics: first deploy" below). Until the deploy,
  `up{job="sentinel-capture"}` is 0 on the dashboard; it pages only after Grafana has
  loaded the new A3 rule.

## Install (orchestrator, with the owner's OK)

The install loads three new launchd agents. It does not touch the recorder or the
capture.

1. Land the branch. Build `main` (`cmake --build --preset mac-clang`), then deploy the
   server: `scripts/dev/deploy-runtime.sh server`. The deploy restarts the recorder, which
   leaves one short gap in the recording.
2. Check the endpoint: `curl -s 127.0.0.1:8090/metrics | head`.
3. Choose a random ntfy topic, for example `sentinel-$(openssl rand -hex 8)`. Subscribe to
   it in the ntfy phone app (server ntfy.sh). Give the topic to the install in one of two
   ways:
   - put `SENTINEL_NTFY_TOPIC=<topic>` in `ops/monitoring/ntfy.env` (gitignored; the
     script refuses the file if git does not ignore it), or
   - set `SENTINEL_NTFY_TOPIC=<topic>` in the environment of the install command.
4. Run `ops/monitoring/install.sh` from the main checkout on the internal disk. The script
   resolves physical paths and refuses a checkout or a `~/Sentinel-runtime` that is on
   `/Volumes` (for example an agent worktree on T7). The script does these steps:
   - It runs `brew install victoriametrics grafana node_exporter`.
   - It renders the plists into `~/Library/LaunchAgents` with umask 077 (mode 600 from
     creation, because the Grafana plist holds the ntfy URL).
   - It renders `grafana.ini` into `~/Sentinel-runtime/monitoring/grafana/`.
   - It runs `launchctl bootstrap` for each of the three agents.
   - It waits until all four checks pass: VictoriaMetrics health, node_exporter, Grafana
     health and `up{job="sentinel-server"} == 1`.

   Use `--no-brew` when the formulae are already installed. Use `--dry-run <dir>` to render
   and lint the plists without installing anything. A dry run always uses a dummy topic
   and never reads the real one.
5. Open http://127.0.0.1:3000. The home page is "Sentinel health". To edit, log in as
   admin/admin; Grafana asks for a new password at the first login.
6. Test the alert path. In Grafana, open Alerting > Contact points > ntfy > Test. The phone
   must get a message. Then stop node_exporter (not the recorder) to fire A3:
   `launchctl bootout gui/$(id -u)/com.sentinel.node-exporter`. Wait about 3 minutes for
   the "A3 service down" and "A3b T7 absent" messages. Then restore it:
   `launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.sentinel.node-exporter.plist`.
7. node_exporter reads the mount table (`getmntinfo`). It does not read files on T7, so
   macOS must not ask for Full Disk Access. Check this on the first run while the owner is
   at the Mac (FM-127 rule).

To remove the three agents: `ops/monitoring/install.sh --uninstall`. The data in
`~/Sentinel-runtime/monitoring` stays.

## How agents read metrics

```sh
# The instant value, straight from the process:
curl -s 127.0.0.1:8090/metrics | rg '^sentinel_recorder'
curl -s 127.0.0.1:8091/metrics | rg '^sentinel_capture'
# PromQL over history (VictoriaMetrics, Prometheus API):
curl -s 'http://127.0.0.1:8428/api/v1/query' --data-urlencode 'query=up'
curl -s 'http://127.0.0.1:8428/api/v1/query' \
  --data-urlencode 'query=max by (product,layer) (sentinel_recorder_column_overdue_seconds)'
curl -s 'http://127.0.0.1:8428/api/v1/query_range' --data-urlencode 'query=increase(sentinel_recorder_invalidations_total[1h])' \
  --data-urlencode "start=$(date -v-24H +%s)" --data-urlencode "end=$(date +%s)" --data-urlencode 'step=1h'
```

Ad hoc exploration: http://127.0.0.1:8428/vmui. Firing alerts:
`curl -s http://127.0.0.1:3000/api/prometheus/grafana/api/v1/alerts`. This endpoint may
need the admin login (`-u admin:<password>`); this is not verified yet.

## sentinel-server metrics (`GET 127.0.0.1:8090/metrics`)

Prometheus text format 0.0.4. A sample marked "absent when ..." is left out of that
scrape. VictoriaMetrics adds `job` and `instance` to every series.

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `sentinel_recorder_running` | gauge | - | 1 when recording v2 started in this process. The `sentinel_recorder_*` series below exist only when it is 1. |
| `sentinel_recorder_last_column_timestamp_seconds` | gauge | product, layer | Start of the newest committed minute column. Absent until the first column. Age = `time() - x`; normal age is 60-125 s. |
| `sentinel_recorder_column_overdue_seconds` | gauge | product, layer | Seconds that the next column is past due, by the stall monitor's deadline: last column bucket (or the connect minute) + 2 min + lateness. 0 means on time. 60 is when the log warns `Recording v2 stalled`. Absent while that product's upstream is disconnected. |
| `sentinel_recorder_columns_written_total` | counter | - | Committed minute columns, all series. |
| `sentinel_recorder_invalidations_total` | counter | - | Recorder book invalidations (from upstream and from the recorder itself). |
| `sentinel_recorder_queue_drops_total` | counter | - | Book messages dropped (or turned into an invalidation) when the recorder queue overflowed. |
| `sentinel_recorder_disk_errors_total` | counter | - | Recorder disk write failures. |
| `sentinel_recorder_late_events_total` | counter | - | Book messages timestamped before the minutes that are already closed. |
| `sentinel_recorder_backward_steps_total` | counter | - | Book messages whose timestamp went backwards. |
| `sentinel_recorder_live_publish_drops_total` | counter | - | Live publications that were refused (series limit or stale). |
| `sentinel_mdc_connected` | gauge | product, pinned | 1 while this product connection is up; pinned=`1` for recorder defaults, `0` for GUI-only feeds. |
| `sentinel_mdc_transport_up_total` / `_down_total` | counter | product, pinned | Transitions in this feed lifetime. Reconnects = up - 1 per product. |
| `sentinel_mdc_connections` | gauge | pinned | Admitted distinct products, including connecting feeds; pinned defaults and GUI-only products counted separately. |
| `sentinel_mdc_max_connections` | gauge | - | GUI-only connection cap (default 8); pinned products are exempt. |
| `sentinel_mdc_refused_total` | counter | product | Admission refusals; bounded LRU of eight most recently refused products. Counts reset after eviction. |
| `sentinel_mdc_ws_latency_ms` | gauge | - | Latest Coinbase WebSocket latency (server time minus exchange timestamp). |
| `sentinel_exchange_clock_offset_ms` | gauge | - | Smoothed local clock minus exchange clock. 0 means not yet measured. |
| `sentinel_stream_sessions` | gauge | - | Open client stream sessions (GUIs). |
| `process_resident_memory_bytes` | gauge | - | RSS. |
| `process_cpu_seconds_total` | counter | - | User + system CPU seconds. |
| `process_start_time_seconds` | gauge | - | Time when the metrics were set up at startup. Restarts = `changes(x[1h])`. |
| `sentinel_build_info` | gauge | version | Always 1. |

Connection product labels are pinned defaults plus currently active GUI products (upper case).
GUI connection series disappear on final unsubscribe/close, including their counters; a new
feed lifetime starts at zero. Refusal series retain only eight recent products, independently
of active feeds, so arbitrary refused names cannot grow registry memory. Recorder series
still name pinned products only. Layers are `near` and `deep`.

Threading: the HTTP listener and the scrape-time samplers run on the server's main thread.
That thread already reads `BookRecorder::stats()` and `watermarks()`. The recorder worker
and the market-data I/O thread only do relaxed atomic stores. No new lock, allocation or
signal is on a hot path.

## sentinel-capture metrics (`GET 127.0.0.1:8091/metrics`)

One upstream connection and one RAWL2 stream per product; one shared disk queue pool
(`--queue-mib`, default 512 MiB) with a per-product floor (`--queue-floor-mib`, default
2 MiB). The pool is accounting only: bytes are allocated as frames queue, so the steady
state is about 0 bytes.

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `sentinel_capture_feed_up` | gauge | product | 1 while the product's WebSocket is up. |
| `sentinel_capture_feed_down_seconds` | gauge | product | Seconds the product's WebSocket has been down. 0 while up. Counts from process start for a product that never connected. |
| `sentinel_capture_connection` | gauge | product | Established connection id, the same number as the RAWL2 record `connection`. Reconnects = id - 1. |
| `sentinel_capture_queue_bytes` | gauge | product | Bytes this product has queued for its disk worker. |
| `sentinel_capture_queue_used_bytes` | gauge | - | Bytes queued across all products. |
| `sentinel_capture_queue_pool_bytes` / `_floor_bytes` | gauge | - | The pool total and the per-product floor. |
| `sentinel_capture_stored_frames_total` | counter | product | WebSocket frames written to the product's RAWL2 files. A silent product is a flat line. |
| `sentinel_capture_file_bytes_total` | counter | product | Bytes written to the product's RAWL2 files. |
| `process_resident_memory_bytes`, `process_cpu_seconds_total`, `process_start_time_seconds`, `sentinel_build_info` | | | As for the server. |

Threading: the listener and the samplers run on the capture's main thread and read only
relaxed atomics. The ingest observer (mdc-io thread) writes the feed state; each disk
worker publishes its stored frames and bytes. A scrape never takes a session mutex and
never waits on the I/O thread, so it answers while T7 I/O is frozen (FM-127).

### Capture metrics: first deploy

1. Land the branch and build `main`.
2. Deploy the capture: `scripts/dev/deploy-runtime.sh capture` (owner at the Mac, FM-127).
3. Check: `curl -s 127.0.0.1:8091/metrics | rg '^sentinel_capture_feed_up'` shows one line
   per product, all 1 within about 10 s (connects are paced at 1/s).
4. Check VictoriaMetrics: `up{job="sentinel-capture"}` is 1 in vmui.
5. Restart Grafana to load the A3 change and the A4/A4b rules:
   `launchctl kickstart -k gui/$(id -u)/com.sentinel.grafana`.

## Alerts (Grafana, to ntfy)

| Alert | Fires when | For |
|---|---|---|
| A1 recorder stalled | `max by (product,layer) (sentinel_recorder_column_overdue_seconds) > 60` (only while that product is connected) | 1 m |
| A1b recorder upstream disconnected | `min by (product) (sentinel_mdc_connected{pinned="1"}) < 1`. A1 is silent by design while disconnected, so this alert covers that gap. | 5 m |
| A3 service down | `up{job=~"sentinel-server\|sentinel-capture\|node"} or (absent(up{job="sentinel-server"}) - 1) or (absent(up{job="sentinel-capture"}) - 1) or (absent(up{job="node"}) - 1)` is below 1. This gives one sample per required job: a failed scrape (up 0) and a job whose `up` series is missing (absent - 1 = 0) both fire for that job. When VictoriaMetrics does not answer, Grafana sends its DatasourceError notification. | 2 m |
| A3b T7 absent | `absent(node_filesystem_avail_bytes{mountpoint="/Volumes/T7"})` | 5 m |
| A4 capture product down | `max by (product) (sentinel_capture_feed_down_seconds) > 120`. The gauge is the down time, so there is no pending period. | 0 s |
| A4b capture queue backing up | `100 * sum(sentinel_capture_queue_used_bytes) / max(sentinel_capture_queue_pool_bytes) > 50`. The disk worker is behind (FM-127); the capture exits when the pool is full. | 2 m |

The rules are in `grafana/provisioning/alerting/rules.yaml`. The contact point and policy
are in `contact-points.yaml`: one ntfy webhook, an inline ntfy template (`template=yes&title={{.title}}&message={{.message}}&priority=high`; the built-in `template=grafana` drops the priority, and iOS hides default-priority pushes), grouped by alert
name, repeated every 4 h while an alert fires. Grafana expands environment variables in
provisioning files. For this reason the annotation templates use `{{ .Labels.x }}` and do
not use `$labels`.

A2 (per-product L2 silence) and the rest of A4 (T7 below 50 GB, recorder disk errors) come
later. See the plan, section 4.

## Add a panel or a metric

- Panel: edit the dashboard in Grafana (log in as admin). Grafana does not save UI edits
  to provisioned dashboards. Use Share > Export, and keep "Export for sharing externally"
  off. Then replace the file in `grafana/dashboards/` with the JSON. Keep the `uid` and use
  datasource uid `sentinel-vm`. Commit the file. Grafana reloads it within 30 s.
- Metric: register it at startup with `sentinel::metrics::MetricsRegistry`
  (`libs/core/metrics/MetricsRegistry.hpp`):
  - Use `counter()` or `gauge()` for a value that a hot path updates. The update is one
    relaxed atomic operation.
  - Use `counterFn()` or `gaugeFn()` for a value that is read at scrape time on the main
    thread. The sampler reads only atomics or state that the main thread owns. It never
    reads non-atomic state from another thread.
  - Add the metric to the table above, and to a dashboard if it matters.
- Scrape target: add a job to `prometheus.yml`. VictoriaMetrics reloads the file within
  60 s.


### Server per-product metrics deploy

Deploy the server via `scripts/dev/deploy-runtime.sh server` before the orchestrator reloads
Grafana alert provisioning. The old unlabeled `sentinel_mdc_connected` disappears; dashboards
and A1b now select `pinned="1"` and keep the product label. Check one connected gauge per
pinned product, `sentinel_mdc_max_connections 8`, and separate `sentinel_mdc_connections`
counts. A1 is unchanged. Test a GUI-only open/close: its three connection series disappear,
pinned counters stay unchanged, and the recorder keeps committing. At capacity, check the
`Feed refused` error, client status text, Agent API refusal object and refused counter.
No monitoring or recorder service is restarted by the agent implementing this change.

### Capture fan-out metrics (slice B)

The existing capture endpoint and scrape job expose these additional series;
no new listener, scrape job or monitoring service restart is required.

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `sentinel_fanout_running` | gauge | - | 1 while serving; 0 during setup backoff, stop or service failure |
| `sentinel_fanout_setup_failures_total` | counter | - | Setup/service failures; retries back off 30 s to 10 min while journaling continues |
| `sentinel_fanout_clients` | gauge | - | Connected sockets, at most 8 (includes pending handshakes) |
| `sentinel_fanout_queue_bytes` | gauge | client | Bounded pending wire bytes, including partial record and queue pointer; stable slot 0..7, zero when unused |
| `sentinel_fanout_disconnects_total` | counter | reason | `slow_client`, `ingress_overflow`, `peer_closed`, `protocol`, `shutdown`, `capacity`, `internal_error`, `handshake_timeout`, `malformed_ingress` |
| `sentinel_fanout_ring_bytes` | gauge | product | Retained wire bytes plus entry accounting; independent of disk QueuePool |
| `sentinel_fanout_ring_oldest_age_seconds` | gauge | product | Monotonic age of oldest retained publication; 0 when empty; sampled at least once per second |
| `sentinel_fanout_ingress_bytes` | gauge | product | Pending writer-to-fanout record/control bytes and object accounting; separate 32 MiB/product cap |
| `sentinel_fanout_ingress_drops_total` | counter | product | Record/control events refused by fanout ingress; forces socket resume, does not drop journal records |
| `sentinel_fanout_resume_hits_total` / `_misses_total` | counter | product | Retained cursor versus explicit journal catch-up |

All scrape reads are atomic. Client labels reuse bounded slots; they are not
permanent consumer identities and never contain peer addresses, paths or secrets.
Memory budgets charge shared wire records in each referring client queue, so
summing these metrics is a conservative accounting bound, not process RSS.
Watch any increase in `slow_client`, `ingress_overflow`, `internal_error` or ingress
drops during deploy. A resnapshot or an ordinary reconnect should not affect
another product's feed gauge or disk queue.

Grafana **Capture fan-out down** alerts when `sentinel_fanout_running < 1` for
5 minutes. Missing series do not trigger this rule: the service-down rule covers
capture scrape failure. Inspect the logged setup reason and repair the socket
path/permissions or listener conflict; capture retries without a service restart.
