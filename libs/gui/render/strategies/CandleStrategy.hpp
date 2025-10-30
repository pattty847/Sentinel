/*
Sentinel — CandleStrategy
Role: A concrete render strategy that visualizes market data as OHLC candlesticks.
Inputs/Outputs: Implements IRenderStrategy to turn a GridSliceBatch into a QSGNode of lines.
Threading: Methods are called exclusively on the Qt Quick render thread.
Performance: Renders all geometry into a single QSGNode for efficiency.
Integration: Instantiated and managed by UnifiedGridRenderer as a pluggable strategy.
Observability: No internal logging.
Related: CandleStrategy.cpp, IRenderStrategy.hpp, UnifiedGridRenderer.h, GridTypes.hpp.
Assumptions: Input cells contain OHLC data in their 'customData' map.
*/
#pragma once
#include "../IRenderStrategy.hpp"
#include "../GridTypes.hpp"

/* TODO: 

  1. Extend CellInstance to include OHLC data
  2. Store OHLC data in a map/dictionary keyed by cell
  3. Use the existing customData field mentioned in the comments

    struct CellInstance {
      // Existing heatmap data
      double liquidity = 0.0;
      bool isBid = true;
      double intensity = 0.0;

      // Add OHLC data
      double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
      int tradeCount = 0;

      // Computed properties
      bool hasOHLC() const { return high > 0.0 && low > 0.0; }
      bool isBullish() const { return close >= open; }
  };

*/

class CandleStrategy : public IRenderStrategy {
public:
    CandleStrategy() = default;
    ~CandleStrategy() override = default;
    
    QSGNode* buildNode(const GridSliceBatch& batch) override;
    QColor calculateColor(double liquidity, bool isBid, double intensity) const override;
    const char* getStrategyName() const override { return "VolumeCandles"; }
    
    void setWickThickness(float thickness) { m_wickThickness = thickness; }
    void setCandleBodyRatio(float ratio) { m_bodyWidthRatio = ratio; }
    
protected:
    virtual QColor getBullishColor(double intensity) const;
    virtual QColor getBearishColor(double intensity) const;
    
private:
    struct CandleVertex {
        float x, y;           // Position
        float r, g, b, a;     // Color
        float width;          // Candle width
    };
    
    struct OHLCData {
        double open, high, low, close;
        bool isValid() const { return high >= std::max(open, close) && low <= std::min(open, close); }
        bool isBullish() const { return close >= open; }
    };
    
    OHLCData extractOHLC(const CellInstance& cell) const;
    void renderCandleBody(QSGGeometry::ColoredPoint2D*& vertices, const OHLCData& ohlc, 
                         const QRectF& bounds, const QColor& color) const;
    void renderWicks(QSGGeometry::ColoredPoint2D*& vertices, const OHLCData& ohlc,
                    const QRectF& bounds, const QColor& color) const;
    
    // Configuration - internal state
    float m_wickThickness = 1.0f;
    float m_bodyWidthRatio = 0.8f;  // 80% of available width for candle body
};