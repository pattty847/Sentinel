# Sentinel Feature Overview

High-level outline of what Sentinel provides. For implementation and pipeline details, see `docs/ARCHITECTURE.md` and `docs/MARKETDATA.md`. For a historical log of completed work, see `docs/FEATURE_ARCHIVE.md`.

## Project Snapshot
- [x] **GPU-Accelerated Heatmap** — Real-time TWAP aggregation and rendering via ring-buffer textures.
- [x] **Unified Charting** — Candlesticks, footprint, and heatmap sharing a single coordinate system.
- [x] **Screener Dock** — Multi-asset real-time monitoring via Python-backend WebSocket.
- [x] **MSDF Text System** — High-performance, zoom-stable GPU numeric overlays.
- [x] **Paper Trading Broker** — Unified simulation engine for manual and algorithmic trading.
- [x] **Refactored Protocol** — Specialized message-router architecture for low-latency dispatch.
- [x] **Agent Infrastructure** — Automated invariant tracking and "Why"-focused documentation.

## Recent shipped changes (Mar 2026)
- [x] **Avendella MM integration (paper)** — Algo engine wiring, chart overlays, and state-flow hardening for paper trading.
- [x] **Manual TP/SL risk brackets** — Server-backed attached risk orders with staged chart interaction + OCO clearing.
- [x] **Trade-log replay path** — Binary trade-log read path feeding replay + chart overlays.
- [x] **ChartDock rename** — HeatmapDock consolidated into ChartDock naming across GUI and docs.
- [x] **Timeframe switching fixes** — Resolved stale/wide-column mismatch in heatmap timeframe changes.

## Core capabilities

- **Client–server trading terminal** — Headless server for 24/7 data ingestion; Qt6/QML client for visualization. Client is remote-only (no local-only mode).
- **GPU-accelerated charting** — Heatmap, footprint, TPO, candlesticks, volume profile, and labels rendered on GPU via streamed textures and MSDF atlas. Single-quad sampling; no per-cell CPU rendering.
- **Order book heatmap** — TWAP aggregation into dense u8 columns (bids/asks); configurable grid size (e.g. 8192×8192). Ring-buffer semantics; incremental column uploads.
- **Unified coordinate mapping** — All chart layers share `TimeAxisMapping` (1 slice = 1 candle bar). See `docs/COORDINATE_SYSTEMS.md` for renderer contracts.
- **Market data** — Coinbase Advanced Trade WebSocket: level2, market_trades, candles, heartbeats. Optional JWT auth for user/futures channels. See `docs/MARKETDATA.md`.
- **Paper trading** — Server-side paper execution: place/cancel/flatten via hotkeys; order and position updates streamed to client. Unified broker handles both live paper and backtest replay. See `docs/TRADING_SIMULATION_BLUEPRINT.md`.
- **Config** — Separate server (authoritative) and client (UI) YAML configs with optional override files. See `docs/CONFIG.md`.
- **TLS (WSS)** — Optional encryption for the client–server stream; self-signed certs for localhost. See `docs/MARKETDATA.md` (Transport security).

## UI and performance

- **Docking framework** — Trade blotter, position overlay, and screener dock integrated into a flexible workspace.
- **Axis labels** — Fixed-capacity models, no QML object churn during pan/zoom; nice ticks with hysteresis on price and time; live axis updates during drag (~110+ FPS).
- **MSDF Text Rendering** — Zoom-stable, crisp labels for price and volume levels using Multi-channel Signed Distance Fields.
- **Screenshot API** — HTTP on 127.0.0.1 for capturing the main window state.

## Market Data & Connectivity

- **Message Router Refactor** — Decomposed protocol handling into specialized handlers (Snapshot, L2, Trade, Heatmap) for improved maintainability and latency.
- **Coinbase Advanced Trade Integration** — Support for Level 2 order books, real-time trades, and 1s candle streams with server-side gating.

## Developer Experience

- **Agent Notes System** — Standardized `_agent/` scratchpad for tracking architectural invariants, failure modes, and technical decisions.
- **Senior-Dev Comment Standard** — Codebase-wide audit to ensure documentation focuses on "Why" (intent/invariants) rather than "What" (obvious logic).
- **Unified Testing** — Comprehensive test suite for protocol validation, stream parsing, and trading logic.

---

For build, run, and API keys, see the root **`README.md`**.
