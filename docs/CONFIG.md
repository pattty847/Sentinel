# Sentinel configuration

Sentinel uses two YAML configs: server (authoritative for data and trading) and client (UI and rendering). Each has an optional override file that is not tracked in git.

## Files and load order

| Role | Default | Override (optional) |
|------|---------|---------------------|
| Server | `config/server_config.yaml` | `config/.server_config.yaml` (overrides default) |
| Client | `config/client_config.yaml` | `config/.client_config.yaml` (overrides default) |

Copy the defaults to the override names to customize; override values take precedence.

`server.bind_address` is a string that selects the stream listener's numeric IPv4 or IPv6
address (default `"127.0.0.1"`, port 8080). The existing flat server config uses the equivalent
top-level `bind_address` key; a wrapped config places it under `server:`. An invalid address
or an address not assigned to a local interface is validated in `apps/sentinel-server/main.cpp`
before `SentinelServerApp` is constructed; it logs an error and exits with code 1 before recording
or feeds start. Other stream listener failures (such as missing TLS files or a port already in use)
are logged and leave the process recording with no client listener. Nothing retries the listener: restart the service (`scripts/dev/deploy-runtime.sh restart server`) once the cause is fixed. Only the log shows the failure today.
For remote clients, set this to the Mac's Tailscale address and use Tailscale for access control.
For example, `"100.89.99.21"` is the owner's Mac tailnet address on 2026-10-07; it is an example,
not a default. Explicit `"0.0.0.0"` or `"::"` allows all interfaces and logs
`listening on all interfaces without client auth`. The stream has no client authentication.
This setting does not affect the Prometheus HTTP listener, which remains on `127.0.0.1:8090`.

## Ownership

**Server-authoritative (client cannot override):**

- Chart timeframes and trade-overlay retention/grid
- Order book tick size and band percent
- Candle gating
- Market data connection and TLS
- Default symbols
- Trading mode and paper slippage

**Client-only:**

- Visual tuning (gamma, contrast, labels, colors)
- GUI settings (API port, screenshot dir, font)
- Client cache sizing and local UI preferences

`orderbook.tick_size` remains the configured BTC-USD live-book tick (default 0.1 USD).
For other products the server derives a live-book tick when metadata and an accepted raw book are
both available, using the current raw BBO midpoint:
about one basis point, rounded to the nearest 1-2-5 step, then rounded up to a multiple of
Coinbase's exact `quote_increment`. Metadata comes from the capture journal header. Until it is valid,
the live aggregated book is unavailable; no global tick or guessed increment is used. The server
retains the bounded native-price snapshot and all in-band deltas while metadata is pending. When
metadata arrives, it builds the live aggregation from that current raw state without reconnecting
the upstream feed or interrupting recording. Actual upstream invalidation clears the raw state and
still requires the provider's next snapshot. The tick stays fixed for that aggregated book until
the next upstream snapshot, and both snapshots and deltas publish it. GUI replicas
use that wire tick unchanged for chart book-top lines, the Agent API and the DOM. `band_pct` remains
server-configured. The server sums raw price levels in each live tick bucket, including BTC-USD;
recorded HMC2 grids are independent of this live-book setting.

## Example snippets

**Server (`config/server_config.yaml` or `.server_config.yaml`):**

```yaml
stream_port: 8080
bind_address: "127.0.0.1"
# bind_address: "100.89.99.21"  # Example only: use the Mac's current local Tailscale address.
chart:
  timeframes_ms: [1000, 60000, 300000, 900000, 3600000, 14400000, 86400000]
  default_timeframe_ms: 60000
trade_overlays:
  retention_columns: 8192

recording:                # recording v2: near and deep order-book layers
  enabled: true
  source: roller           # required; primary has no feed and is refused at startup
  dir: /Volumes/T7/sentinel-data/recording # protected archival root, unused for serving
  fallback_dir: data/recording   # protected archival fallback root
  near_tick: 1              # $ rows within near_pct of the mid
  near_pct: 0.05
  deep_tick: 5              # $ rows across [mid*deep_low_frac, mid*deep_high_mult]
  deep_low_frac: 0.25
  deep_high_mult: 4
  live_publish_ms: 500      # open-minute live publication interval (2 Hz); clamped to [250, 5000]
  # advanced: price_scale (100), size_floor (1e-6), codes_per_octave (819), lateness_ms (2000)

roller_shadow:
  enabled: true
  journal_dir: /Volumes/T7/sentinel-data/raw-l2
  dir: /Volumes/T7/sentinel-data/hmc2
  products: [BTC-USD]

mdc:
  ssl_ca_bundle: resources/certs/ca-bundle.crt  # REST candle-history TLS
```

