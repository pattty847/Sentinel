# Sentinel configuration

Sentinel uses two YAML configs: server (authoritative for data and trading) and client (UI and rendering). Each has an optional override file that is not tracked in git.

## Files and load order

| Role | Default | Override (optional) |
|------|---------|---------------------|
| Server | `config/server_config.yaml` | `config/.server_config.yaml` (overrides default) |
| Client | `config/client_config.yaml` | `config/.client_config.yaml` (overrides default) |

Copy the defaults to the override names to customize; override values take precedence.

## Ownership

**Server-authoritative (client cannot override):**

- Heatmap grid, timeframes, intensity normalization
- Order book tick size and band percent
- Candle gating
- Market data connection and TLS
- Default symbols
- Trading mode and paper slippage

**Client-only:**

- Visual tuning (gamma, contrast, labels, colors)
- GUI settings (API port, screenshot dir, font)
- Client cache sizing and local UI preferences

## Example snippets

**Server (`config/server_config.yaml` or `.server_config.yaml`):**

```yaml
stream_port: 8080
heatmap:
  timeframes_ms: [1000, 60000, 300000, 900000, 3600000, 14400000, 86400000]
  timeframe: 60000
  grid_width: 2048
  grid_height: 1024
  intensity_mode: log
  intensity_max_mode: running
  persistence_enabled: true
  persistence_dir: data/heatmap
  persistence_fsync_every_n_records: 1
  persistence_fsync_every_ms: 1000
  persistence_retention_days: 0  # keep all day files

recording:                # recording v2: near and deep order-book layers (docs/research/2026-09-recording-v2.md)
  enabled: true
  dir: /Volumes/T7/sentinel-data/recording
  fallback_dir: data/recording   # used when dir's volume is not mounted; empty = do not record
  near_tick: 1              # $ rows within near_pct of the mid
  near_pct: 0.05
  deep_tick: 5              # $ rows across [mid*deep_low_frac, mid*deep_high_mult]
  deep_low_frac: 0.25
  deep_high_mult: 4
  live_publish_ms: 500      # open-minute live publication interval (2 Hz); clamped to [250, 5000]
  # advanced: price_scale (100), size_floor (1e-6), codes_per_octave (819), lateness_ms (2000)

server:
  mdc:
    host: advanced-trade-ws.coinbase.com
    port: 443
    target: /v1
    use_jwt: false   # true only when key.json exists and user/futures channels are needed
    ssl_ca_bundle: resources/certs/ca-bundle.crt
    connect_timeout_ms: 20000  # resolve + TCP + TLS + WS handshake; timeout -> backoff retry
    close_timeout_ms: 3000     # WS close to an unresponsive peer
    max_connections: 8        # GUI-only products; pinned default_symbols do not count (minimum 1)
```

`recording.live_publish_ms` (default 500, clamped to [250, 5000]) is how often the recorder publishes each layer's open minute to live subscribers (owner decision 2026-10-01: 500 ms, 2 Hz). The live worker paces subscriptions at half of it, so each publication is sent at the next worker turn; refused sends back off from there up to 5 s. It does not change what is recorded on disk. Recorder-thread cost: about 0.4 ms per publication for a BTC-USD book (~23,500 near+deep entries), 0.08% of a core at 2 Hz (`recording_live_bench --publish`).

Changing `recording.deep_tick` from $10 to $5 changes the recording config hash and starts a new HMC2 generation; existing files stay in the same series. Display ticks use `{1,2,2.5,5} x 10^k` restricted to native-tick multiples, so the $5 layer supports $25 rows. Pages spanning old $10 and new $5 generations serve both at common multiples (for example $50); at $25, output buckets containing $10 records are unknown while compatible buckets still serve. History and live projection use the same rule.

`server.mdc.max_connections` limits distinct active GUI-only products, including feeds still connecting. The default is 8; values below 1 are rejected. Multiple clients watching the same product share a slot. Pinned `default_symbols` are exempt. At capacity, a new symbol is refused with a stream error (symbol and cap), a GUI status-bar message and `data.lastSubscriptionRefusal` in `/api/v1/state`; existing watched products are never evicted. Unsubscribing or disconnecting the final watcher frees the slot.

