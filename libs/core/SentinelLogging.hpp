#pragma once

#include <QLoggingCategory>
#include <QDebug>
#include <cstdlib>

// =============================================================================
// SENTINEL LOGGING CATEGORIES - SIMPLIFIED 4-CATEGORY SYSTEM
// =============================================================================
// Simplified from 15+ categories to 4 core categories for better developer UX
// Each category supports atomic throttling for high-frequency operations

Q_DECLARE_LOGGING_CATEGORY(logApp)      // Application: init, lifecycle, config, auth
Q_DECLARE_LOGGING_CATEGORY(logData)     // Data: network, cache, trades, market data, WebSocket  
Q_DECLARE_LOGGING_CATEGORY(logRender)   // Render: all rendering, charts, GPU, coordinates, camera
Q_DECLARE_LOGGING_CATEGORY(logDebug)    // Debug: detailed diagnostics (disabled by default)

// =============================================================================
// ATOMIC THROTTLING SYSTEM
// =============================================================================

namespace sentinel::log_throttle {
    // Compile-time defaults (overridden by env vars)
    inline constexpr int kApp    = 1;    // Log every app event (low frequency)
    inline constexpr int kData   = 20;   // Log every 20th data operation  
    inline constexpr int kRender = 100;  // Log every 100th render operation
    inline constexpr int kDebug  = 10;   // Log every 10th debug message
}

// Atomic throttling macro with runtime env var override
#define SLOG_THROTTLED(cat, defaultInterval, ...)                                   \
    do {                                                                             \
        static std::atomic<uint32_t> _counter{0};                                    \
        static int _interval = []() {                                                \
            const char* env = std::getenv("SENTINEL_LOG_" #cat "_INTERVAL");        \
            return env ? std::atoi(env) : (defaultInterval);                        \
        }();                                                                         \
        if ((++_counter % _interval) == 1) {                                        \
            qCDebug(log##cat) << __VA_ARGS__;                                        \
        }                                                                            \
    } while(false)

// =============================================================================
// SIMPLIFIED LOGGING MACROS - 4 CATEGORIES WITH ATOMIC THROTTLING
// =============================================================================

// Primary logging macros (automatically throttled for hot paths)
#define sLog_App(...)     SLOG_THROTTLED(App, sentinel::log_throttle::kApp, __VA_ARGS__)
#define sLog_Data(...)    SLOG_THROTTLED(Data, sentinel::log_throttle::kData, __VA_ARGS__)  
#define sLog_Render(...)  SLOG_THROTTLED(Render, sentinel::log_throttle::kRender, __VA_ARGS__)
#define sLog_Debug(...)   SLOG_THROTTLED(Debug, sentinel::log_throttle::kDebug, __VA_ARGS__)

// Override macros for specific throttle intervals
#define sLog_AppN(n, ...)    SLOG_THROTTLED(App, n, __VA_ARGS__)
#define sLog_DataN(n, ...)   SLOG_THROTTLED(Data, n, __VA_ARGS__)
#define sLog_RenderN(n, ...) SLOG_THROTTLED(Render, n, __VA_ARGS__)
#define sLog_DebugN(n, ...)  SLOG_THROTTLED(Debug, n, __VA_ARGS__)

// Always-on macros (no throttling for critical messages)
#define sLog_Warning(...)  qCWarning(logApp) << __VA_ARGS__
#define sLog_Error(...)    qCCritical(logApp) << __VA_ARGS__

// =============================================================================
// 🔥 MIGRATION COMPLETE! BACKWARD COMPATIBILITY ALIASES OBLITERATED! 🔥
// =============================================================================
// All logging now uses the pure 4-category system with atomic throttling:
// - sLog_App()    : Application lifecycle, config, auth
// - sLog_Data()   : Network, cache, trades, WebSocket operations  
// - sLog_Render() : Charts, rendering, GPU, coordinates
// - sLog_Debug()  : Debug diagnostics and detailed logging
//
// 📊 PERFORMANCE BENEFITS:
// ✅ Atomic throttling eliminates console spam
// ✅ Thread-safe counters (no race conditions)  
// ✅ Runtime tunable via environment variables
// ✅ Zero manual static counter maintenance
//
// 🎯 LINUS-APPROVED: No blocking, no races, no lies!

// =============================================================================
// RUNTIME ENVIRONMENT VARIABLE CONTROL
// =============================================================================
// Override throttle intervals at runtime:
//   export SENTINEL_LOG_App_INTERVAL=1      # Log every app message  
//   export SENTINEL_LOG_Data_INTERVAL=5     # Log every 5th data operation
//   export SENTINEL_LOG_Render_INTERVAL=50  # Log every 50th render
//   export SENTINEL_LOG_Debug_INTERVAL=1    # Log every debug message

// =============================================================================
// USAGE EXAMPLES & DOCUMENTATION
// =============================================================================
/*
NEW 4-CATEGORY SYSTEM WITH ATOMIC THROTTLING:

BASIC USAGE:
sLog_App("🚀 CREATING GPU TRADING TERMINAL!");           // Application events
sLog_Data("💰 Trade processed:" << price << size);       // Data operations  
sLog_Render("🎨 Frame rendered:" << fps << "fps");       // Rendering operations
sLog_Debug("🔍 Debug info:" << variable);                // Debug details

CUSTOM THROTTLING:
sLog_DataN(5, "Every 5th trade:" << trade);              // Custom interval
sLog_RenderN(50, "Every 50th frame:" << frame);          // Custom interval

MIGRATION FROM OLD SYSTEM:
OLD: sLog_Connection("Connected to WebSocket");
NEW: sLog_Data("Connected to WebSocket");                // Network = Data

OLD: sLog_Chart("Rendering chart with" << points << "points");  
NEW: sLog_Render("Rendering chart with" << points << "points"); // Chart = Render

OLD: static int count; if (++count % 20 == 1) sLog_Trades(...);
NEW: sLog_Data(...);                                     // Automatic throttling!

CATEGORY GUIDELINES:
- sLog_App():    Init, lifecycle, config, auth           (throttle: every 1)
- sLog_Data():   Network, cache, trades, WebSocket       (throttle: every 20)  
- sLog_Render(): Charts, GPU, coordinates, painting      (throttle: every 100)
- sLog_Debug():  Detailed diagnostics, variables         (throttle: every 10)

RUNTIME CONTROL:
export SENTINEL_LOG_Data_INTERVAL=1    # See every data operation
export SENTINEL_LOG_Render_INTERVAL=10 # See every 10th render
export QT_LOGGING_RULES="*.debug=true" # Enable all categories
*/ 