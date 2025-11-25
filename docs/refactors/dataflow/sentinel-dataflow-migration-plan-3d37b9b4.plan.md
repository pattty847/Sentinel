<!-- 3d37b9b4-2193-4f1e-90ca-39e1c189e7a5 8ec685b9-144f-464a-a237-3175d8b4cc99 -->
# Sentinel Data Flow Architecture & Migration Plan

## 1. Architecture & Data Flow Map

### Current System Diagram (Reconstructed)

```mermaid
graph TD
    subgraph Core
        DC[DataCache] -->|recentTrades(symbol)| UGR[UnifiedGridRenderer]
        DC -->|OrderBook| DP[DataProcessor]
    end

    subgraph Background Thread
        DP -->|Process| LTE[LiquidityTimeSeriesEngine]
        LTE -->|Aggregated Slices| DP
        DP -->|m_visibleCells (Snapshot)| UGR
    end

    subgraph GUI Thread (Render Loop)
        UGR -->|1. Duplicate Trades| UGR_Trades[m_recentTrades RingBuffer]
        UGR -->|2. Fetch Cells| UGR_Cells[m_visibleCells]
        UGR -->|3. Bundle| Batch[GridSliceBatch]
        
        Batch -->|Pass to all| GSN[GridSceneNode]
        
        GSN -->|Batch| HM[HeatmapStrategy]
        GSN -->|Batch| TB[TradeBubbleStrategy]
        GSN -->|Batch| TF[TradeFlowStrategy]
        
        HM -.->|Uses| Batch_Cells[Batch.cells]
        TB -.->|Uses| Batch_Trades[Batch.recentTrades]
        TF -.->|Uses| Batch_Trades
        
        %% Waste
        HM --x|Ignores| Batch_Trades
        TB --x|Ignores| Batch_Cells
    end
```

### Data Flow Anomalies

1.  **Trade Duplication**: `DataCache` maintains a `RingBuffer<Trade, 1000>`. `UnifiedGridRenderer` *also* maintains `m_recentTrades` (lines 77-85), effectively duplicating the data in memory on the GUI thread.
2.  **The "God DTO"**: `GridSliceBatch` forces the coupling of unrelated data (heatmap cells vs. individual trades). Every time the viewport changes, this entire batch is reconstructed.
3.  **Implicit Coupling**: `DataProcessor` is tightly coupled to `GridViewState` (lines 408-418 of `DataProcessor.cpp`). It uses the UI's view state to determine what data to process, effectively mixing UI logic into the data backend.

---

## 2. Architectural Chokepoints

| Component | Location | Issue | Consequence |

|-----------|----------|-------|-------------|

| **GridSliceBatch** | `libs/gui/render/GridTypes.hpp` | Monolithic struct bundling `cells` and `recentTrades`. | Strategies receive 50%+ irrelevant data. Prevents independent update cycles (e.g., updating bubbles without touching heatmap). |

| **UGR::updatePaintNode** | `libs/gui/UnifiedGridRenderer.cpp:504` | Reconstructs `GridSliceBatch` on every frame/dirty flag. | Unnecessary object creation and copying. Centralizes all data knowledge in UGR. |

| **TradeBubbleStrategy** | `TradeBubbleStrategy.cpp:21` | Takes `GridSliceBatch` but ignores `cells`. | Semantic mismatch. If `cells` is massive, we're passing a heavy pointer for no reason. |

| **DataProcessor** | `DataProcessor.cpp:103` | Direct dependency on `GridViewState`. | Backend processor depends on frontend state object. Makes testing `DataProcessor` in isolation difficult. |

| **UGR::m_recentTrades** | `UnifiedGridRenderer.cpp:81` | Redundant ring buffer. | `DataCache` already has this. Wastes memory and CPU cycles syncing two buffers. |

---

## 3. Ideal Future Architecture

The goal is a **Pull-Based / Accessor Pattern**. Strategies request exactly what they need from an interface.

