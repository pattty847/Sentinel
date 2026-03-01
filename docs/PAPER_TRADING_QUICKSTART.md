# Paper Trading Quickstart

This is a minimal usage guide for Sentinel's built-in paper trading vertical slice.

## What it does

Paper trading is server-authoritative and brokerless:

- Client sends `trade_command` messages.
- Server executes with paper logic (`mode: paper`), updates order/position state.
- Server streams `order_update` and `position_update` back to clients.

No real broker APIs are used.

## 1) Configure

### Server config (`config/server_config.yaml`)

```yaml
trading:
  mode: "paper"
  slippage_bps: 2
```

- `mode`: currently paper mode.
- `slippage_bps`: basis-point slippage applied by paper fills.

### Client config (`config/client_config.yaml`)

```yaml
gui:
  default_order_qty: 1.0
```

- Default order quantity used by trade hotkeys.
- In GUI, the Qty input in the status bar overrides this at runtime.

## 2) Run server and GUI

Start the server first, then the GUI client.

Typical build command:

```bash
cmake --build --preset windows-msvc-vs
```

Then launch:

- `sentinel-server`
- `sentinel_gui`

## 3) Subscribe to a symbol

In GUI, enter a symbol like `BTC-USD` and subscribe.

Paper fills rely on last trade price for that symbol, so make sure live market data is flowing.

## 4) Place/cancel/flatten with hotkeys

Default hotkeys:

- `B` = Buy market
- `S` = Sell market
- `F` = Flatten current symbol position
- `C` = Cancel all active orders

Order quantity source:

1. Qty input in status bar (if set)
2. `client.gui.default_order_qty`
3. Safe fallback (`1.0`)

## 5) Read UI feedback

- **Trade Blotter Dock**: order rows with ID, side, qty, filled, status.
- **Position overlay**: current symbol position size, avg price, unrealized PnL.

Expected flow for a market order:

1. `NEW` update appears.
2. Immediate `FILLED` update follows.
3. Position overlay updates.

## 6) Troubleshooting

- No fills: verify server is running and symbol has last trade updates.
- No blotter updates: verify client is connected/subscribed to the symbol.
- Unexpected price: check `trading.slippage_bps`.

## Protocol notes

Trade messages:

- Client -> Server: `trade_command`
- Server -> Client: `order_update`, `position_update`

For full wire-level details, see `docs/MARKETDATA.md`.