Public market data (level2, market_trades, candles) does not require a key; the server runs without `key.json` by default.

**Client (`config/client_config.yaml` or `.client_config.yaml`):**

```yaml
heatmap:
  source: legacy             # legacy | recording; recording requires advertised recording.available
  gamma: 1.05
  contrast: 1.15
  label_px: 9999
  client_cache_columns: 1024  # history/GPU page; protocol maximum is 1024
  initial_column_px: 8        # time zoom on connect: screen pixels per heatmap column
  initial_price_pct: 5        # price zoom on connect: % of the band; 0 = full
  target_row_px: 2            # display tick: minimum row height (px)
  cell_aspect: 0.75           # display tick: rows merge (1-2-5 steps) toward column width * cell_aspect (square-ish cells)
  sensitivity_min: 0.05       # recording mode colour range in base units (log scale): <= min dark
  sensitivity_max: 50         # >= max brightest

gui:
  api_port: 17100
  screenshot_dir: ./screenshots
  default_order_qty: 1.0
```

`heatmap.source` defaults to `legacy` (unknown values also fall back to legacy).
`recording` is active only after a connected server advertises `recording.available: true`;
otherwise the client uses the legacy path. Recording mode displays closed recording buckets
only; legacy live values are not mixed into its absolute log codes. Live recording columns
are deferred to S4. The presentation clock holds at the newest recorded bucket.

Recording requests use 2048 rows and the square-cell target from `target_row_px` and
`cell_aspect`. A price exit from the buffered band or a change in the ideal 1-2-5 tick
starts a 150 ms trailing debounce. Requests retain at least 50% of the visible price
span on each side (clipped at price zero). Because the current protocol's `display_tick`
is exact and native ticks are not advertised, the first request expands the range to
at least `2048 * ideal_tick` and leaves tick selection to the server. Its returned
band is authoritative; continuation pages pin that band and tick. Thus the buffer
can be wider than 50%, especially at close time zoom. This requires no protocol change.

For data-path checks, start the GUI with `SENTINEL_PROBES=heatmap.recording,heatmap.window`.
`heatmap.recording.reband` logs the generation, requested range, ideal tick and row count;
`request` adds the unique request id and paging boundary; `page` logs the authoritative
tick, scanned interval, `next_end` and exhaustion; `stale` reports obsolete replies.
Shader/label support for these codes and validity bits is a separate S3b change.

### Heatmap chart settings (S6)