```mermaid
graph TD
    subgraph Data Access Layer
        DA[IDataAccessor]
        DA -->|getVisibleCells()| DP_Ref[DataProcessor Ref]
        DA -->|getRecentTrades()| DC_Ref[DataCache Ref]
        DA -->|getOrderBook()| DC_Ref
    end

    subgraph Render Strategies
        HM[HeatmapStrategy] -->|Pull| DA
        TB[TradeBubbleStrategy] -->|Pull| DA
    end
    
    subgraph Composition
        UGR[UnifiedGridRenderer] -->|owns| DA
        UGR -->|passes DA| Strategies
    end
```

**Key Benefits:**

-   **Zero-Copy**: Strategies access shared pointers directly from the source.
-   **Decoupled**: Heatmap doesn't know about trades; Bubbles don't know about cells.
-   **Extensible**: Adding `getL2Data()` to the accessor doesn't break existing strategy signatures.

---

## 4. Phased Migration Plan

This plan allows us to migrate safely without breaking `main` or stopping development.

### Phase 1: The Interface Bridge (Safe Start)

**Goal**: Introduce `IDataAccessor` without changing the underlying data flow yet.

1.  Define `IDataAccessor` pure virtual interface in `libs/gui/render/IDataAccessor.hpp`.
2.  Create `UGRDataAccessor` implementation in `UnifiedGridRenderer.cpp`.

    -   Initially, this will just hold a reference to the existing `GridSliceBatch` (dirty hack for transition).

3.  Update `IRenderStrategy::buildNode` signature to take `IDataAccessor*` instead of `GridSliceBatch`.
4.  Update all strategies to call `accessor->getRecentTrades()` etc. (which internally just reads the batch).

### Phase 2: Eliminate Trade Duplication

**Goal**: Remove `m_recentTrades` from UGR and pull from `DataCache`.

1.  Update `UGRDataAccessor::getRecentTrades` to query `m_ugr->m_dataCache->recentTrades()` directly.
2.  Remove `m_recentTrades` vector and `onTradeReceived` buffering logic from `UnifiedGridRenderer`.
3.  Remove `recentTrades` vector from `GridSliceBatch`.
4.  *Result*: `TradeBubbleStrategy` now pulls live data from Core, bypassing UGR storage.

### Phase 3: Decouple Heatmap Data

**Goal**: Direct access to `DataProcessor` output.

1.  Expose `DataProcessor::getVisibleCells()` (thread-safe shared_ptr) via a public getter on `UnifiedGridRenderer` (or pass DP to Accessor).
2.  Update `UGRDataAccessor::getVisibleCells` to fetch this directly.
3.  Remove `cells` from `GridSliceBatch`.
4.  *Result*: `HeatmapStrategy` pulls cells. `GridSliceBatch` is now nearly empty (just Viewport left).

### Phase 4: The Viewport Context

**Goal**: Kill `GridSliceBatch` completely.

1.  Move `Viewport` struct into `IDataAccessor::getViewport()`.
2.  Strategies now query viewport from the accessor.
3.  Delete `GridSliceBatch` struct entirely.
4.  Refactor `GridSceneNode` to stop passing batches.

### Phase 5: Cleanup & Optimization (Optional DOD)

1.  Now that data access is abstract, we can optimize the *implementation* of `getVisibleCells` to return SoA (Structure of Arrays) or raw GPU buffers without changing strategy logic.

---

## 5. Branch Management Recommendation

**Current Status**: `refactor/dataflow` is active.

**DOD Branch**: You mentioned a previous DOD branch.

**Recommendation**: **NUKE the old DOD branch.**

-   **Why?** Premature optimization is the root of all evil. The DOD changes likely optimized data structures that we are about to move/refactor.
-   **Strategy**: Complete the architecture refactor (Phases 1-4) on `refactor/dataflow`. Once the *flow* is clean, start a new `feature/dod-optimization` branch to implement the internal storage optimizations (Phase 5).

## 6. Immediate Next Step

Execute **Phase 1**: Define the Interface and Bridge. This is a low-risk, high-value step that sets the stage for everything else.

### To-dos

- [ ] Create IDataAccessor interface in libs/gui/render/IDataAccessor.hpp
- [ ] Implement UGRDataAccessor in UnifiedGridRenderer that wraps legacy GridSliceBatch
- [ ] Update IRenderStrategy::buildNode signature to use IDataAccessor*
- [ ] Update Heatmap and Bubble strategies to use the new accessor methods