`recording.live_publish_ms` (default 500, clamped to [250, 5000]) is how often the recorder publishes each layer's open minute to live subscribers (owner decision 2026-10-01: 500 ms, 2 Hz). The live worker paces subscriptions at half of it, so each publication is sent at the next worker turn; refused sends back off from there up to 5 s. It does not change what is recorded on disk. Recorder-thread cost: about 0.4 ms per publication for a BTC-USD book (~23,500 near+deep entries), 0.08% of a core at 2 Hz (`recording_live_bench --publish`).

The roller derives each product's daily native grid and size scale from the journal
metadata and first valid two-sided snapshot (see [ROLLER.md](ROLLER.md)). The
retained `recording.near_tick`, `near_pct`, `deep_tick`, `deep_low_frac`,
`deep_high_mult`, `price_scale` and `lateness_ms` values do not configure the
roller. Display ticks remain native-grid multiples; incompatible historical
generations are represented as unknown rather than resampled onto an invalid grid.

The server's only live source is the capture journal through the roller. It requires
`recording.source: roller` and `roller_shadow.enabled: true`; `primary` is refused.
Subscriptions accept only captured products (`roller_shadow.products`, falling back
to normalized `default_symbols`); there is no GUI feed cap or upstream acquisition.

`ssl_ca_bundle` may appear beside or inside `mdc`, at the YAML root or under `server`.
The nested value overrides its sibling. All former WebSocket keys (`host`, `port`,
`target`, `use_jwt`, timeout keys, `max_connections`) are ignored.
There is no `recording.live_feed` setting; the capture journal is always the live source.
The hourly self-check compares the live roller with a re-roll of the same journal;
there is no independent primary recorder comparison.

Public market data (level2, market_trades, candles) does not require a key; the server runs without `key.json` by default.

**Client (`config/client_config.yaml` or `.client_config.yaml`):**

```yaml
heatmap:
  gamma: 1.05
  contrast: 1.15
  initial_column_px: 8        # time zoom on connect: screen pixels per heatmap column
  initial_price_pct: 5        # price zoom on connect: % of a 2048-row $1 band; 0 = full
  sensitivity_min: 0.05       # colour range in base units (log scale): <= min dark
  sensitivity_max: 50         # >= max brightest

gui:
  api_port: 17100
  screenshot_dir: ./screenshots
```

### Order Book dock

`OrderBookDock/lean` in the `Sentinel` / `SentinelTerminal` QSettings store is a boolean,
default `true`. The heading's Lean toggle shows four native-book imbalance bands at
quote-price distances 5, 15, 50 and 100 from mid, mirrored cumulative depth over 100,
and two minutes of 15-band history sampled about every 500 ms. The strip collapses below
280 px dock width without changing the saved preference. Hidden, collapsed or disabled
strips do no analytics work and reset their history. Symbol/tick changes, withdrawal,
and invalid grids also reset history; a one-sided book uses its remaining touch.
A locked native bucket top is valid: mid is that slot and its bid/ask sizes count in every
band. A crossed top retains the last bands, depth and history, marked stale, without
adding samples. Valid updates resume sampling; no retained values cross symbol/tick resets.

### Heatmap chart settings (S6)

