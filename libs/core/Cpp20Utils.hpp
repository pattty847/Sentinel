#pragma once

// 🚀 C++20 UTILITIES FOR SENTINEL TRADING APPLICATION
// Optimized helper functions for high-performance real-time trading

#include <string>
#include <string_view>
#include <format>
#include <stdexcept>
#include "TradeData.h"
#include <charconv>
#include <cstdlib>

namespace Cpp20Utils {
/**
 * Cpp20Utils provides high-performance trading utilities:
 * - fastStringToDouble/Int: Optimized string-to-number conversion with error handling
 * - fastSideDetection: Efficient trade side detection (Buy/Sell/Unknown)
 * - formatTradeLog/OrderBookLog: Fast logging message formatting for trades and order books
 * - formatErrorLog/SuccessLog: Error and success message formatting
 * - formatPerformanceMetric/Throughput: Performance monitoring and throughput calculation
 * All functions optimized for real-time trading data processing with minimal overhead.
 */

// 🚀 FAST STRING-TO-NUMBER CONVERSION
// Optimized for real-time trading data processing

/**
 * Fast string-to-double conversion with error handling
 * @param str Input string to convert
 * @return Converted double value, or 0.0 on error
 */
inline double fastStringToDouble(std::string_view str) {
    // Fallback: std::from_chars for double may not be available on all platforms/stdlibs
    // Use std::strtod for fast, non-throwing conversion
    const char* begin = str.data();
    char* end = nullptr;
    double value = std::strtod(begin, &end);
    if (end == begin) return 0.0; // Conversion failed
    return value;
}

/**
 * Fast string-to-double conversion with default value
 * @param str Input string to convert
 * @param defaultValue Value to return if conversion fails
 * @return Converted double value, or defaultValue on error
 */
inline double fastStringToDouble(std::string_view str, double defaultValue) {
    if (str.empty()) return defaultValue;
    const char* begin = str.data();
    char* end = nullptr;
    double value = std::strtod(begin, &end);
    if (end == begin) return defaultValue;
    return value;
}

/**
 * Fast string-to-int conversion with error handling
 * @param str Input string to convert
 * @return Converted int value, or 0 on error
 */
inline int fastStringToInt(std::string_view str) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
    if (ec == std::errc()) {
        return value;
    }
    return 0;
}

/**
 * Fast string-to-int conversion with default value
 * @param str Input string to convert
 * @param defaultValue Value to return if conversion fails
 * @return Converted int value, or defaultValue on error
 */
inline int fastStringToInt(std::string_view str, int defaultValue) {
    if (str.empty()) return defaultValue;
    int value = 0;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
    if (ec == std::errc()) {
        return value;
    }
    return defaultValue;
}

// 🚀 FAST SIDE DETECTION
// Optimized for trade processing

/**
 * Fast side detection using string_view for efficiency
 * @param side String representing trade side ("BUY", "SELL", etc.)
 * @return AggressorSide enum value
 */
inline AggressorSide fastSideDetection(std::string_view side) {
    if (side == "BUY") return AggressorSide::Buy;
    if (side == "SELL") return AggressorSide::Sell;
    return AggressorSide::Unknown;
}

/**
 * Fast side detection with custom strings
 * @param side String representing trade side
 * @param buyStr String for buy side (default: "BUY")
 * @param sellStr String for sell side (default: "SELL")
 * @return AggressorSide enum value
 */
inline AggressorSide fastSideDetection(const std::string& side, 
                                      const std::string& buyStr = "BUY", 
                                      const std::string& sellStr = "SELL") {
    if (side == buyStr) return AggressorSide::Buy;
    if (side == sellStr) return AggressorSide::Sell;
    return AggressorSide::Unknown;
}

// 🚀 FAST STRING FORMATTING
// Optimized logging and message formatting

/**
 * Fast trade logging message formatting
 * @param productId Product identifier
 * @param price Trade price
 * @param size Trade size
 * @param side Trade side
 * @param tradeCount Total trade count
 * @return Formatted log message
 */
inline std::string formatTradeLog(const std::string& productId, 
                                 double price, 
                                 double size, 
                                 const std::string& side, 
                                 int tradeCount) {
    return std::format("💰 {}: ${:.2f} size:{:.6f} ({}) [{} trades total]",
        productId, price, size, side, tradeCount);
}

/**
 * Fast order book logging message formatting
 * @param productId Product identifier
 * @param bidCount Number of bid levels
 * @param askCount Number of ask levels
 * @param updateCount Number of updates (optional)
 * @return Formatted log message
 */
inline std::string formatOrderBookLog(const std::string& productId, 
                                     size_t bidCount, 
                                     size_t askCount, 
                                     int updateCount = -1) {
    if (updateCount >= 0) {
        return std::format("📸 ORDER BOOK {}: {} bids, {} asks (+{} changes)",
            productId, bidCount, askCount, updateCount);
    } else {
        return std::format("📸 ORDER BOOK {}: {} bids, {} asks",
            productId, bidCount, askCount);
    }
}

/**
 * Fast error message formatting
 * @param context Error context
 * @param message Error message
 * @return Formatted error message
 */
inline std::string formatErrorLog(const std::string& context, 
                                 const std::string& message) {
    return std::format("❌ {}: {}", context, message);
}

/**
 * Fast success message formatting
 * @param context Success context
 * @param message Success message
 * @return Formatted success message
 */
inline std::string formatSuccessLog(const std::string& context, 
                                   const std::string& message) {
    return std::format("✅ {}: {}", context, message);
}

// 🚀 PERFORMANCE MONITORING
// Utilities for tracking performance in real-time systems

/**
 * Format performance metric
 * @param metricName Name of the metric
 * @param value Metric value
 * @param unit Unit of measurement
 * @return Formatted performance string
 */
inline std::string formatPerformanceMetric(const std::string& metricName, 
                                          double value, 
                                          const std::string& unit = "") {
    if (unit.empty()) {
        return std::format("📊 {}: {:.2f}", metricName, value);
    } else {
        return std::format("📊 {}: {:.2f} {}", metricName, value, unit);
    }
}

/**
 * Format throughput metric
 * @param operationName Name of the operation
 * @param count Number of operations
 * @param timeMs Time in milliseconds
 * @return Formatted throughput string
 */
inline std::string formatThroughput(const std::string& operationName, 
                                   int count, 
                                   double timeMs) {
    double opsPerSec = (timeMs > 0) ? (count * 1000.0 / timeMs) : 0.0;
    return std::format("⚡ {}: {} ops in {:.1f}ms ({:.0f} ops/sec)",
        operationName, count, timeMs, opsPerSec);
}

} // namespace Cpp20Utils 