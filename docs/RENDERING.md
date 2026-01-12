## Sentinel Rendering Pipeline Overview

### 1. Primary Role of Each Class

| Class | Layer | Role |
|-------|-------|------|
| **`DataProcessor`** | GUI (worker thread) | Receives market data (trades, order books), drives `LiquidityTimeSeriesEngine`, converts aggregated slices into `CellInstance` batches, and publishes snapshots for rendering |
| **`LiquidityTimeSeriesEngine`** | Core | Aggregates raw `OrderBook` snapshots into multi-resolution `LiquidityTimeSlice` buckets (100ms, 250ms, 1s, etc.) with tick-based O(1) access and anti-spoofing metrics |
| **`UnifiedGridRenderer`** | GUI (QQuickItem) | Orchestrates the render loop: owns `DataProcessor`, manages viewport state, dirty flags, and calls `update()` to trigger Qt's scene graph |
| **`GridSceneNode`** | Render Thread (QSG) | A `QSGTransformNode` container that owns strategy-generated child nodes (heatmap, bubbles, flow layers); no business logic |

---

### 2. Connections Between Components

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           EXTERNAL DATA SOURCES                             │
│              (WebSocket → Dispatcher → DataCache → signals)                 │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼ Qt::QueuedConnection
┌─────────────────────────────────────────────────────────────────────────────┐
│                            DataProcessor                                    │
│  (lives on worker QThread)                                                  │
│                                                                             │
│  Slots:                                                                     │
│   • onTradeReceived(Trade)                                                  │
│   • onOrderBookUpdated(shared_ptr<OrderBook>)                               │
│   • onLiveOrderBookUpdated(productId, deltas)  ─────┐                       │
│                                                     │                       │
│                          ┌──────────────────────────▼───────────────────┐   │
│                          │   LiquidityTimeSeriesEngine                  │   │
│                          │   (owned by DataProcessor)                   │   │
│                          │                                              │   │
│                          │   • addOrderBookSnapshot(OrderBook)          │   │
│                          │   • addDenseSnapshot(DenseBookSnapshotView)  │   │
│                          │   • getVisibleSlices(timeframe, range)       │   │
│                          │        → returns vector<LiquidityTimeSlice*> │   │
│                          └──────────────────────────────────────────────┘   │
│                                                                             │
│   updateVisibleCells():                                                     │
│    1. Query LTSE for visible slices                                         │
│    2. createCellsFromLiquiditySlice() → builds CellInstance vector          │
│    3. Publish snapshot via m_publishedCells (shared_ptr swap)               │
│    4. emit dataUpdated() ─────────────────────────────────────────────────┐ │
└─────────────────────────────────────────────────────────────────────────┼─┘
                                                                          │
                                      Qt::QueuedConnection                │
                                                                          ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         UnifiedGridRenderer                                 │
│  (QQuickItem on GUI thread)                                                 │
│                                                                             │
│   On dataUpdated():                                                         │
│    • m_appendPending = true                                                 │
│    • update()  ───────────────────────────────────────────────────────────┐ │
│                                                                           │ │
│   updatePaintNode(oldNode):  ◄────────────────────────────────────────────┘ │
│    • Check dirty flags: geometry > append > material > transform            │
│    • updateVisibleCells() → grabs m_publishedCells snapshot (zero-copy)     │
│    • Build UGRDataAccessor with Viewport + cells                            │
│    • Call GridSceneNode::updateLayeredContent(accessor, strategies...)      │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼ Render Thread
┌─────────────────────────────────────────────────────────────────────────────┐
│                            GridSceneNode                                    │
│  (QSGTransformNode)                                                         │
│                                                                             │
│   updateLayeredContent(IDataAccessor*, strategies...):                      │
│    • heatmapStrategy->buildNode(accessor) → appends m_heatmapNode           │
│    • bubbleStrategy->buildNode(accessor)  → appends m_bubbleNode            │
│    • flowStrategy->buildNode(accessor)    → appends m_flowNode              │
│                                                                             │
│   updateTransform(QMatrix4x4):                                              │
│    • setMatrix(transform) — applies pan/zoom offset                         │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
                               ┌─────────────┐
                               │     GPU     │
                               └─────────────┘