The main chart draws the heatmap through `HeatmapGpuLayer`/`HeatmapTileNode` (S6b), the only renderer since S8a: on-chart liquidity labels come from the S7a cell query (S7b, below), walls use the S7a CPU cell query on request (defaulting to the viewport with optional period and tick), and the chart's GridViewState clamps zoom-out at one column per pixel (and one row per pixel in Manual). A timeframe change keeps the columns on screen: the time span scales by the timeframe ratio about the view end (1m -> 1h shows 60x more history), inside the clamps (S6d, FM-134).

The process-only `--api-port N` override moves the Agent API off `gui.api_port` for a second GUI process on the same server; it is never persisted. Screener data comes from the stream server, which invokes `screener_fetch.py`; the GUI does not launch a separate screener server.

The palette (`palette_preset`, or `Custom` with `bid_gradient`/`ask_gradient`) and `sensitivity_min`/`sensitivity_max` are chart settings (S6b): `HeatmapTileNode` samples a 512-texel palette image with the tone mapping `heatmap.gamma`, `contrast` and `shader_floor` (the retired legacy renderer's colours, matched in S6). The toolbar's palette combo writes `palettePreset`.

On first use, per-chart defaults come from the client YAML keys below (also supported under `client.heatmap`). Persisted values override those defaults. `HeatmapSettingsStore` uses `QSettings("Sentinel", "SentinelTerminal")`, `heatmap/<chartId>/<field>` with camelCase field names; the main chart ID is `main`. Named layout save/restore snapshots the persisted model under `layouts/<name>/heatmap/<chartId>/<field>`. `_last_session` never saves or restores heatmap settings, preventing stale close-time snapshots from overwriting live changes after a crash. Agent API patches with `persist:false` affect only that process and are excluded from later unrelated persisted patches and named snapshots. Shared manual tick choices live under `heatmap/manualTick/<symbol>/<timeframeMs>` and are independent of layouts. Names are escaped as path segments. Malformed stored fields fall back to their configured defaults.

| YAML key | Default | Validation / meaning |
|---|---|---|
| `tick_mode` | `auto` | `auto`, `manual` |
| `manual_tick` | 100 | Integer price units, clamped 1..10^12 then rounded up to a `{1,2,2.5,5} x 10^k` preset |
| `min_row_px` | 2 | 0.5..32 |
| `hysteresis` | 0.25 | 0..0.9 |
| `crossfade_ms` | 150 | Integer 0..2000; 0 disables |
| `show_band_edges` | false | Boolean; retained in config and settings storage, with no main-chart effect or settings-dialog control. The lab has its own band-edge toggle. |
| `palette_preset` | `Electric` | `Electric`, `Fire`, `Ocean`, `Monochrome`, `Matrix`, `Custom` |
| `bid_gradient`, `ask_gradient` | black-to-cyan / black-to-orange | 2..16 `{position, color}` stops; strictly increasing positions, first 0 and last 1; `#RRGGBB` or `#RRGGBBAA` |
| `sensitivity_min` | 0.05 | 10^-9..10^12, base quantity |
| `sensitivity_max` | 50 | 10^-9..10^15; raised to twice min when <= min |
| `opacity` | 1 | 0..1 |
| `gpu_cap_bytes` | 335544320 (320 MiB) | Integer 1 MiB..4 GiB per chart |
| `upload_budget_bytes` | 8388608 (8 MiB) | Integer 1..min(128 MiB, GPU cap) |
| `prefetch_tiles` | 1 | Integer 0..16; retained in config and settings storage, with no settings-dialog control. The span planner uses max(2, view width) tiles instead. |
| `live_min_interval_ms` | 500 | Integer 100..5000; the shortest spacing of the chart's live-edge compositions (S6c: applied live to its controller; the slow-compose backoff stays max(5000, this)) |
| `show_telemetry` | false | Boolean; shows the Heatmap Telemetry dock (View menu, its close button and Settings > Debug write it) |
| (settings only) `showLabels` | true | Boolean; GPU liquidity labels on the heatmap cells |
| (settings only) `labelCurrency` | `usd` | `usd` (price x size, `$1.24M`) or `asset` (the size as a plain number, `12.5`) |
| (settings only) `labelMinPx`, `labelMaxPx` | 12, 15 | Label font size in logical px: min 8..24, max min..32 |
| (settings only) `candleUpColor`, `candleDownColor` | `#2EBD85`, `#F6465D` | Opaque candle body colours (`#RRGGBB`). Teal-green and warm red remain distinct from the heatmap's cyan bid and orange/blue liquidity. |
| (settings only) `candleWickColor` | `auto` | `auto` follows each body colour; `#RRGGBB` overrides both wicks. |
| (settings only) `candleBodyOpacity` | 1 | 0..1; 1 is fully opaque. |
| (settings only) `candleWickWidth` | 1 | Integer 1..3 device pixels, independent of zoom and display scale. |
| (settings only) `showLadder` | false | Boolean; the chart-edge book ladder strip (chart menu "Order book ladder"). |
| (settings only) `ladderWidthPx` | 120 | Integer 60..240 logical px; the strip width while the ladder is on (chart menu "Ladder width"). |

**Liquidity labels and the range filter (S7b, GPU renderer).** A label shows on every coloured cell (valid and above the low end of the liquidity range, exactly as the shader colours it) where its text fits at `labelMinPx` plus padding (3 px each side, 1 px above and below); it grows with the cells up to `labelMaxPx` and never shrinks below `labelMinPx`. Text: at most three significant digits with k/M/B/T, trailing zeros dropped (`$11k`, `$1.2M`, `$1.24M`); the asset amount is the number only (`12.5`, `0.358`), owner decision 2026-10-02. Labels draw only on columns that show exactly the picture they were computed from (the node's drawn span content and live version, and this frame's target); a column in a transition (a live version paging in, a span revision in slot fallback, a crossfade or hold) shows none until both agree. The toolbar's two-handle range slider (log scale) writes `sensitivityMin` (low handle: smaller cells get no colour and no label) and `sensitivityMax` (high handle: colour saturation); its ends span the liquidity of the current label window (5th percentile to maximum), widened to the handles, and hold while zoomed out past the 16,000-cell label window. A drag applies for this process while it moves and is saved on release. Label text and the window are computed off the GUI thread (HeatmapCellQuery); the chart only lays out reused glyph runs per frame.

**Chart menu and toolbar modes.** The toolbar gear opens the chart menu: Chart settings / Heatmap settings (the dialog's Chart and Look tabs), Order book ladder and Ladder width (60, 90, 120, 180, 240 px), Liquidity labels, Label currency, Label size presets (12-15, 14-18, 16-20 px), Candle style, Save chart screenshot (the chart's own scene grab into `gui.screenshot_dir`, as the camera button), Layouts (save, restore, reset) and Font. The toolbar shows only the controls of the active layers: tick selector, palette and liquidity controls with the heatmap layer; candle style with candles; TPO session with TPO or the volume profile; TPO layout with TPO.

**Settings UI (S6c).** Every field above is editable in the chart's Chart Settings dialog (toolbar gear > Chart settings): tabs Chart (labels and Candles with a live preview), Tick, Look, Budgets, Live, Debug (and TPO for the chart's TPO controls). Candle colours, opacity, and wick width persist under `heatmap/<chartId>/` with the other chart settings; candle style remains a session control. Changes apply at once and are saved per chart (the Debug tab holds the telemetry toggle). Each tab has a Reset to defaults (the YAML/config defaults). The Budgets tab also edits the process RAM tiers (`heatmap/budgets/...`, every chart, applied to the running data service at once and saved only when it accepts them). The toolbar's Auto/Manual combo and preset combo (beside the timeframe) write `tickMode`/`manualTick`; a preset picked there locks Manual and is remembered under `heatmap/manualTick/<symbol>/<timeframeMs>`; entering Manual with nothing remembered locks the tick drawn now. The dialog, toolbar, telemetry dock and Agent API all go through one `HeatmapSettingsModel` per chart, so a change from any of them shows in the others. Tone mapping (gamma/contrast/shader floor) stays a session control in the Look tab, as before.

