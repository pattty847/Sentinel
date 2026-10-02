# Sentinel monitoring

Metrics, dashboards and phone alerts for the always-on services. The plan and the owner
decisions are in `docs/research/2026-10-observability.md`. Logs remain the detailed view
(`AGENTS.md` section 4a). Metrics answer the quick questions: is the service up, is it
recording, and did anything drop.

## What runs

| Service | launchd label | Listens | Data | Logs |
|---|---|---|---|---|
| sentinel-server `/metrics`, `/ping` | `com.sentinel.recorder` (already there) | `127.0.0.1:8090` | - | `~/Library/Logs/Sentinel/sentinel-server-latest.log` |
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
- sentinel-capture `/metrics` on `127.0.0.1:8091` comes in slice 2. Its scrape job is
  commented out in `prometheus.yml` until then.
- The `/metrics` port follows `SENTINEL_HEALTH_PORT` (default 8090), the same as `/ping`.
  There is no config key.

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
| `sentinel_recorder_column_overdue_seconds` | gauge | product, layer | Seconds that the next column is past due, by the stall monitor's deadline: last column bucket (or the connect minute) + 2 min + lateness. 0 means on time. 60 is when the log warns `Recording v2 stalled`. Absent while the upstream is disconnected. |
| `sentinel_recorder_columns_written_total` | counter | - | Committed minute columns, all series. |
| `sentinel_recorder_invalidations_total` | counter | - | Recorder book invalidations (from upstream and from the recorder itself). |
| `sentinel_recorder_queue_drops_total` | counter | - | Book messages dropped (or turned into an invalidation) when the recorder queue overflowed. |
| `sentinel_recorder_disk_errors_total` | counter | - | Recorder disk write failures. |
| `sentinel_recorder_late_events_total` | counter | - | Book messages timestamped before the minutes that are already closed. |
| `sentinel_recorder_backward_steps_total` | counter | - | Book messages whose timestamp went backwards. |
| `sentinel_recorder_live_publish_drops_total` | counter | - | Live publications that were refused (series limit or stale). |
| `sentinel_mdc_connected` | gauge | - | 1 while the upstream market-data transport is up. |
| `sentinel_mdc_transport_up_total` / `_down_total` | counter | - | Upstream up and down transitions. Reconnects = up - 1. |
| `sentinel_mdc_ws_latency_ms` | gauge | - | Latest Coinbase WebSocket latency (server time minus exchange timestamp). |
| `sentinel_exchange_clock_offset_ms` | gauge | - | Smoothed local clock minus exchange clock. 0 means not yet measured. |
| `sentinel_stream_sessions` | gauge | - | Open client stream sessions (GUIs). |
| `process_resident_memory_bytes` | gauge | - | RSS. |
| `process_cpu_seconds_total` | counter | - | User + system CPU seconds. |
| `process_start_time_seconds` | gauge | - | Time when the metrics were set up at startup. Restarts = `changes(x[1h])`. |
| `sentinel_build_info` | gauge | version | Always 1. |

Product label values are the pinned default symbols as the server subscribes them
(upper case). Layers are `near` and `deep`.

Threading: the HTTP listener and the scrape-time samplers run on the server's main thread.
That thread already reads `BookRecorder::stats()` and `watermarks()`. The recorder worker
and the market-data I/O thread only do relaxed atomic stores. No new lock, allocation or
signal is on a hot path.

## Alerts (Grafana, to ntfy)

| Alert | Fires when | For |
|---|---|---|
| A1 recorder stalled | `max by (product,layer) (sentinel_recorder_column_overdue_seconds) > 60` (only while connected) | 1 m |
| A1b recorder upstream disconnected | `sentinel_mdc_connected < 1`. A1 is silent by design while disconnected, so this alert covers that gap. | 5 m |
| A3 service down | `up{job=~"sentinel-server\|node"} or (absent(up{job="sentinel-server"}) - 1) or (absent(up{job="node"}) - 1)` is below 1. This gives one sample per required job: a failed scrape (up 0) and a job whose `up` series is missing (absent - 1 = 0) both fire for that job. When VictoriaMetrics does not answer, Grafana sends its DatasourceError notification. | 2 m |
| A3b T7 absent | `absent(node_filesystem_avail_bytes{mountpoint="/Volumes/T7"})` | 5 m |

The rules are in `grafana/provisioning/alerting/rules.yaml`. The contact point and policy
are in `contact-points.yaml`: one ntfy webhook, an inline ntfy template (`template=yes&title={{.title}}&message={{.message}}&priority=high`; the built-in `template=grafana` drops the priority, and iOS hides default-priority pushes), grouped by alert
name, repeated every 4 h while an alert fires. Grafana expands environment variables in
provisioning files. For this reason the annotation templates use `{{ .Labels.x }}` and do
not use `$labels`.

A2 (per-product L2 silence) and A4 (capture queue, T7 below 50 GB, disk errors) come
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
