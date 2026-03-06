# Sentinel Config Guide

Sentinel uses two config files with optional override files.
Server controls authoritative data settings; client controls UI and rendering preferences.

## Files

Server:
- `config/server_config.yaml`
- `config/.server_config.yaml` (optional override, not tracked)

Client:
- `config/client_config.yaml`
- `config/.client_config.yaml` (optional override, not tracked)

## Load Order

Server:
1. `config/server_config.yaml`
2. `config/.server_config.yaml` (overrides)

Client:
1. `config/client_config.yaml`
2. `config/.client_config.yaml` (overrides)

## Ownership Rules

Server-authoritative (client cannot override):
- Heatmap grid/timeframes/intensity normalization
- Order book tick size and band percent
- Candle gating
- Market data connection + TLS
- Default symbols
- Trading mode + paper slippage

Client-only:
- Visual tuning (gamma/contrast/labels/colors)
- GUI settings (API port, screenshots, font)
- Client cache sizing and local UI prefs

## Examples

Server config (authoritative):
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
    use_jwt: false   # Set true only when key.json exists and you need user/futures channels
    ssl_ca_bundle: resources/certs/ca-bundle.crt
```

Public market data (level2, market_trades, candles) does not require a key; the server runs without `key.json` by default.

Client config (local UI):
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

Trading config (server):
```yaml
trading:
  mode: paper
  slippage_bps: 2
```
