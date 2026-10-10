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
curl -s 'http://127.0.0.1:8428/api/v1/query_range' --data-urlencode 'query=increase(sentinel_roller_invalidations_total[1h])' \
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
| `sentinel_recorder_running` | gauge | - | 1 when recording is served: primary recorder started or serving roller attached. |
| `sentinel_recorder_last_column_timestamp_seconds` | gauge | product, layer | Start of the newest committed minute column. Absent until the first column. Age = `time() - x`; normal age is 60-125 s. |
| `sentinel_recorder_column_overdue_seconds` | gauge | product, layer | Seconds that the next column is past due, by the stall monitor's deadline: last column bucket (or the connect minute) + 2 min + lateness. 0 means on time. 60 is when the log warns `Recording v2 stalled`. Absent while that product's upstream is disconnected. |
| `sentinel_roller_invalidations_total` | counter | product | Journal book invalidations counted at ShadowRoller journal invalidation handling; retained across daily recorders/retries, resets with the process. |
| `sentinel_roller_write_errors_total` | counter | product | Failed HMC2 appends (minutes and hourly rollups), counted once at the append failure; excludes setup, checkpoint and socket errors (see shadow setup failures). Retained across daily recorders/retries, resets with the process. |
| `sentinel_recorder_columns_written_total` | counter | - | Serving roller: committed minute appends across products/layers, excluding rollups and checkpoint replay. Primary mode: BookRecorder appends (including rollups). Process lifetime; present at zero before first commit. |
| `sentinel_recorder_invalidations_total` | counter | - | Primary mode only (absent in roller mode): Recorder book invalidations (from upstream and from the recorder itself). |
| `sentinel_recorder_queue_drops_total` | counter | - | Primary mode only (absent in roller mode): Book messages dropped (or turned into an invalidation) when the recorder queue overflowed. |
| `sentinel_recorder_disk_errors_total` | counter | - | Primary mode only (absent in roller mode): Recorder disk write failures. |
| `sentinel_recorder_late_events_total` | counter | - | Primary mode only (absent in roller mode): Book messages timestamped before the minutes that are already closed. |
| `sentinel_recorder_backward_steps_total` | counter | - | Primary mode only (absent in roller mode): Book messages whose timestamp went backwards. |
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

## Shadow roller (server :8090)

Opt in with `roller_shadow.enabled` in the server YAML. Defaults are off;
`journal_dir` is the RAWL2 root, `dir` is the separate HMC2 shadow root
(deployment: `/Volumes/T7/sentinel-data/hmc2`), `socket` defaults to
`~/Sentinel-runtime/run/capture.sock`, and `from` is a required UTC-midnight
ISO timestamp for the first journal day. Set the start deliberately before
turning it on. Existing per-day `roller.json` checkpoints retain the replay
policy/range and select the first incomplete day. No primary path or client
wire capability changes. The output must be disjoint from the journal and
all actual/configured primary/fallback roots, including symlink aliases.
`fault_min_duration_ms` defaults to 120000 and must be positive. It is the
minimum elapsed time for an identical-failure streak before slow probes begin.

| Metric | Type | Labels | Meaning |
| --- | --- | --- | --- |
| `sentinel_roller_journal_corrupt_blocks_total` | counter | product | Distinct (run, segment, offset) payload, header, framing or index failures encountered by journal readers in this process, including anchor searches and batch comparisons. Replays do not count the same block again. |
| `sentinel_roller_shadow_running` | gauge | product | 1 while applying durable records; 0 during setup/retry/stop. Present only when enabled. |
| `sentinel_roller_shadow_lag_seconds` | gauge | product | Scrape-time age of last applied durable record, -1 before the first record. |
| `sentinel_roller_shadow_records_applied_total` | counter | product | Applied durable records, including deterministic restart/day warmup replay. |
| `sentinel_roller_shadow_setup_failures_total` | counter | product | Setup, socket framing, continuity or write failures that trigger backoff. Malformed journal payloads invalidate and continue like batch. |
| `sentinel_roller_shadow_fault_cooldown` | gauge | product | 1 after at least three identical consecutive faults spanning `fault_min_duration_ms` without new committed progress; 0 after new durable checkpoint progress or a different fault. |
| `sentinel_roller_shadow_fault_cooldowns_total` | counter | product | Entries into the persistent-fault slow probe loop. |
| `sentinel_roller_shadow_start_failures_total` | counter | - | Supervisor construction failure, such as inability to create worker threads. |
| `sentinel_roller_shadow_mismatch_total` | counter | product, layer | Persisted count of strict same-journal mismatching buckets, including one-sided missing and partial buckets. Series absent until checkpoint restore completes; restart does not re-count old hours. |
| `sentinel_roller_shadow_last_comparison_timestamp_seconds` | gauge | product | Completion time of last successful hourly report; 0 before the first. |
| `sentinel_roller_shadow_comparison_failures_total` | counter | product | Hourly oracle/report failures; failed hours retry at the next pass. |

