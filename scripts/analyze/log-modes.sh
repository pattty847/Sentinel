#!/bin/bash

# Sentinel Logging Mode Switcher
# Usage: source scripts/log-modes.sh && log-production

# 🎯 PRODUCTION MODE - Clean and professional (5-10 lines)
log-production() {
    export QT_LOGGING_RULES="*.debug=false;*.warning=true;*.critical=true"
    echo "🎯 PRODUCTION MODE: Only warnings and errors"
    echo "   Rules: $QT_LOGGING_RULES"
}

# 🚀 DEVELOPMENT MODE - Everything enabled (200+ lines)  
log-development() {
    export QT_LOGGING_RULES="sentinel.*.debug=true"
    echo "🚀 DEVELOPMENT MODE: All categories enabled"
    echo "   Rules: $QT_LOGGING_RULES"
}

# 💰 TRADING FOCUS - Debug trading issues
log-trading() {
    export QT_LOGGING_RULES="sentinel.trades.debug=true;sentinel.cache.debug=true;sentinel.network.debug=true;sentinel.gpu.debug=true"
    echo "💰 TRADING FOCUS: Trade processing, data flow, GPU pipeline"
    echo "   Rules: $QT_LOGGING_RULES"
}

# 🎨 RENDERING FOCUS - Debug visual issues
log-rendering() {
    export QT_LOGGING_RULES="sentinel.chart.debug=true;sentinel.candles.debug=true;sentinel.render.debug=true;sentinel.camera.debug=true"
    echo "🎨 RENDERING FOCUS: Charts, candles, basic rendering, camera"
    echo "   Rules: $QT_LOGGING_RULES"
}

# ⚡ PERFORMANCE FOCUS - Debug performance issues
log-performance() {
    export QT_LOGGING_RULES="sentinel.performance.debug=true;sentinel.debug.timing.debug=true"
    echo "⚡ PERFORMANCE FOCUS: Performance metrics and timing"
    echo "   Rules: $QT_LOGGING_RULES"
}

# 🔌 NETWORK FOCUS - Debug connection issues
log-network() {
    export QT_LOGGING_RULES="sentinel.network.debug=true;sentinel.connection.debug=true;sentinel.subscription.debug=true"
    echo "🔌 NETWORK FOCUS: WebSocket, connections, subscriptions"
    echo "   Rules: $QT_LOGGING_RULES"
}

# 🔍 DEEP DEBUG - Enable detailed debugging (VERY VERBOSE!)
log-deep() {
    export QT_LOGGING_RULES="sentinel.*.debug=true;sentinel.debug.*.debug=true;sentinel.render.detail.debug=true"
    echo "🔍 DEEP DEBUG: All categories + detailed debugging (VERY VERBOSE!)"
    echo "   Rules: $QT_LOGGING_RULES"
}

# 🧹 CLEAN MODE - Minimal spam, keep useful logs
log-clean() {
    export QT_LOGGING_RULES="sentinel.render.detail.debug=false;sentinel.debug.*.debug=false"
    echo "🧹 CLEAN MODE: Disable high-frequency spam, keep useful logs"
    echo "   Rules: $QT_LOGGING_RULES"
}

# 📋 HELP - Show available modes
log-help() {
    echo "🎛️  SENTINEL LOGGING MODES"
    echo "=========================="
    echo ""
    echo "📋 Usage: source scripts/log-modes.sh && log-<mode>"
    echo ""
    echo "🎯 log-production   - Only errors/warnings (5-10 lines)"
    echo "🚀 log-development  - All categories (200+ lines)"
    echo "💰 log-trading      - Trade processing & data flow"
    echo "🎨 log-rendering    - Charts, candles, basic rendering"
    echo "⚡ log-performance  - Performance metrics & timing"
    echo "🔌 log-network      - WebSocket & connections"
    echo "🔍 log-deep         - Everything + detailed debug (VERY VERBOSE!)"
    echo "🧹 log-clean        - Disable spam, keep useful logs"
    echo ""
    echo "💡 Then run: ./build/apps/sentinel_gui/sentinel"
    echo ""
    echo "🔄 Current setting: $QT_LOGGING_RULES"
}

# Show help by default
log-help 