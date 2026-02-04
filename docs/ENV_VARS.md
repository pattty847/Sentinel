# Sentinel Environment Variables

This is the authoritative list of runtime environment variables read or set by Sentinel.
Defaults shown are the in-code fallbacks when the variable is unset or invalid.

## Sentinel (core/gui)

- `SENTINEL_SERVER_MODE` (server)  
  Enables server mode when set (server entrypoint sets this to `1`).
- `SENTINEL_SSL_CA_BUNDLE` (server)  
  Path to a custom CA bundle for TLS verification.
- `SENTINEL_CA_BUNDLE` (server)  
  Alias for `SENTINEL_SSL_CA_BUNDLE` (legacy/short).

- `SENTINEL_HEATMAP_TF` (server)  
  Heatmap timeframe in milliseconds. Default: `1000`. Client now uses server_config.
- `SENTINEL_HEATMAP_TIMEFRAMES` (server)  
  Comma/space-separated list of server heatmap anchor timeframes (ms). Default: `1000 60000 3600000 86400000`.
- `SENTINEL_HEATMAP_GRID_WIDTH` (server)  
  Heatmap grid width (history columns). Default: `5120`.
- `SENTINEL_HEATMAP_GRID_HEIGHT` (server)  
  Heatmap grid height (price buckets). Default: `2048`.
- `SENTINEL_HEATMAP_GRID` (server)  
  Heatmap grid height (price buckets). Default: `2048`. Legacy alias for height on server.
- `SENTINEL_HEATMAP_TICK_SIZE` (server)  
  Fixed tick size for heatmap grid rows (locks row size; banding only recenters). Default: `1.0`.  
  Set to `0` to enable dynamic tick sizing based on band percent.
- `SENTINEL_HEATMAP_RECENTER_DELTA` (server)  
  Recenter threshold as fraction of mid price. Default: `0.01`.
- `SENTINEL_HEATMAP_BAND_FAST` (server)  
  Band percent for fast timeframes. Default: `0.15`.
- `SENTINEL_HEATMAP_BAND_MED` (server)  
  Band percent for medium timeframes. Default: `0.25`.
- `SENTINEL_HEATMAP_BAND_SLOW` (server)  
  Band percent for slow timeframes. Default: `0.35`.
- `SENTINEL_HEATMAP_INTENSITY_FLOOR` (server)  
  Minimum intensity cutoff in [0.0, 1.0]. Default: `0.001`.
- `SENTINEL_HEATMAP_INTENSITY_MODE` (server)  
  Intensity normalization mode: `log` (default), `power`, or `linear`.
- `SENTINEL_HEATMAP_INTENSITY_LOG_SCALE` (server)  
  Log scale factor for `log` mode. Default: `1000`.
- `SENTINEL_HEATMAP_INTENSITY_POWER` (server)  
  Exponent for `power` mode. Default: `0.4`.
- `SENTINEL_HEATMAP_INTENSITY_MAX_MODE` (server)  
  Max normalization mode: `running` (default) or `column`.
- `SENTINEL_HEATMAP_INTENSITY_MAX_DECAY` (server)  
  Running max decay factor in (0, 1]. Default: `0.995`.
- `SENTINEL_HEATMAP_RECENTER` (client, deprecated)  
  Deprecated; client ignores. Use `SENTINEL_HEATMAP_RECENTER_DELTA` on server.
- `SENTINEL_HEATMAP_SLICE_LOG` (server + client)  
  Enables periodic heatmap slice logging.
- `SENTINEL_HEALTH_PORT` (server)  
  Local health endpoint port for `/ping`. Default: `8090`.
- `SENTINEL_STREAM_PORT` (server)  
  WebSocket stream port for SentinelStreamServer. Default: `8080`.
- `SENTINEL_SERVER_DEFAULT_SYMBOLS` (server)  
  Symbols to auto-subscribe on server start (comma/space-separated). Default: `BTC-USD`.
- `SENTINEL_CANDLE_UPDATE_BPS_FAST` (server)  
  Bps threshold for candle updates on fast timeframes (<=1s). Default: `0.00005` (0.5 bps).
- `SENTINEL_CANDLE_UPDATE_BPS_SLOW` (server)  
  Bps threshold for candle updates on slow timeframes (>1s). Default: `0.0002` (2 bps).
- `SENTINEL_CANDLE_UPDATE_TICK_MULT_FAST` (server)  
  Tick multiplier for update gating on fast timeframes. Default: `1`.
