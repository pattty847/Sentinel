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
  timeframe: 1000
  grid_width: 2048
  grid_height: 1024
  intensity_mode: log
  intensity_max_mode: running

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