The independent comparison worker runs hourly over fully committed completed
UTC hours, with temporary batch output and sequential product/hour processing.
It calls the same diff implementation as `hmc2_diff`. Strict live-vs-batch
mismatches increment the gate metric; comparing the independent primary
connection with shadow is an informational log report with the round-2 bands
(0.5% per-side total TWAP, 0.02% mids, 1% entry counts, 99% row overlap).
The pinned identical-input legacy timer fixture separately enforces the tier-2
32-code/1%-cells/0.01%-totals bounds. No cross-connection difference pages.

`sentinel-roller-journal-corruption` pages when the per-product corruption
counter increases over two hours, matching the shadow mismatch rule. It resolves
when that window contains no new counted damage. Repeated reads of the same
damaged region do not increment the counter again. Damage discovered before the
first scrape may not produce an observed increase; the error log still records it.
Read `Roller journal corrupt block` for product, file, offset and reason. The
roller skips corrupt payloads and framing using a validated closing index or a
bounded successor-header scan, and invalidates observation until a new exchange
snapshot. A damaged/missing index is treated as unsealed. `capture-verify` remains
strict. Preserve RAWL2 files; do not repair them in place. Product startup registers
`registerJournalMetrics(registry, product)` idempotently in the server registry.

Grafana provisions `sentinel-roller-shadow-mismatch` (increase over 2 h) and
`sentinel-roller-shadow-down` (product down for 5 min); absence is OK while
shadow is disabled. Existing primary health rules remain authoritative.
Read `Shadow roller retry` for the error and 1 s to 60 s exponential backoff.
After **three identical consecutive failures spanning at least two minutes by
default without a newer committed watermark**, that product probes once every
**ten minutes**, including storage faults and unavailable journal volumes.
`sentinel-roller-shadow-fault-cooldown` pages after the gauge remains set for
another **two minutes**. Replaying the old checkpoint does not reset
the streak. Socket/retract/EOF recovery keeps the applied durable book in memory
and resumes the journal at its exclusive applied cursor. Recorder failures and
process restarts reconstruct from the first incomplete day's anchor with the
persisted commit floor; resuming a stateless book at a position alone is invalid.
No additional book-state snapshot format is introduced.

`<shadow>/<product>/comparison.json` atomically persists the exclusive compared
hour, both mismatch totals, start-day identity and report completion time. Both
layers must finish before that transaction; metrics publish only after it is
durable. Restarts restore totals without exposing a transient zero. Already
compared hours are skipped even if a later run changes those historical files;
use a fresh shadow root for an intentional full re-audit. Do not delete this
checkpoint to silence alerts. If a previously observed comparison checkpoint
disappears, the checker retains its totals and reports a comparison failure
until the checkpoint is restored; it does not start a fresh audit.

The hourly oracle remains a fresh deterministic batch from the day's anchor:
24 hourly comparisons scan roughly **12.5 complete days of records per product
per day** (1 + 2 + ... + 24 hours), plus any pre-midnight anchor warmup. With
seven products this is roughly 87.5 product-day scans/day. Product comparisons
are sequential on their own thread, but CPU and storage bandwidth still compete
with other processes. Incremental oracle state is deferred: the current batch
checkpoint stores output progress, not the reconstructed book/TWAP state, so
keeping only its checkpoint would not remove the daily warmup cost.

`roller_shadow.products` lists the rolled products (default: the server's
`default_symbols`). A listed product with no RAWL2 file whose header
`product_metadata.product_id` matches under `journal_dir/<product>` is refused
at startup: an `E` line `Roller refused product=...`, one
`setup_failures_total` increment, `running 0`, lag -1, and no worker. An
unmounted `journal_dir` is not a refusal; the worker retries as usual.

### Serving (`recording.source: roller`, slice D-a)