- `SENTINEL_CANDLE_UPDATE_TICK_MULT_SLOW` (server)  
  Tick multiplier for update gating on slow timeframes. Default: `2`.
- `SENTINEL_CANDLE_UPDATE_SILENCE_MS_FAST` (server)  
  Max silence (ms) before forcing an update on fast timeframes. Default: `200`.
- `SENTINEL_CANDLE_UPDATE_SILENCE_MS_SLOW` (server)  
  Max silence (ms) before forcing an update on slow timeframes. Default: `1000`.
- `SENTINEL_CANDLE_UPDATE_VOLUME_FAST` (server)  
  Volume delta threshold for update gating on fast timeframes. Default: `0.0` (disabled).
- `SENTINEL_CANDLE_UPDATE_VOLUME_SLOW` (server)  
  Volume delta threshold for update gating on slow timeframes. Default: `0.0` (disabled).
- `SENTINEL_CANDLE_UPDATE_TICK_SIZE` (server)  
  Tick size used for update gating (overrides orderbook tick size). Default: `0` (falls back to `SENTINEL_ORDERBOOK_TICK_SIZE`).

- `SENTINEL_MDC_HOST` (server)  
  Override MarketDataCore WebSocket host. Default: `advanced-trade-ws.coinbase.com`.
- `SENTINEL_MDC_PORT` (server)  
  Override MarketDataCore WebSocket port. Default: `443`.
- `SENTINEL_MDC_TARGET` (server)  
  Override MarketDataCore WebSocket target path. Default: `/v1`.
- `SENTINEL_MDC_USE_JWT` (server)  
  Enable JWT on market data subscriptions. Default: `0`. Set to `1` to enable (any other value disables).

- `SENTINEL_GPU_HEATMAP_DEBUG` (client)  
  Enables verbose GPU heatmap diagnostics.
- `SENTINEL_GPU_HEATMAP_FORCE_FULL` (client)  
  Forces full texture render rather than visible range.
- `SENTINEL_HEATMAP_GAMMA` (client)  
  Heatmap gamma applied in the shader. Default: `1.05`.
- `SENTINEL_HEATMAP_CONTRAST` (client)  
  Heatmap contrast applied in the shader. Default: `1.15`.
- `SENTINEL_HEATMAP_SHADER_FLOOR` (client)  
  Minimum shader brightness floor in [0.0, 1.0]. Default: `0.1`.
- `SENTINEL_DUMP_GLYPH_ATLAS` (client)  
  Dumps glyph atlas textures for debugging.
- `SENTINEL_HEATMAP_LABEL_PX` (client)  
  Minimum cell pixel height before labels render. Default: `24`.
- `SENTINEL_HEATMAP_CLIENT_CACHE_COLUMNS` (client)  
  Client heatmap cache capacity (columns). Default: `gridWidth`.
- `SENTINEL_MSDF_FONT` (client)  
  Absolute path to a font file used for MSDF atlas generation (Lab dock).

- `SENTINEL_ORDERBOOK_TICK_SIZE` (server)  
  Default order book tick size. Default: `0.10`.
- `SENTINEL_ORDERBOOK_BAND_PCT` (server)  
  Default order book band percent. Default: `0.30`.

- `SENTINEL_QML_PATH` (client)  
  Overrides QML path used by the GUI.
- `SENTINEL_ZOOM_DEBUG` (client)  
  Enables zoom debug logging.
- `SENTINEL_CHART_DEBUG` (client)  
  Enables verbose chart timeframe + candle overlay diagnostics (history ranges, buffer size, ms/px).

- `SENTINEL_GUI_API_PORT` (client)  
  Local GUI-only HTTP API port for screenshot capture. Default: `17100`. Set to `0` to disable.
- `SENTINEL_GUI_SCREENSHOT_DIR` (client)  
  Output directory for GUI screenshot captures. Default: `./screenshots`.

- `SENTINEL_LOG_App_INTERVAL`  
- `SENTINEL_LOG_Data_INTERVAL`  
- `SENTINEL_LOG_Render_INTERVAL`  
- `SENTINEL_LOG_Debug_INTERVAL`  
  Per-category log throttle interval overrides (positive integer).

## Qt/Graphics (set by app)

- `QSG_RHI_BACKEND`  
  Forced by the GUI entrypoint to `d3d11`, `metal`, or `opengl` depending on OS.
- `QSG_RENDER_LOOP`  
  Forced to `threaded` by the GUI entrypoint.
- `QT_QPA_PLATFORM`  
  On Linux, defaults to `xcb` in the GUI entrypoint when unset to avoid Wayland dock glitches.
