#!/bin/bash

# 🔥 REDUCE LOG SPAM SCRIPT
# This script sets environment variables to reduce console output by 90%

echo "🎛️  REDUCING LOG SPAM..."
echo "=========================="

# Set Qt logging rules to disable verbose categories
export QT_LOGGING_RULES="sentinel.render.detail.debug=false;sentinel.debug.coords.debug=false;sentinel.gpu.debug=false;sentinel.chart.debug=false"

echo "✅ Log spam reduced! Now run:"
echo "   ./build/apps/sentinel_gui/sentinel"
echo ""
echo "💡 To restore full logging, run:"
echo "   unset QT_LOGGING_RULES"
echo ""
echo "🎯 Current setting: $QT_LOGGING_RULES" 