`recording.source` is `primary` (default: this process's BookRecorder writes
`recording.dir`) or `roller`. With `roller` (requires `recording.enabled` and
`roller_shadow.enabled`) no primary recorder is created: chunks, availability,
`recording.available` and the live minute come from `roller_shadow.dir` and
the roller. Committed minutes are published from the history recorder, which
still applies only durable fan-out prefixes. The forming minute comes from a
**live lead**: a non-persisting fork of the day's history state that also
applies each provisional fan-out record as it arrives and ticks every 250 ms
like the primary. A retract, disconnect, EOF, socket failure or day end drops
the lead and withdraws every provisional minute it published from the live
cache (`LiveService::retractProvisional`; committed minutes stay). The next
raw-tail frame omits them, and the client drops omitted provisional minutes
(`LiveEdge`); when no provisional minute is left the frame resends the newest
final so it still reaches the client. A lead therefore publishes only while
the live cache holds a committed minute of both layers: after a restart (whose
replay republishes no final) it seeds the newest persisted minute from the
served root, and on a root with none yet it waits for the first commit
(`Roller live lead waits for a committed minute`). The next fork republishes the forming
minute from durable history. The legacy-renderer page path cannot withdraw a
column a client already holds (only a new subscription is clean).
The lead is forked once per start, per UTC day (about 2 s without a forming
minute at 00:01 UTC while the new day replays from its anchor) and per
provisional discard.

| Metric | Type | Labels | Meaning |
| --- | --- | --- | --- |
| `sentinel_roller_shadow_live_lead_forks_total` | counter | product | Live leads forked from durable history (serving only). Steady growth beyond one per day means repeated provisional discards: read `Shadow roller retry`. |
| `sentinel_recorder_running` | gauge | - | 1 when recording is served: primary recorder started or serving roller attached. |
| `sentinel_recorder_last_column_timestamp_seconds`, `sentinel_recorder_column_overdue_seconds` | gauge | product, layer | Same series, from the roller's committed watermarks for `roller_shadow.products`; overdue is present while that product's worker is running. |
| `sentinel_recorder_live_publish_drops_total` | counter | - | Roller publications the live cache refused. |

The columns-written counter is exported in both serving modes. The primary-only
late-event, backward-step, queue-drop, invalidation and disk-error counters are
absent while the roller serves. Dashboards use `sentinel_roller_invalidations_total`
and `sentinel_roller_write_errors_total` instead; late/backward targets have no
roller equivalent, and blocking recorder admission has no queue-drop equivalent.
The recorder columns/minute query is unchanged. Alert provisioning currently
references none of these six counters; no alert expressions need changing.
The cross-connection comparison still reads `recording.dir` (informational;
it stops growing after the flip). Served watermarks never move back across
the daily anchor replay.

`sentinel-roll` refuses a roller-served `roller_shadow.dir` for unscoped
batch, judged on each config file and on their merge (the private
`config/.server_config.yaml` overrides per key, as in the server). `--product-lease` takes the shared root lease plus the product's
exclusive lease (INV-115): it may repair a product the live roller is not
rolling, and fails on the product lock for one it is rolling.

Startup lines: `Roller started ... mode=shadow`, or `mode=live` when serving
(diagnostic only: it precedes the checkpoint policy check and the leases).
When serving, `Roller writer open product=...` follows each product's history
writer (diagnostic). A product is healthy (`Roller writer healthy`) from the
first durable checkpoint of that writer, which follows a committed minute at
the next minute boundary, until any failure or the writer closing (also each
UTC midnight): then `Roller serving not ready product=... reason=...`.
`Roller serving ready products=N` is logged whenever all N configured products
are healthy at once; a refused product, a held lease, a policy mismatch or a
failing checkpoint keeps it absent. `deploy-runtime.sh` accepts `Recording v2
started`, or a roller log whose latest readiness line is `ready`, read once
from the regular (non-symlink) log file named for the PID launchd reports
after the restart (never the replaced PID), headed with that PID and `exe=`
the deployed binary, and only if launchd still reports that PID after the
read. The server window is 150 s (capture 60 s): readiness needs the catch-up
and the next minute boundary after a commit. `deploy-runtime.sh
check-server-log <log dir> <pid> <exe>` runs the same log check. Probe `roller.live` logs every forming-minute publication
with `ageMs` (now minus bucket start plus observed time) and the lowest
native price in it.
