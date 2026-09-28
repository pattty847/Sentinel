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
  deep_tick: 10             # $ rows across [mid*deep_low_frac, mid*deep_high_mult]
  deep_low_frac: 0.25
  deep_high_mult: 4
  # advanced: price_scale (100), size_floor (1e-6), codes_per_octave (819), lateness_ms (2000)

server:
  mdc:
    host: advanced-trade-ws.coinbase.com
    port: 443
    target: /v1
    use_jwt: false   # true only when key.json exists and user/futures channels are needed
    ssl_ca_bundle: resources/certs/ca-bundle.crt
```

Public market data (level2, market_trades, candles) does not require a key; the server runs without `key.json` by default.

**Client (`config/client_config.yaml` or `.client_config.yaml`):**

```yaml
heatmap:
  gamma: 1.05
  contrast: 1.15
  label_px: 9999
  client_cache_columns: 1024  # history/GPU page; protocol maximum is 1024
  initial_column_px: 8        # time zoom on connect: screen pixels per heatmap column
  initial_price_pct: 5        # price zoom on connect: % of the band; 0 = full
  target_row_px: 2            # display tick: minimum row height (px)
  cell_aspect: 0.75           # display tick: rows merge (1-2-5 steps) toward column width * cell_aspect (square-ish cells)

gui:
  api_port: 17100
  screenshot_dir: ./screenshots
  default_order_qty: 1.0
```

**Paper trading (server):**

```yaml
trading:
  mode: paper
  slippage_bps: 2
```

## Full options

See the default files `config/server_config.yaml` and `config/client_config.yaml` for every key and comment. Override only what you need in the `.server_config.yaml` / `.client_config.yaml` copies.

## Related documentation

- **`docs/ARCHITECTURE.md`** — How server and client use config (e.g. `server_config` on connect).
- **`docs/PAPER_TRADING_QUICKSTART.md`** — Paper trading setup and hotkeys.
