# Sentinel Environment Variables

This is the authoritative list of runtime environment variables read or set by Sentinel.
Defaults shown are the in-code fallbacks when the variable is unset or invalid.

## Sentinel (core/gui)

- `SENTINEL_SERVER_MODE` (server)  
  Enables server mode when set (server entrypoint sets this to `1`).
- `SENTINEL_SSL_CA_BUNDLE` (server)  
  Path to a custom CA bundle for TLS verification.

- `SENTINEL_HEATMAP_TF` (server + client)  
  Heatmap timeframe in milliseconds. Default: `100`. Must match on both.
- `SENTINEL_HEATMAP_GRID` (server)  
  Heatmap grid height. Default: `2048`. Client uses server-provided grid size.
- `SENTINEL_HEATMAP_TICK_SIZE` (server)  
  Default tick size for heatmap grid. Default: `1.0`.
- `SENTINEL_HEATMAP_RECENTER_DELTA` (server)  
  Recenter threshold as fraction of mid price. Default: `0.01`.
- `SENTINEL_HEATMAP_BAND_FAST` (server)  
  Band percent for fast timeframes. Default: `0.15`.
- `SENTINEL_HEATMAP_BAND_MED` (server)  
  Band percent for medium timeframes. Default: `0.25`.
- `SENTINEL_HEATMAP_BAND_SLOW` (server)  
  Band percent for slow timeframes. Default: `0.35`.
- `SENTINEL_HEATMAP_INTENSITY_FLOOR` (server)  
  Minimum intensity cutoff in [0.0, 1.0]. Default: `0.01`.
- `SENTINEL_HEATMAP_RECENTER` (client)  
  Recenter fraction for client processing. Default: `0.15`.
- `SENTINEL_HEATMAP_SLICE_LOG` (server + client)  
  Enables periodic heatmap slice logging.
- `SENTINEL_HEALTH_PORT` (server)  
  Local health endpoint port for `/ping`. Default: `8090`.

- `SENTINEL_GPU_HEATMAP` (client)  
  Enables GPU heatmap path when set.
- `SENTINEL_GPU_HEATMAP_DEBUG` (client)  
  Enables verbose GPU heatmap diagnostics.
- `SENTINEL_GPU_HEATMAP_FORCE_FULL` (client)  
  Forces full texture render rather than visible range.
- `SENTINEL_DUMP_GLYPH_ATLAS` (client)  
  Dumps glyph atlas textures for debugging.

- `SENTINEL_ORDERBOOK_TICK_SIZE` (server + client)  
  Default order book tick size. Default: `0.10`.
- `SENTINEL_ORDERBOOK_BAND_PCT` (server + client)  
  Default order book band percent. Default: `0.30`.

- `SENTINEL_QML_PATH` (client)  
  Overrides QML path used by the GUI.
- `SENTINEL_ZOOM_DEBUG` (client)  
  Enables zoom debug logging.

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
