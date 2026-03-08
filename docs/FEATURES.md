# Sentinel feature overview

High-level outline of what Sentinel provides. For implementation and pipeline details, see `docs/ARCHITECTURE.md` and `docs/MARKETDATA.md`.

## Core capabilities

- **Client–server trading terminal** — Headless server for 24/7 data ingestion; Qt6/QML client for visualization. Client is remote-only (no local-only mode).
- **GPU-accelerated charting** — Heatmap, footprint, TPO, candlesticks, volume profile, and labels rendered on GPU via streamed textures and MSDF atlas. Single-quad sampling; no per-cell CPU rendering.
- **Order book heatmap** — TWAP aggregation into dense u8 columns (bids/asks); configurable grid size (e.g. 8192×8192). Ring-buffer semantics; incremental column uploads.
- **Unified coordinate mapping** — All chart layers share `TimeAxisMapping` (1 slice = 1 candle bar). See `docs/COORDINATE_SYSTEMS.md` for renderer contracts.
- **Market data** — Coinbase Advanced Trade WebSocket: level2, market_trades, candles, heartbeats. Optional JWT auth for user/futures channels. See `docs/MARKETDATA.md`.
- **Paper trading** — Server-side paper execution: place/cancel/flatten via hotkeys; order and position updates streamed to client. No real broker. See `docs/PAPER_TRADING_QUICKSTART.md`.
- **Config** — Separate server (authoritative) and client (UI) YAML configs with optional override files. See `docs/CONFIG.md`.
- **TLS (WSS)** — Optional encryption for the client–server stream; self-signed certs for localhost. See `docs/MARKETDATA.md` (Transport security).

## UI and performance

- Docking framework; trade blotter and position overlay.
- Axis labels: fixed-capacity models, no QML object churn during pan/zoom; nice ticks with hysteresis on price and time; live axis updates during drag (~110+ FPS with aggressive pan/zoom when tuned).
- Screenshot API (HTTP on 127.0.0.1) for capturing the main window.

## Notable completed work (reference)

**Axis performance (2026-02)** — Replaced Repeater/model churn with fixed-capacity axis models and deferred viewport commits. Moved pan offset out of per-label bindings; axis recompute uses visual pan ranges. Result: ~110 FPS with dynamic axis updates during pan/zoom; role-scoped `dataChanged` to reduce binding overhead.

---

For build, run, and API keys, see the root **`README.md`**.
