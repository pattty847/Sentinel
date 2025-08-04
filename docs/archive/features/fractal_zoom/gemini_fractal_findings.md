
# Gemini Fractal Zoom Findings

This document outlines the findings from the analysis of the existing codebase and a plan to implement the "Timestamp Quantization" and "Unified Frame Rendering" system.

## 1. Analysis of Existing Code

The existing codebase consists of three main components: `GPUChartWidget`, `CandlestickBatched`, and `FractalZoomController`.

*   **`GPUChartWidget`**: This is the core of the rendering system. It manages the main view, handles user input (pan/zoom), and renders trade data. It already has a `worldToScreen` function, but it's based on a continuous time-to-pixel mapping, which is the source of the desync issues. It also has a `viewChanged` signal that will be crucial for coordinating the different layers.
*   **`CandlestickBatched`**: This class is responsible for rendering candlestick charts. It has its own `worldToScreen` function, which is a copy of the one in `GPUChartWidget`. This is a clear example of the duplicated logic that needs to be unified. It also has a `onViewChanged` slot that can be connected to `GPUChartWidget::viewChanged`.
*   **`FractalZoomController`**: This class manages the Level of Detail (LOD) settings based on the zoom level. It defines the different timeframes and their corresponding update intervals. This will be the central piece for the new quantized time system.

The files for `HeatmapBatched` and `TemporalSettings` were not found. The plan will proceed with the assumption that `HeatmapBatched` will need the same refactoring as `CandlestickBatched`.

## 2. Plan for Refactoring

The following is a detailed plan to refactor the codebase to implement the "Timestamp Quantization" and "Unified Frame Rendering" system.

### 2.1. Create a Unified Coordinate System

A new class, `ChartCoordinator`, will be created to be the single source of truth for all coordinate transformations. This class will implement the "Timestamp Quantization" logic.

**`ChartCoordinator` Public API:**

```cpp
class ChartCoordinator : public QObject {
    Q_OBJECT
public:
    // --- Time Quantization ---
    qint64 quantizeTimestamp(qint64 timestamp_ms) const;
    int timestampToFrameIndex(qint64 timestamp_ms) const;
    qint64 frameIndexToTimestamp(int frameIndex) const;

    // --- Coordinate Transformation ---
    QPointF worldToScreen(qint64 timestamp_ms, double price) const;
    QPointF screenToWorld(const QPointF& screenPos) const;

    // --- Viewport Management ---
    void setViewRect(const QRectF& viewRect);
    void setTimeRange(qint64 startTime_ms, qint64 endTime_ms);
    void setPriceRange(double minPrice, double maxPrice);

signals:
    void viewChanged();

private:
    // ... internal state for time and price ranges, bucket size, etc.
};
```

### 2.2. Refactor `GPUChartWidget`

*   `GPUChartWidget` will own the `ChartCoordinator` instance.
*   The `worldToScreen` and `screenToWorld` functions in `GPUChartWidget` will be removed and replaced with calls to the `ChartCoordinator`.
*   The `viewChanged` signal in `GPUChartWidget` will be connected to the `ChartCoordinator`'s `setViewRect`, `setTimeRange`, and `setPriceRange` slots.
*   The `updatePaintNode` function will be updated to use the `ChartCoordinator` for all coordinate calculations.

### 2.3. Refactor `CandlestickBatched` and `HeatmapBatched`

*   These classes will receive a pointer to the `ChartCoordinator` from `GPUChartWidget`.
*   Their `worldToScreen` functions will be removed and replaced with calls to the `ChartCoordinator`.
*   The `onViewChanged` slot will be connected to the `ChartCoordinator::viewChanged` signal.

### 2.4. Implement the `RenderFrame` System

A new class, `RenderFrame`, will be created to hold the aggregated data for a single time bucket.

```cpp
struct RenderFrame {
    qint64 startTime_ms;
    qint64 endTime_ms;
    std::vector<Trade> trades;
    // std::vector<OrderBookUpdate> orderBookUpdates; // Add this when HeatmapBatched is available
};
```

The `GPUChartWidget` will be responsible for creating and managing the `RenderFrame` objects. It will have a new data structure to store the frames, and the `onTradeReceived` and `onOrderBookUpdated` slots will be updated to populate the correct `RenderFrame`.

### 2.5. Create Test Data Generator

A new class, `FractalDataGenerator`, will be created to generate test data for the order book to visualize the fractal zoom. This will be a simple class that generates a series of order book updates with varying prices and sizes.

### 2.6. Code Snippets

Here are the proposed changes to the `worldToScreen` functions:

**`GPUChartWidget.cpp`**

```cpp
// Remove the old worldToScreen function and replace with:
QPointF GPUChartWidget::worldToScreen(int64_t timestamp_ms, double price) const {
    return m_chartCoordinator->worldToScreen(timestamp_ms, price);
}
```

**`CandlestickBatched.cpp`**

```cpp
// Remove the old worldToScreen function and replace with:
QPointF CandlestickBatched::worldToScreen(int64_t timestamp_ms, double price) const {
    return m_chartCoordinator->worldToScreen(timestamp_ms, price);
}
```

This concludes the findings and the plan for the refactoring.
