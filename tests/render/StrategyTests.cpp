#include <gtest/gtest.h>
#include "../../libs/gui/render/strategies/HeatmapStrategy.hpp"
#include "../../libs/gui/render/strategies/TradeFlowStrategy.hpp" 
#include "../../libs/gui/render/strategies/CandleStrategy.hpp"
#include "../../libs/gui/render/RenderTypes.hpp"

class StrategyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test data
        batch.intensityScale = 1.0;
        batch.minVolumeFilter = 0.0;
        batch.maxCells = 1000;
        
        // Add sample cells
        CellInstance cell;
        cell.screenRect = QRectF(10, 10, 20, 20);
        cell.color = QColor(255, 0, 0);
        cell.intensity = 0.8;
        cell.liquidity = 100.0;
        cell.isBid = true;
        cell.timeSlot = 1609459200000; // Example timestamp
        cell.priceLevel = 50000.0;
        
        batch.cells.push_back(cell);
        
        // Add another cell
        cell.screenRect = QRectF(30, 30, 20, 20);
        cell.isBid = false;
        cell.liquidity = 150.0;
        batch.cells.push_back(cell);
    }
    
    GridSliceBatch batch;
};

TEST_F(StrategyTest, HeatmapStrategyCreateNode) {
    HeatmapStrategy strategy;
    
    EXPECT_STREQ(strategy.getStrategyName(), "LiquidityHeatmap");
    
    QSGNode* node = strategy.buildNode(batch);
    ASSERT_NE(node, nullptr);
    
    // Node should be a geometry node for rendering
    auto* geoNode = dynamic_cast<QSGGeometryNode*>(node);
    EXPECT_NE(geoNode, nullptr);
    
    delete node;
}

TEST_F(StrategyTest, TradeFlowStrategyCreateNode) {
    TradeFlowStrategy strategy;
    
    EXPECT_STREQ(strategy.getStrategyName(), "TradeFlow");
    
    QSGNode* node = strategy.buildNode(batch);
    ASSERT_NE(node, nullptr);
    
    auto* geoNode = dynamic_cast<QSGGeometryNode*>(node);
    EXPECT_NE(geoNode, nullptr);
    
    delete node;
}

TEST_F(StrategyTest, CandleStrategyCreateNode) {
    CandleStrategy strategy;
    
    EXPECT_STREQ(strategy.getStrategyName(), "VolumeCandles");
    
    QSGNode* node = strategy.buildNode(batch);
    ASSERT_NE(node, nullptr);
    
    auto* geoNode = dynamic_cast<QSGGeometryNode*>(node);
    EXPECT_NE(geoNode, nullptr);
    
    delete node;
}

TEST_F(StrategyTest, EmptyBatchReturnsNull) {
    HeatmapStrategy strategy;
    GridSliceBatch emptyBatch;
    
    QSGNode* node = strategy.buildNode(emptyBatch);
    EXPECT_EQ(node, nullptr);
}

TEST_F(StrategyTest, ColorCalculation) {
    HeatmapStrategy heatmap;
    TradeFlowStrategy tradeFlow; 
    CandleStrategy candle;
    
    // Test bid colors (should be greenish for heatmap)
    QColor bidColor = heatmap.calculateColor(100.0, true, 0.8);
    EXPECT_GT(bidColor.green(), bidColor.red());
    EXPECT_GT(bidColor.green(), bidColor.blue());
    
    // Test ask colors (should be reddish for heatmap)
    QColor askColor = heatmap.calculateColor(100.0, false, 0.8);
    EXPECT_GT(askColor.red(), askColor.green());
    EXPECT_GT(askColor.red(), askColor.blue());
    
    // All strategies should return valid colors
    EXPECT_TRUE(bidColor.isValid());
    EXPECT_TRUE(askColor.isValid());
    EXPECT_TRUE(tradeFlow.calculateColor(100.0, true, 0.8).isValid());
    EXPECT_TRUE(candle.calculateColor(100.0, false, 0.8).isValid());
}