Non-finite stored/config numbers fall back to model defaults. The API requires finite numbers and correct types before clamping. Example custom gradient:

```yaml
heatmap:
  palette_preset: Custom
  bid_gradient: [{position: 0, color: "#000000"}, {position: 1, color: "#00ffff"}]
  ask_gradient: [{position: 0, color: "#000000"}, {position: 1, color: "#ffc800"}]
```

Process budgets use `HeatmapBudgets`, loaded by the main app and sentinel-lab entry point from `heatmap/budgets/decodedChunks`, `spanSources`, and `cpuCeiling` in the same settings domain, with YAML defaults `decoded_chunk_bytes: 536870912`, `span_source_bytes: 268435456`, and `cpu_ceiling_bytes: 1073741824`. Each is clamped to 1 MiB..4 GiB; the ceiling is then raised to at least the sum of the two tiers (up to 8 GiB). The lab passes budgets explicitly through `LabData::configure()` and preserves them across cache clears; direct users/tests use struct defaults without reading YAML or QSettings. The service validates budgets before starting. Chart controllers are created explicitly on its data thread; S6a creates none in the main app. Layouts snapshot chart settings only, not shared CPU budgets or shared manual tick memory.

**Paper trading (server):**

```yaml
trading:
  mode: paper
  slippage_bps: 2
```

