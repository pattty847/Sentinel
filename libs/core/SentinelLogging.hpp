#pragma once

#include <QLoggingCategory>
#include <QDebug>

// =============================================================================
// SENTINEL LOGGING CATEGORIES
// =============================================================================
// Organized by component and severity for efficient debugging
// Use Q_LOGGING_CATEGORY environment variables to control output

// CORE COMPONENTS
Q_DECLARE_LOGGING_CATEGORY(logCore)           // Core data structures, auth
Q_DECLARE_LOGGING_CATEGORY(logNetwork)       // WebSocket, connections, subscriptions  
Q_DECLARE_LOGGING_CATEGORY(logCache)         // DataCache operations, trades, order books
Q_DECLARE_LOGGING_CATEGORY(logPerformance)   // Performance monitoring, timing

// GUI COMPONENTS
Q_DECLARE_LOGGING_CATEGORY(logRender)        // High-frequency rendering operations
Q_DECLARE_LOGGING_CATEGORY(logRenderDetail)  // Detailed paint/geometry logging (normally disabled)
Q_DECLARE_LOGGING_CATEGORY(logChart)         // Chart operations, coordinate calculations
Q_DECLARE_LOGGING_CATEGORY(logCandles)       // Candlestick processing, LOD system
Q_DECLARE_LOGGING_CATEGORY(logTrades)        // Trade visualization, color processing
Q_DECLARE_LOGGING_CATEGORY(logCamera)        // Camera pan/zoom operations
Q_DECLARE_LOGGING_CATEGORY(logGPU)           // GPU data adapter, batching

// INITIALIZATION & LIFECYCLE
Q_DECLARE_LOGGING_CATEGORY(logInit)          // Component initialization
Q_DECLARE_LOGGING_CATEGORY(logConnection)    // Connection establishment, cleanup
Q_DECLARE_LOGGING_CATEGORY(logSubscription)  // Market data subscriptions

// DEBUGGING CATEGORIES (disabled by default)
Q_DECLARE_LOGGING_CATEGORY(logDebugCoords)   // Coordinate calculation details
Q_DECLARE_LOGGING_CATEGORY(logDebugGeometry) // Geometry creation details
Q_DECLARE_LOGGING_CATEGORY(logDebugTiming)   // Frame timing, paint cycles
Q_DECLARE_LOGGING_CATEGORY(logDebugData)     // Raw data processing details

// =============================================================================
// CONVENIENCE MACROS FOR COMMON PATTERNS
// =============================================================================

// Replace direct qDebug() calls with categorized versions
#define sLog_Core(...)        qCDebug(logCore) << __VA_ARGS__
#define sLog_Network(...)     qCDebug(logNetwork) << __VA_ARGS__
#define sLog_Cache(...)       qCDebug(logCache) << __VA_ARGS__
#define sLog_Performance(...) qCDebug(logPerformance) << __VA_ARGS__

#define sLog_Render(...)      qCDebug(logRender) << __VA_ARGS__
#define sLog_RenderDetail(...) qCDebug(logRenderDetail) << __VA_ARGS__
#define sLog_Chart(...)       qCDebug(logChart) << __VA_ARGS__
#define sLog_Candles(...)     qCDebug(logCandles) << __VA_ARGS__
#define sLog_Trades(...)      qCDebug(logTrades) << __VA_ARGS__
#define sLog_Camera(...)      qCDebug(logCamera) << __VA_ARGS__
#define sLog_GPU(...)         qCDebug(logGPU) << __VA_ARGS__

#define sLog_Init(...)        qCDebug(logInit) << __VA_ARGS__
#define sLog_Connection(...)  qCDebug(logConnection) << __VA_ARGS__
#define sLog_Subscription(...) qCDebug(logSubscription) << __VA_ARGS__

#define sLog_DebugCoords(...) qCDebug(logDebugCoords) << __VA_ARGS__
#define sLog_DebugGeometry(...) qCDebug(logDebugGeometry) << __VA_ARGS__
#define sLog_DebugTiming(...) qCDebug(logDebugTiming) << __VA_ARGS__
#define sLog_DebugData(...)   qCDebug(logDebugData) << __VA_ARGS__

// Warning and Error macros (always enabled)
#define sLog_Warning(...)     qCWarning(logCore) << __VA_ARGS__
#define sLog_Error(...)       qCCritical(logCore) << __VA_ARGS__

// =============================================================================
// THROTTLED LOGGING HELPERS
// =============================================================================
// Use these for high-frequency operations to reduce spam

template<int N = 50>
class ThrottledLogger {
private:
    mutable int m_count = 0;
    
public:
    // Simple one-argument version for most common use case
    void log(const QLoggingCategory& category, const QString& message) const {
        if (++m_count % N == 1) {
            qCDebug(category) << message << "[call #" << m_count << "]";
        }
    }
    
    // Two-argument version for formatted messages  
    template<typename T>
    void log(const QLoggingCategory& category, const QString& message, const T& value) const {
        if (++m_count % N == 1) {
            qCDebug(category) << message << value << "[call #" << m_count << "]";
        }
    }
};

// Common throttled loggers for high-frequency operations
extern ThrottledLogger<20> gTradeLogger;        // Log every 20th trade
extern ThrottledLogger<100> gRenderLogger;      // Log every 100th render
extern ThrottledLogger<50> gCoordLogger;        // Log every 50th coordinate calc
extern ThrottledLogger<30> gCacheLogger;        // Log every 30th cache access

// =============================================================================
// USAGE EXAMPLES & DOCUMENTATION
// =============================================================================
/*
ENVIRONMENT VARIABLE CONTROL:
Export these to control which categories are enabled:

# Enable all core logging
export QT_LOGGING_RULES="*.debug=true"

# Enable only specific components
export QT_LOGGING_RULES="sentinel.core.debug=true;sentinel.network.debug=true"

# Disable high-frequency categories
export QT_LOGGING_RULES="sentinel.render.detail.debug=false;sentinel.debug.*.debug=false"

# Production mode - only warnings and errors
export QT_LOGGING_RULES="*.debug=false;*.warning=true;*.critical=true"

MIGRATION EXAMPLES:

OLD: qDebug() << "🚀 CREATING GPU TRADING TERMINAL!";
NEW: sLog_Init() << "🚀 CREATING GPU TRADING TERMINAL!";

OLD: qDebug() << "🕯️ CANDLE RENDER UPDATE:" << "LOD:" << timeframe;
NEW: sLog_Candles() << "🕯️ CANDLE RENDER UPDATE:" << "LOD:" << timeframe;

OLD: static int count = 0; if (++count % 20 == 1) qDebug() << "💰 Trade:" << trade;
NEW: gTradeLogger.log(logTrades, "💰 Trade:", trade);

FREQUENCY GUIDELINES:
- logInit, logConnection: One-time events
- logNetwork, logCache: Low frequency (< 10/sec)
- logChart, logCandles: Medium frequency (< 100/sec)  
- logRender: High frequency (> 100/sec) - use throttling
- logRenderDetail, logDebug*: Debug only - disabled in production
*/ 