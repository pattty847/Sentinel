#include "SentinelLogging.hpp"

// =============================================================================
// SIMPLIFIED 4-CATEGORY LOGGING SYSTEM DEFINITIONS
// =============================================================================

Q_LOGGING_CATEGORY(logApp, "sentinel.app")         // Application: init, lifecycle, config, auth
Q_LOGGING_CATEGORY(logData, "sentinel.data")       // Data: network, cache, trades, market data, WebSocket  
Q_LOGGING_CATEGORY(logRender, "sentinel.render")   // Render: all rendering, charts, GPU, coordinates, camera
Q_LOGGING_CATEGORY(logDebug, "sentinel.debug")     // Debug: detailed diagnostics (disabled by default)

// =============================================================================
// NOTE: Atomic throttling is now built into the macros themselves!
// No need for global ThrottledLogger instances - each macro has its own
// atomic counter and configurable throttling via environment variables.
// ============================================================================= 