`heatmap.renderer: legacy|gpu` defaults to `gpu` (the owner flipped the default on 2026-10-01); unknown config values fall back to `gpu`. `legacy` stays selectable (Settings > Debug, `--heatmap-renderer legacy`, the settings route) until S8 deletes it. This is independent of `heatmap.source` (the legacy projection's data source). With `gpu` (S6b) the main chart draws the heatmap through `HeatmapGpuLayer`/`HeatmapTileNode`, the legacy band stream is muted (`DataProcessor::setHeatmapEnabled(false)`; on-chart liquidity labels come from the S7a cell query (S7b, below); walls use the S7a CPU cell query on request, defaulting to the viewport with optional period and tick), and the chart's GridViewState clamps zoom-out at one column per pixel (and one row per pixel in Manual). A timeframe change keeps the columns on screen: the time span scales by the timeframe ratio about the view end (1m -> 1h shows 60x more history), inside the clamps (S6d, FM-134); the legacy renderer resets to `initial_column_px` columns instead. The renderer flips at runtime both ways through the settings route.

Process-only overrides on the `sentinel-gui` command line (never persisted): `--heatmap-renderer legacy|gpu` selects the renderer for this process, `--api-port N` moves the Agent API off `gui.api_port` (two processes on one server for A/B runs), and `--no-screener` skips the `screener_server.py` child (it binds port 17200 and the GUI kills any port-17200 holder before it starts one, so extra processes such as `scripts/dev/heatmap-ab.sh` pass it).

The palette (`palette_preset`, or `Custom` with `bid_gradient`/`ask_gradient`) and `sensitivity_min`/`sensitivity_max` are chart settings that both renderers draw (S6b): the legacy renderer and `HeatmapTileNode` sample the same 512-texel palette image with the legacy tone mapping (`heatmap.gamma`, `contrast`, `shader_floor`), so a preset gives the same colour for the same recording code. The toolbar's palette combo writes `palettePreset`. `opacity` applies to the GPU renderer.

On first use, per-chart defaults come from the client YAML keys below (also supported under `client.heatmap`). Persisted values override those defaults. `HeatmapSettingsStore` uses `QSettings("Sentinel", "SentinelTerminal")`, `heatmap/<chartId>/<field>` with camelCase field names; the main chart ID is `main`. Named layout save/restore snapshots the persisted model under `layouts/<name>/heatmap/<chartId>/<field>`. `_last_session` never saves or restores heatmap settings, preventing stale close-time snapshots from overwriting live changes after a crash. Agent API patches with `persist:false` affect only that process and are excluded from later unrelated persisted patches and named snapshots. Shared manual tick choices live under `heatmap/manualTick/<symbol>/<timeframeMs>` and are independent of layouts. Names are escaped as path segments. Malformed stored fields fall back to their configured defaults.

| YAML key | Default | Validation / meaning |
|---|---|---|
| `renderer` | `gpu` | `gpu`, `legacy` (legacy is removed in S8) |
| `tick_mode` | `auto` | `auto`, `manual` |
| `manual_tick` | 100 | Integer price units, clamped 1..10^12 then rounded up to a `{1,2,2.5,5} x 10^k` preset |
| `min_row_px` | 2 | 0.5..32; GPU policy, separate from legacy `target_row_px` |
| `hysteresis` | 0.25 | 0..0.9 |
| `crossfade_ms` | 150 | Integer 0..2000; 0 disables |
| `show_band_edges` | false | Boolean; saved and editable, drawn by sentinel-lab only (the main chart does not draw band edges yet) |
| `palette_preset` | `Electric` | `Electric`, `Fire`, `Ocean`, `Monochrome`, `Matrix`, `Custom` |
| `bid_gradient`, `ask_gradient` | black-to-cyan / black-to-orange | 2..16 `{position, color}` stops; strictly increasing positions, first 0 and last 1; `#RRGGBB` or `#RRGGBBAA` |
| `sensitivity_min` | 0.05 | 10^-9..10^12, base quantity |
| `sensitivity_max` | 50 | 10^-9..10^15; raised to twice min when <= min |
| `opacity` | 1 | 0..1 |
| `gpu_cap_bytes` | 335544320 (320 MiB) | Integer 1 MiB..4 GiB per chart |
| `upload_budget_bytes` | 8388608 (8 MiB) | Integer 1..min(128 MiB, GPU cap) |
| `prefetch_tiles` | 1 | Integer 0..16; saved and editable, not read by the span planner yet (it prefetches max(2, view width) tiles) |
| `live_min_interval_ms` | 500 | Integer 100..5000; the shortest spacing of the chart's live-edge compositions (S6c: applied live to its controller; the slow-compose backoff stays max(5000, this)) |
| `show_telemetry` | false | Boolean; shows the Heatmap Telemetry dock (View menu, its close button and Settings > Debug write it) |
| (settings only) `showLabels` | true | Boolean; GPU liquidity labels on the heatmap cells |
| (settings only) `labelCurrency` | `usd` | `usd` (price x size, `$1.24M`) or `asset` (the size as a plain number, `12.5`) |
| (settings only) `labelMinPx`, `labelMaxPx` | 12, 15 | Label font size in logical px: min 8..24, max min..32 |
| (settings only) `candleUpColor`, `candleDownColor` | `#2EBD85`, `#F6465D` | Opaque candle body colours (`#RRGGBB`). Teal-green and warm red remain distinct from the heatmap's cyan bid and orange/blue liquidity. |
| (settings only) `candleWickColor` | `auto` | `auto` follows each body colour; `#RRGGBB` overrides both wicks. |
| (settings only) `candleBodyOpacity` | 1 | 0..1; 1 is fully opaque. |
| (settings only) `candleWickWidth` | 1 | Integer 1..3 device pixels, independent of zoom and display scale. |

**Liquidity labels and the range filter (S7b, GPU renderer).** A label shows on every coloured cell (valid and above the low end of the liquidity range, exactly as the shader colours it) where its text fits at `labelMinPx` plus padding (3 px each side, 1 px above and below); it grows with the cells up to `labelMaxPx` and never shrinks below `labelMinPx`. Text: at most three significant digits with k/M/B/T, trailing zeros dropped (`$11k`, `$1.2M`, `$1.24M`); the asset amount is the number only (`12.5`, `0.358`), owner decision 2026-10-02. Labels draw only on columns that show exactly the picture they were computed from (the node's drawn span content and live version, and this frame's target); a column in a transition (a live version paging in, a span revision in slot fallback, a crossfade or hold) shows none until both agree. The toolbar's two-handle range slider (log scale) writes `sensitivityMin` (low handle: smaller cells get no colour and no label) and `sensitivityMax` (high handle: colour saturation); its ends span the liquidity of the current label window (5th percentile to maximum), widened to the handles, and hold while zoomed out past the 16,000-cell label window. A drag applies for this process while it moves and is saved on release. In legacy mode the toolbar shows the old threshold slider instead, and no Labels toggle (legacy always draws its labels; the currency combo stays). Label text and the window are computed off the GUI thread (HeatmapCellQuery); the chart only lays out reused glyph runs per frame.

**Chart menu and toolbar modes.** The toolbar gear opens the chart menu: Chart settings / Heatmap settings (the dialog's Chart and Look tabs), Liquidity labels, Label currency, Label size presets (12-15, 14-18, 16-20 px), Candle style, Save chart screenshot (the chart's own scene grab into `gui.screenshot_dir`, as the camera button), Layouts (save, restore, reset) and Font. The toolbar shows only the controls of the active layers: tick selector, palette and liquidity controls with the heatmap layer; candle style with candles; TPO session with TPO or the volume profile; TPO layout with TPO.

**Settings UI (S6c).** Every field above is editable in the chart's Chart Settings dialog (toolbar gear > Chart settings): tabs Chart (labels and Candles with a live preview), Tick, Look, Budgets, Live, Debug (and TPO for the chart's TPO controls). Candle colours, opacity, and wick width persist under `heatmap/<chartId>/` with the other chart settings; candle style remains a session control. Changes apply at once and are saved per chart, except the Debug renderer, which applies to this session only unless "Make default" is ticked. Each tab has a Reset to defaults (the YAML/config defaults). The Budgets tab also edits the process RAM tiers (`heatmap/budgets/...`, every chart, applied to the running data service at once and saved only when it accepts them). The toolbar's Auto/Manual combo and preset combo (beside the timeframe) write `tickMode`/`manualTick`; a preset picked there locks Manual and is remembered under `heatmap/manualTick/<symbol>/<timeframeMs>`; entering Manual with nothing remembered locks the tick drawn now. The dialog, toolbar, telemetry dock and Agent API all go through one `HeatmapSettingsModel` per chart, so a change from any of them shows in the others. Tone mapping (gamma/contrast/shader floor) stays a session control in the Look tab, as before.

Non-finite stored/config numbers fall back to model defaults. The API requires finite numbers and correct types before clamping. Example custom gradient:

```yaml
heatmap:
  renderer: gpu
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

## Trade overlays

Footprint, TPO and volume profile use an independent trade grid. The server
advertises these settings as `trade_overlays` in `server_config`:

```yaml
trade_overlays:
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
