#include "SentinelLogging.hpp"

// =============================================================================
// LOGGING CATEGORY DEFINITIONS
// =============================================================================

// CORE COMPONENTS
Q_LOGGING_CATEGORY(logCore, "sentinel.core")
Q_LOGGING_CATEGORY(logNetwork, "sentinel.network")  
Q_LOGGING_CATEGORY(logCache, "sentinel.cache")
Q_LOGGING_CATEGORY(logPerformance, "sentinel.performance")

// GUI COMPONENTS  
Q_LOGGING_CATEGORY(logRender, "sentinel.render")
Q_LOGGING_CATEGORY(logRenderDetail, "sentinel.render.detail")
Q_LOGGING_CATEGORY(logChart, "sentinel.chart")
Q_LOGGING_CATEGORY(logCandles, "sentinel.candles")
Q_LOGGING_CATEGORY(logTrades, "sentinel.trades")
Q_LOGGING_CATEGORY(logCamera, "sentinel.camera")
Q_LOGGING_CATEGORY(logGPU, "sentinel.gpu")

// INITIALIZATION & LIFECYCLE
Q_LOGGING_CATEGORY(logInit, "sentinel.init")
Q_LOGGING_CATEGORY(logConnection, "sentinel.connection")
Q_LOGGING_CATEGORY(logSubscription, "sentinel.subscription")

// DEBUGGING CATEGORIES
Q_LOGGING_CATEGORY(logDebugCoords, "sentinel.debug.coords")
Q_LOGGING_CATEGORY(logDebugGeometry, "sentinel.debug.geometry")
Q_LOGGING_CATEGORY(logDebugTiming, "sentinel.debug.timing")
Q_LOGGING_CATEGORY(logDebugData, "sentinel.debug.data")

// =============================================================================
// THROTTLED LOGGER INSTANCES  
// =============================================================================

// Global throttled loggers for high-frequency operations
ThrottledLogger<20> gTradeLogger;        // Log every 20th trade
ThrottledLogger<100> gRenderLogger;      // Log every 100th render
ThrottledLogger<50> gCoordLogger;        // Log every 50th coordinate calc
ThrottledLogger<30> gCacheLogger;        // Log every 30th cache access 