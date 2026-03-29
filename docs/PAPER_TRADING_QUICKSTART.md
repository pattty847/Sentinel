# Paper trading quickstart

Minimal guide to Sentinel’s built-in paper trading: server-authoritative, no real broker.

## What it does

- Client sends **`trade_command`** messages (place, cancel, flatten).
- Server runs in **paper mode** (`trading.mode: paper`), applies fills using last trade price and optional slippage, and maintains order/position state.
- Server streams **`order_update`** and **`position_update`** back to clients.

No real broker APIs are used.

## 1. Configure

**Server** (`config/server_config.yaml` or `config/.server_config.yaml`):

```yaml
trading:
  mode: "paper"
  slippage_bps: 2
```

- `mode` — Currently only `paper` is supported.
- `slippage_bps` — Basis-point slippage applied to paper fills.

**Client** (`config/client_config.yaml` or `config/.client_config.yaml`):

```yaml
gui:
  default_order_qty: 1.0
```

- Default quantity for trade hotkeys. The Qty field in the status bar overrides this at runtime.

## 2. Run server and client

Start the server first, then the GUI client (e.g. after `cmake --build --preset windows-msvc-vs`):

- `sentinel-server`
- `sentinel_gui`

## 3. Subscribe to a symbol

In the GUI, subscribe to a symbol (e.g. `BTC-USD`). Paper fills use the last trade price for that symbol; ensure market data is flowing.

## 4. Hotkeys

| Key | Action |
|-----|--------|
| **B** | Buy market |
| **S** | Sell market |
| **F** | Flatten current symbol position |
| **C** | Cancel all active orders |

**Order quantity:** Status bar Qty input → else `gui.default_order_qty` from client config → else `1.0`.

## 5. UI feedback

- **Trade blotter dock** — Order rows (ID, side, qty, filled, status).
- **Position overlay** — Current symbol position size, average price, unrealized PnL.

Typical market order: `NEW` → `FILLED` → position overlay updates.

## 6. Troubleshooting

- **No fills** — Check server is running and the symbol has recent trade updates.
- **No blotter updates** — Check client is connected and subscribed to the symbol.
- **Unexpected fill price** — Check `trading.slippage_bps` in server config.

## Protocol

- **Client → Server:** `trade_command`
- **Server → Client:** `order_update`, `position_update`

For wire-level and TLS details, see **`docs/MARKETDATA.md`** (Trading stream and Transport security sections). For config keys, see **`docs/CONFIG.md`**.
