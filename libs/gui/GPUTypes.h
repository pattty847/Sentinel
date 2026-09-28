// ======= libs/core/GPUTypes.h =======
#pragma once
#include <cstdint>

namespace GPUTypes {
    struct Point {
        float x, y;           // Screen coordinates
        float r, g, b, a;     // Color (RGBA)
        
        double rawPrice = 0.0;      // Original price before coordinate transformation
        double rawTimestamp = 0.0;  // Original timestamp in milliseconds
        int64_t timestamp_ms = 0;   // Timestamp for aging/cleanup
        float size = 4.0f;          // Point size
    };
    
    struct QuadInstance {
        float x, y;           // Position
        float width, height;  // Quad dimensions
        float r, g, b, a;     // Color (RGBA)
        
        double rawPrice = 0.0;      // Original price
        double rawTimestamp = 0.0;  // Original timestamp in milliseconds
        float intensity = 1.0f;     // Volume/liquidity intensity
        double price = 0.0;         // Price level
        double size = 0.0;          // Order size
        double timestamp = 0.0;     // Timestamp for coordination
    };
}

// ======= Forward Declarations =======
struct Trade;
struct OrderBook;
struct OrderBookLevel;
struct CandleUpdate;
class GPUDataAdapter;
class UnifiedGridRenderer;