## Full options

See the default files `config/server_config.yaml` and `config/client_config.yaml` for every key and comment. Override only what you need in the `.server_config.yaml` / `.client_config.yaml` copies.

## TPO (market profile)

Client-only look and session selection for the TPO layer (`tpo:` in `client_config.yaml`):

| Key | Default | Meaning |
|-----|---------|---------|
| `layout` | `collapsed` | `collapsed`: each price row's letters pack left-to-right from the session start (one profile per session; it sticks to the plot's left edge while its session is on screen). `split`: every period keeps its own time column, aligned with candles. |
| `theme` | `rainbow` | `rainbow`: cell colour walks red to violet across the session's periods. `calm`: single blue hue. `sage`: single green hue. In every theme the POC row is light and cells outside the 70% value area are dimmed. |
| `session` | `h24` | `ny`, `london`, `asia`, `australia`, `h24`, `w1` (7 days from Monday 00:00 UTC) or `m1` (calendar month, UTC). |
| `period_minutes` | `30` | Letter bracket. It must divide the session (for `w1`/`m1`, a UTC day) into at most 2048 periods; otherwise the largest valid standard bracket (15, 30, 60, 120, 240, 480, 1440 minutes) below it is used. |
| `sessions` | `5` | Sessions shown: the current one plus earlier ones (1-8). History is requested newest first in pages of at most 7 days and 512 periods, one page in flight at a time (the next page is sent when the reply arrives; a failed or silent page is skipped after an error or 45 s and cancelled on the server). The page budget covers every configured session, capped at 64 pages. Re-requesting the selection that is already loading is a no-op. |
| `row_px` | `14` | Price rows (trade grid ticks) merge in 1-2-5 steps until a row is at least this tall. POC and value area are computed on the merged rows. |

Letters are session-relative: `A`-`Z`, then `a`-`z`, then repeat. They fade out
when a cell is too small to read; the coloured cells remain. `m1` needs a server
that accepts session type 6.

## Related documentation

- **`docs/ARCHITECTURE.md`** — How server and client use config (e.g. `server_config` on connect).
- **`docs/PAPER_TRADING_QUICKSTART.md`** — Paper trading setup and hotkeys.

## Chart timeframes and retained trades

`chart.timeframes_ms` configures candle aggregation, the candle-history allowlist,
and advertised chart/recording timeframes. Its default is
`[1000, 60000, 300000, 900000, 3600000, 14400000, 86400000]`; the loader keeps
positive durations, sorts and deduplicates them, and keeps defaults for an empty
result. The checked-in configuration adds 100:
`[100, 1000, 60000, 300000, 900000, 3600000, 14400000, 86400000]`.
`chart.default_timeframe_ms` defaults to 0 (use the first configured
candidate); the checked-in configuration selects 60000.

`chart.timeframes_ms` accepts 100 (100 ms candles), 1000 and minute multiples;
the aggregator rejects any other value with a warning.

