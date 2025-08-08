#!/bin/bash

# 🔥 SENTINEL POLLING OPTIMIZATION SCRIPT
# ========================================
# This script helps optimize order book polling rates and test different configurations
# to maximize throughput while maintaining stability.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "🔥 SENTINEL POLLING OPTIMIZATION"
echo "================================"
echo "📍 Project root: $PROJECT_ROOT"
echo ""

# Function to update polling rate in StreamController
update_polling_rate() {
    local rate_ms=$1
    local file="$PROJECT_ROOT/libs/gui/streamcontroller.cpp"
    
    echo "🔧 Setting order book polling to ${rate_ms}ms..."
    
    # Update the order book polling rate
    sed -i.bak "s/m_orderBookPollTimer->start([0-9]*);/m_orderBookPollTimer->start($rate_ms);/" "$file"
    
    # Also update trade polling for consistency
    sed -i.bak2 "s/m_pollTimer->start([0-9]*);/m_pollTimer->start($rate_ms);/" "$file"
    
    echo "✅ Updated polling rates to ${rate_ms}ms"
}

# Function to build and test
build_and_test() {
    local rate_ms=$1
    
    echo "🔨 Building with ${rate_ms}ms polling..."
    cd "$PROJECT_ROOT"
    
    # Build the project
    cmake --build --preset=default --target sentinel > /dev/null 2>&1
    
    if [ $? -eq 0 ]; then
        echo "✅ Build successful with ${rate_ms}ms polling"
        return 0
    else
        echo "❌ Build failed with ${rate_ms}ms polling"
        return 1
    fi
}

# Function to run firehose test
run_firehose_test() {
    echo "🚀 Running Coinbase firehose test..."
    cd "$PROJECT_ROOT"
    
    # Build the firehose test
    cmake --build --preset=default --target test_coinbase_firehose > /dev/null 2>&1
    
    if [ $? -eq 0 ]; then
        echo "📊 Running firehose benchmark..."
        ./build/tests/test_coinbase_firehose 2>/dev/null | grep -E "(SCENARIO|Latency|Throughput|Status)"
    else
        echo "❌ Failed to build firehose test"
    fi
}

# Function to restore original settings
restore_original() {
    local file="$PROJECT_ROOT/libs/gui/streamcontroller.cpp"
    
    if [ -f "$file.bak" ]; then
        mv "$file.bak" "$file"
        echo "🔄 Restored original polling settings"
    fi
    
    if [ -f "$file.bak2" ]; then
        rm "$file.bak2"
    fi
}

# Main optimization test
optimize_polling() {
    echo "🎯 TESTING DIFFERENT POLLING RATES"
    echo "=================================="
    echo ""
    
    # Test different polling rates (in milliseconds)
    local rates=(100 50 25 10 5 1)
    
    for rate in "${rates[@]}"; do
        echo "📊 Testing ${rate}ms polling rate..."
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        
        update_polling_rate $rate
        
        if build_and_test $rate; then
            echo "✅ ${rate}ms: Build successful"
            
            # Calculate theoretical message rate
            local msgs_per_sec=$((1000 / rate))
            echo "📈 Theoretical max: ${msgs_per_sec} updates/second"
            
            if [ $rate -le 10 ]; then
                echo "⚡ HIGH FREQUENCY: Suitable for active trading"
            elif [ $rate -le 50 ]; then
                echo "📊 MEDIUM FREQUENCY: Good balance of performance/stability"
            else
                echo "🐌 LOW FREQUENCY: Conservative, may miss fast market moves"
            fi
        else
            echo "❌ ${rate}ms: Build failed"
        fi
        
        echo ""
    done
    
    restore_original
}

# Function to show current settings
show_current_settings() {
    local file="$PROJECT_ROOT/libs/gui/streamcontroller.cpp"
    
    echo "📋 CURRENT POLLING CONFIGURATION"
    echo "================================"
    
    if [ -f "$file" ]; then
        echo "📁 File: $file"
        echo ""
        
        # Extract current polling rates
        local ob_rate=$(grep -o "m_orderBookPollTimer->start([0-9]*)" "$file" | grep -o "[0-9]*" || echo "NOT_FOUND")
        local trade_rate=$(grep -o "m_pollTimer->start([0-9]*)" "$file" | grep -o "[0-9]*" || echo "NOT_FOUND")
        
        if [ "$ob_rate" != "NOT_FOUND" ]; then
            echo "📊 Order Book Polling: ${ob_rate}ms ($(( 1000 / ob_rate )) updates/sec max)"
        else
            echo "❌ Order Book Polling: Not found in source"
        fi
        
        if [ "$trade_rate" != "NOT_FOUND" ]; then
            echo "💰 Trade Polling: ${trade_rate}ms ($(( 1000 / trade_rate )) updates/sec max)"
        else
            echo "❌ Trade Polling: Not found in source"
        fi
        
        echo ""
        
        # Show the actual lines
        echo "🔍 Source lines:"
        grep -n "PollTimer->start" "$file" || echo "No polling timers found"
    else
        echo "❌ StreamController source file not found!"
    fi
}

# Function to set optimal rate
set_optimal_rate() {
    local rate=${1:-10}
    
    echo "🎯 SETTING OPTIMAL POLLING RATE"
    echo "==============================="
    echo "🔧 Setting both timers to ${rate}ms..."
    echo ""
    
    update_polling_rate $rate
    
    if build_and_test $rate; then
        echo ""
        echo "✅ SUCCESS: Optimal ${rate}ms polling configured!"
        echo "📈 Theoretical throughput: $(( 1000 / rate )) updates/second"
        echo "⚡ This rate provides a good balance of performance and stability."
        echo ""
        echo "🚀 To test the new rate:"
        echo "   cd $PROJECT_ROOT"
        echo "   ./build/apps/sentinel_gui/sentinel"
        echo ""
    else
        echo "❌ Failed to configure ${rate}ms polling"
        restore_original
    fi
}

# Usage function
show_usage() {
    echo "Usage: $0 [COMMAND] [OPTIONS]"
    echo ""
    echo "Commands:"
    echo "  optimize          Test different polling rates and show recommendations"
    echo "  show              Show current polling configuration"
    echo "  set <rate>        Set specific polling rate in milliseconds (default: 10)"
    echo "  restore           Restore original polling settings"
    echo "  firehose          Run FastOrderBook firehose stress test"
    echo ""
    echo "Examples:"
    echo "  $0 optimize       # Test rates from 100ms down to 1ms"
    echo "  $0 set 5          # Set 5ms polling (200 updates/sec)"
    echo "  $0 show           # Show current settings"
    echo "  $0 firehose       # Run stress test"
    echo ""
}

# Main script logic
case "${1:-}" in
    "optimize")
        optimize_polling
        ;;
    "show")
        show_current_settings
        ;;
    "set")
        set_optimal_rate "${2:-10}"
        ;;
    "restore")
        restore_original
        echo "✅ Original settings restored"
        ;;
    "firehose")
        run_firehose_test
        ;;
    "help"|"-h"|"--help")
        show_usage
        ;;
    "")
        echo "🔥 No command specified. Use 'help' for usage information."
        echo ""
        show_current_settings
        ;;
    *)
        echo "❌ Unknown command: $1"
        echo ""
        show_usage
        exit 1
        ;;
esac 