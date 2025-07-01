// =============================================================================
// SENTINEL LOGGING MIGRATION DEMONSTRATION
// =============================================================================
// This file shows before/after examples of the logging migration

#include "SentinelLogging.hpppp"
#include <QCoreApplication>
#include <QDebug>

void demonstrateOldLogging() {
    qDebug() << "=== OLD LOGGING SYSTEM (CHAOS) ===";
    
    // Example of current high-frequency spam
    for (int i = 0; i < 5; ++i) {
        qDebug() << "🕯️ CANDLE RENDER UPDATE #" << i << "LOD: 1sec Total: 42";
        qDebug() << "✅ SCENE GRAPH VALIDATION PASSED: Returning valid node structure";
        qDebug() << "🎨 PAINT NODE UPDATE #" << i << "Widget size: 1360 x 774";
        qDebug() << "🗺️ UNIFIED COORD CALC #" << i << "TS: 1751262838000 P: 108260";
    }
    
    // This creates 20 lines of spam for just 5 iterations!
    qDebug() << "💰 Result: 20 log lines in 5 iterations = SPAM";
}

void demonstrateNewLogging() {
    qDebug() << "\n=== NEW LOGGING SYSTEM (ORGANIZED) ===";
    
    // High-frequency rendering logs (can be disabled via environment)
    for (int i = 0; i < 5; ++i) {
        sLog_Render() << "🕯️ CANDLE RENDER UPDATE #" << i << "LOD: 1sec Total: 42";
        sLog_RenderDetail() << "✅ SCENE GRAPH VALIDATION PASSED: Returning valid node";
        sLog_DebugTiming() << "🎨 PAINT NODE UPDATE #" << i << "Widget size: 1360 x 774";
        sLog_DebugCoords() << "🗺️ UNIFIED COORD CALC #" << i << "TS: 1751262838000 P: 108260";
    }
    
    qDebug() << "💰 Result: Same logs, but now controllable via environment variables!";
}

void demonstrateThrottling() {
    qDebug() << "\n=== THROTTLED LOGGING DEMONSTRATION ===";
    
    // Simulate high-frequency trade processing
    for (int i = 1; i <= 25; ++i) {
        // Old way: Manual throttling with static counters
        static int oldCount = 0;
        if (++oldCount % 20 == 1) {
            qDebug() << "💰 OLD WAY: BTC-USD trade #" << i << "[call #" << oldCount << "]";
        }
        
        // New way: Clean throttled logging
        gTradeLogger.log(logTrades, QString("💰 NEW WAY: BTC-USD trade #%1").arg(i));
    }
    
    qDebug() << "💰 Result: Throttled loggers automatically manage frequency";
}

void demonstrateCategoryControl() {
    qDebug() << "\n=== CATEGORY CONTROL DEMONSTRATION ===";
    qDebug() << "Set these environment variables to control output:";
    qDebug() << "";
    qDebug() << "# Enable only trades and charts:";
    qDebug() << "export QT_LOGGING_RULES=\"sentinel.trades.debug=true;sentinel.chart.debug=true\"";
    qDebug() << "";
    qDebug() << "# Disable high-frequency rendering spam:";  
    qDebug() << "export QT_LOGGING_RULES=\"sentinel.render.detail.debug=false;sentinel.debug.*.debug=false\"";
    qDebug() << "";
    qDebug() << "# Production mode (errors only):";
    qDebug() << "export QT_LOGGING_RULES=\"*.debug=false;*.warning=true;*.critical=true\"";
    
    // Test different categories
    sLog_Init() << "🚀 This is an initialization log (one-time only)";
    sLog_Network() << "🔌 This is a network operation log";
    sLog_Cache() << "🔍 This is a cache access log";
    sLog_Trades() << "💰 This is a trade processing log";
    sLog_Performance() << "📊 This is a performance metric log";
    
    // Debug categories (disabled by default)
    sLog_DebugCoords() << "🗺️ This debug coord log won't show unless enabled";
    sLog_DebugGeometry() << "🏗️ This debug geometry log won't show unless enabled";
    
    // Warnings and errors (always shown)
    sLog_Warning() << "⚠️ This warning will always show";
    sLog_Error() << "❌ This error will always show";
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    
    qDebug() << "🚀 SENTINEL LOGGING MIGRATION DEMONSTRATION";
    qDebug() << "============================================";
    
    demonstrateOldLogging();
    demonstrateNewLogging();
    demonstrateThrottling();
    demonstrateCategoryControl();
    
    qDebug() << "";
    qDebug() << "✅ DEMONSTRATION COMPLETE";
    qDebug() << "Now you can control logging via QT_LOGGING_RULES environment variable!";
    
    return 0;
}

// =============================================================================
// EXPECTED OUTPUT PATTERNS
// =============================================================================

/*
DEFAULT OUTPUT (all categories enabled):
- Shows init, network, cache, trades, performance logs
- Hides debug.* categories (coords, geometry, timing)
- Always shows warnings and errors

PRODUCTION OUTPUT (QT_LOGGING_RULES="*.debug=false;*.warning=true;*.critical=true"):
- Only shows warnings and errors
- Massive reduction in log spam

COMPONENT-SPECIFIC (QT_LOGGING_RULES="sentinel.trades.debug=true;sentinel.performance.debug=true"):
- Only shows trade and performance logs
- Perfect for focused debugging

HIGH-FREQUENCY DISABLED (QT_LOGGING_RULES="sentinel.render.debug=false;sentinel.debug.*.debug=false"):
- Disables paint cycle spam 
- Keeps useful operational logs
- Best balance for development
*/ 