`chart.history_bars` maps a configured candle timeframe in ms to the number of
closed bars the server guarantees to keep in RAM for it (only 100 and 1000 are
served from RAM). A key that is not in `timeframes_ms` is ignored with a warning.
A timeframe with an entry keeps between the value and the value + 10 percent
once populated: when it holds more than value + value/10 bars, the oldest are
dropped down to the value. A timeframe without an entry keeps the 10,000-bar
cap: at most 10,000 bars, trimmed to 9,001 once full (9,001..10,000 once
populated). An absent key means no entry; there is no default value. The
checked-in configuration sets `{100: 18000}` (100 ms keeps 18,000..19,800
bars; 18,000 contiguous buckets are 30 minutes) and has no 1000 entry, so 1 s keeps the 10,000-bar
cap. Values are clamped to
[350, 200000] with a warning (350 is one history page; 200,000 bounds the
element count at about 12 MB per product before a trim, allocation up to about
twice that). The value takes effect at the next server start. At every start the
server rebuilds the newest 30 minutes of 100 ms and 1 s bars from the capture
journal (when those timeframes are configured) with these retentions; coarser
RAM history is empty after a restart. There is no knob for the rebuild.

`trade_overlays.retention_columns` defaults to 5120 (8192 in the checked-in config).
Retained execution history uses exactly
`clamp(2 * max(1000, default_timeframe_ms, timeframes_ms...) * max(1024, retention_columns), 300000, 86400000)`
milliseconds. It is independent of the overlay display `grid_width` (default 512).
These sections may be at the YAML root or under `server`. Server `heatmap.*` keys
are retired, with no aliases. Existing `data/heatmap` archives are untouched and
no longer read or written. Deploying this change requires the owner present.

## Trade overlays

Footprint, TPO and volume profile use an independent trade grid. The server
advertises these settings as `trade_overlays` in `server_config`:

```yaml
trade_overlays:
  retention_columns: 8192
  grid_width: 512
  grid_height: 2048
  tick_size: 5
  footprint_timeframe_ms: 60000
```

Width and height are bounded to 2048. Footprint's selected chart timeframe is
sent explicitly in its history request; the configured timeframe is the startup
default. TPO uses its requested session and bracket duration, with at most 2048
session columns; the bracket must divide the session duration. Price rows descend
from `max_price`, with `min_price = max_price - grid_height * tick_size`.
The first successful publication anchors the grid on the latest retained trade
(or a candle close for TPO history after restart), rounded to its own tick. The grid remains fixed for that subscription; requests
may explicitly change `price_min`, `tick_size` and `rows`. Weekly and monthly TPO
sessions use the same rows at a 5x / 10x tick, centred on this grid. Recording heatmap zoom,
re-bands and tick changes do not change this grid.

## Standalone pristine capture

`sentinel-capture` uses CLI options only and does not read or edit these YAML
files. Its default root is `/Volumes/T7/sentinel-data/raw-l2`; it refuses an
unmounted external volume and the server's recording directory. Defaults are
1-second/1-MiB zstd blocks, fsync after every block, one connection and one RAWL2
v1 stream per product, and one shared disk queue pool:

| Option | Default | Meaning |
|---|---|---|
| `--queue-mib` | `512` | Pool for all products (1..4096 MiB). Allocated only as frames queue; about 0 bytes in steady state. |
| `--queue-floor-mib` | `2` | Per-product share no other product can take (0..256 MiB). Products x floor must not exceed `--queue-mib`, else the capture refuses to start. |
| `--metrics-port` | `8091` | Prometheus `/metrics` and `/ping` on 127.0.0.1; 0 = no listener. |

Use `--root`, `--symbol`, `--symbols`, `--block-ms`, `--block-bytes`, `--fsync-blocks`,
`--zstd-level`, `--duration`, `--key-file`, `--jwt` and `--ca-bundle` to override
the other defaults. The launchd plist passes none of the queue or metrics options. See [the capture runbook](RAW_CAPTURE.md) for detached startup,
SIGTERM shutdown, offline verification and the RAWL2 format.
