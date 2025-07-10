#!/bin/bash

# Sentinel Fractal Zoom Testing Script
echo "🎯 FRACTAL ZOOM COMPREHENSIVE TESTING"
echo "======================================"
echo ""

# Check if test data exists
TEST_DATA_FILE="logs/test_orderbook_data.json"

if [ ! -f "$TEST_DATA_FILE" ]; then
    echo "📁 Test data not found. Generating..."
    echo ""
    
    # Generate test data
    echo "🚀 STEP 1: Generating Test Order Book Data"
    echo "----------------------------------------"
    python3 scripts/generate_test_orderbook.py
    
    if [ $? -eq 0 ]; then
        echo "✅ Test data generation complete!"
    else
        echo "❌ Test data generation failed!"
        exit 1
    fi
else
    echo "✅ Test data found: $TEST_DATA_FILE"
fi

echo ""
echo "🔧 STEP 2: Build Application with Test Data Player"
echo "-------------------------------------------------"
echo "Building Sentinel with TestDataPlayer..."
cmake --build build --parallel 4

if [ $? -eq 0 ]; then
    echo "✅ Build successful!"
else
    echo "❌ Build failed!"
    exit 1
fi

echo ""
echo "🧪 STEP 3: Fractal Zoom Testing Instructions"
echo "--------------------------------------------"
echo ""
echo "🎯 WHAT WAS IMPLEMENTED THIS SESSION:"
echo "   ✅ FractalZoomController coordination system"
echo "   ✅ LOD signal emission from CandlestickBatched"
echo "   ✅ Heatmap cleanup throttling (1000x reduction)"
echo "   ✅ TestDataPlayer for realistic order book replay"
echo "   ✅ Python test data generator (1 hour of data)"
echo ""
echo "🔍 WHAT TO TEST:"
echo "   1. Start the application and load test data"
echo "   2. Watch for these logs:"
echo "      '🎯 FRACTAL ZOOM: TimeFrame changed to...'"
echo "      '🎯 FRACTAL ZOOM: LOD signal emitted...'"
echo "      '🎯 FRACTAL ZOOM: Connected depth aggregation...'"
echo "   3. Zoom out and verify aggregation working"
echo "   4. Monitor memory usage (should stay < 50MB)"
echo ""
echo "🚀 NEXT DEVELOPMENT PRIORITIES:"
echo "   1. Implement parallel aggregation buffers (Model B)"
echo "   2. Volume-weighted aggregation for longer timeframes"
echo "   3. Smart decimation during rendering"
echo ""
echo "🎮 TESTING COMMANDS:"
echo "   # Start with test data:"
echo "   ./build/apps/sentinel_gui/sentinel"
echo ""
echo "   # Monitor specific logs:"
echo "   source scripts/log-modes.sh && log-fractal"
echo "   ./build/apps/sentinel_gui/sentinel | grep 'FRACTAL ZOOM'"
echo ""
echo "   # Test at different speeds:"
echo "   # - Normal speed: 100ms intervals"
echo "   # - Fast test: 10ms intervals (10x speed)"
echo ""

# File size information
if [ -f "$TEST_DATA_FILE" ]; then
    FILE_SIZE=$(du -h "$TEST_DATA_FILE" | cut -f1)
    echo "📊 TEST DATA STATS:"
    echo "   File: $TEST_DATA_FILE"
    echo "   Size: $FILE_SIZE"
    echo "   Contains: 36,000 order book snapshots (1 hour @ 100ms)"
    echo ""
fi

echo "🎯 READY FOR FRACTAL ZOOM TESTING!"
echo ""
echo "💡 TIP: The test data covers all timeframes:"
echo "   100ms → 500ms → 1sec → 1min → 5min → 15min → 60min"
echo "   Perfect for testing aggregation transitions!" 