```

---

### 3. Key Data Structures at Each Stage

```
  RAW INPUT                    AGGREGATION                   RENDERING
 ───────────────────────────────────────────────────────────────────────────

 Trade                                                     
   • price                                                 
   • size                                                  
   • timestamp                                             
   • side                                                  
                    ┌──────────────────────────────────┐   
 OrderBook          │   LiquidityTimeSeriesEngine      │   
   • bids[]         │                                  │   
   • asks[]    ────►│   addOrderBookSnapshot()         │   
   • timestamp      │        │                         │   
                    │        ▼                         │   
                    │   OrderBookSnapshot              │   
                    │     • timestamp_ms               │   
                    │     • bids: map<price, size>     │   
                    │     • asks: map<price, size>     │   
                    │        │                         │   
                    │        ▼                         │   
                    │   LiquidityTimeSlice             │   
                    │     • startTime_ms, endTime_ms   │   
                    │     • minTick, maxTick, tickSize │   
                    │     • bidMetrics[]: PriceLevelMetrics   
                    │     • askMetrics[]: PriceLevelMetrics   
                    │     • dataVersion (change detect)│   
                    └──────────────────┼───────────────┘   
                                       │                   
                    DataProcessor      │                   
                    createCellsFromLiquiditySlice()        
                                       │                   
                                       ▼                   
                               CellInstance               
                                 • timeStart_ms           
                                 • timeEnd_ms             
                                 • priceMin, priceMax     
                                 • liquidity (float)      
                                 • isBid (bool)           
                                       │                   
                         ──────────────┼──────────────     
                        │              │              │    
                        ▼              ▼              ▼    
                   HeatmapNode    BubbleNode     FlowNode  
                    (QSGNode)     (QSGNode)     (QSGNode)  
```

---

### 4. Summary Flow (Text)

```
Trade/OrderBook (raw)
    │
    ▼
DataProcessor.onOrderBookUpdated()
    │
    ├──► LiquidityTimeSeriesEngine.addOrderBookSnapshot()
    │         │
    │         ▼
    │    [OrderBookSnapshot] aggregated into [LiquidityTimeSlice] per timeframe
    │
    ▼
DataProcessor.updateVisibleCells()
    │
    ├──► LTSE.getVisibleSlices(timeframe, viewport)
    │         │
    │         ▼
    │    [vector<LiquidityTimeSlice*>]
    │
    ├──► createCellsFromLiquiditySlice() → [vector<CellInstance>]
    │
    └──► emit dataUpdated()
              │
              ▼ (QueuedConnection)
    UnifiedGridRenderer
         │
         ├──► update() triggers updatePaintNode()
         │
         ▼
    GridSceneNode.updateLayeredContent(IDataAccessor*)
         │
         ├──► HeatmapStrategy.buildNode() → QSGNode (geometry from CellInstance[])
         ├──► BubbleStrategy.buildNode()  → QSGNode (trades from DataCache)
         └──► FlowStrategy.buildNode()    → QSGNode
                   │
                   ▼
               GPU Render
```

---

### Key Takeaways for Future Investigation

1. **Hot path**: `captureOrderBookSnapshot()` → `addOrderBookSnapshot()` → `updateVisibleCells()` — this is where most CPU cycles are spent.

2. **Viewport version gating**: `DataProcessor` compares `m_lastViewportVersion` to decide rebuild vs. append. If viewport changes, all cells are cleared and rebuilt.

3. **Thread boundaries**:
   - **Worker thread**: `DataProcessor` + `LiquidityTimeSeriesEngine`
   - **GUI thread**: `UnifiedGridRenderer` (QueuedConnection signals)
   - **Render thread**: `GridSceneNode` + strategy `buildNode()` calls

4. **Zero-copy handoff**: `m_publishedCells` is a `shared_ptr<const vector<CellInstance>>` swapped atomically — renderer grabs the pointer without copying.

5. **Strategy contract**: Strategies implement `IRenderStrategy::buildNode(IDataAccessor*)` and receive cells/trades through the accessor interface.