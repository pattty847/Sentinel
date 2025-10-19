#pragma once

// 🚀 C++20 UTILITIES FOR SENTINEL TRADING APPLICATION
// Optimized helper functions for high-performance real-time trading

#include <string>
#include <string_view>
#include <format>
#include <stdexcept>
#include "marketdata/model/TradeData.h"
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <ctime>

namespace Cpp20Utils {

inline char asciiToLower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c + ('a' - 'A'));
    }
    return static_cast<char>(c);
}

inline bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (asciiToLower(static_cast<unsigned char>(lhs[i])) != asciiToLower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}
/**
 * Cpp20Utils provides high-performance trading utilities:
 * - fastStringToDouble/Int: Optimized string-to-number conversion with error handling
 * - fastSideDetection: Efficient trade side detection (Buy/Sell/Unknown)
 * - parseISO8601: Fast ISO8601 timestamp parsing for exchange data
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
    if (equalsIgnoreCase(side, "BUY")) return AggressorSide::Buy;
    if (equalsIgnoreCase(side, "SELL")) return AggressorSide::Sell;
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
    if (equalsIgnoreCase(side, buyStr)) return AggressorSide::Buy;
    if (equalsIgnoreCase(side, sellStr)) return AggressorSide::Sell;
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

// 🚀 FAST ISO8601 TIMESTAMP PARSING
// Optimized for Coinbase timestamp format: "2023-02-09T20:32:50.714964855Z"

/**
 * Fast ISO8601 timestamp parser for exchange timestamps
 * @param iso8601_str Timestamp string in format "2023-02-09T20:32:50.714964855Z"
 * @return chrono::system_clock::time_point, or current time if parsing fails
 */
inline std::chrono::system_clock::time_point parseISO8601(std::string_view iso8601_str) {
    if (iso8601_str.empty() || iso8601_str.size() < 19) {
        return std::chrono::system_clock::now(); // Fallback to current time
    }
    
    try {
        // Parse format: "2023-02-09T20:32:50.714964855Z"
        // Extract main components: YYYY-MM-DDTHH:MM:SS
        const int yearValue = fastStringToInt(iso8601_str.substr(0, 4));
        const unsigned int monthValue = static_cast<unsigned int>(fastStringToInt(iso8601_str.substr(5, 2)));
        const unsigned int dayValue = static_cast<unsigned int>(fastStringToInt(iso8601_str.substr(8, 2)));
        const int hourValue = fastStringToInt(iso8601_str.substr(11, 2));
        const int minuteValue = fastStringToInt(iso8601_str.substr(14, 2));
        const int secondValue = fastStringToInt(iso8601_str.substr(17, 2));

        // Parse fractional seconds if present (supports up to microsecond precision)
        size_t pos = 19;
        int64_t fractional_microseconds = 0;
        int fractional_digits = 0;
        if (pos < iso8601_str.size() && iso8601_str[pos] == '.') {
            ++pos;  // skip '.'
            while (pos < iso8601_str.size()) {
                char ch = iso8601_str[pos];
                if (ch < '0' || ch > '9') {
                    break;
                }
                if (fractional_digits < 6) {
                    fractional_microseconds = fractional_microseconds * 10 + (ch - '0');
                    ++fractional_digits;
                }
                ++pos;
            }
            while (fractional_digits > 0 && fractional_digits < 6) {
                fractional_microseconds *= 10;
                ++fractional_digits;
            }
        }

        // Parse timezone offset (Z, +HH:MM, -HH:MM)
        int tzSign = 0;
        int tzHours = 0;
        int tzMinutes = 0;
        if (pos < iso8601_str.size()) {
            const char tzChar = iso8601_str[pos];
            if (tzChar == 'Z' || tzChar == 'z') {
                ++pos;
            } else if (tzChar == '+' || tzChar == '-') {
                tzSign = (tzChar == '+') ? 1 : -1;
                ++pos;
                if (pos + 1 < iso8601_str.size()) {
                    tzHours = fastStringToInt(iso8601_str.substr(pos, 2));
                    pos += 2;
                }
                if (pos < iso8601_str.size() && iso8601_str[pos] == ':') {
                    ++pos;
                }
                if (pos + 1 < iso8601_str.size()) {
                    tzMinutes = fastStringToInt(iso8601_str.substr(pos, 2));
                    pos += 2;
                }
            }
        }

        using namespace std::chrono;

        const auto y = year{yearValue};
        const auto m = month{monthValue};
        const auto d = day{dayValue};

        if (!y.ok() || !m.ok() || !d.ok()) {
            return std::chrono::system_clock::now();
        }

        sys_days days_since_epoch{y / m / d};
        auto time_point = sys_time<microseconds>(days_since_epoch);
        time_point += hours(hourValue);
        time_point += minutes(minuteValue);
        time_point += seconds(secondValue);
        time_point += microseconds(fractional_microseconds);

        if (tzSign != 0) {
            auto offset = hours(tzHours) + minutes(tzMinutes);
            time_point -= offset * tzSign;
        }

        return time_point;

    } catch (...) {
        // On any parsing error, return current time
        return std::chrono::system_clock::now();
    }
}

/**
 * Format exchange timestamp for logging
 * @param timestamp Exchange timestamp to format
 * @return Formatted timestamp string
 */
inline std::string formatExchangeTimestamp(std::chrono::system_clock::time_point timestamp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(timestamp);
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        timestamp.time_since_epoch()) % 1000000;
    
    std::tm* utc_tm = std::gmtime(&time_t_val);
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:06d}Z",
        utc_tm->tm_year + 1900, utc_tm->tm_mon + 1, utc_tm->tm_mday,
        utc_tm->tm_hour, utc_tm->tm_min, utc_tm->tm_sec, microseconds.count());
}

} // namespace Cpp20Utils 
