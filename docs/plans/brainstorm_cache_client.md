# Client-Side Caching Strategy - Brainstorming Session

## Feature Analysis

### Core Problem Statement
We need local caching of remote heatmap data so the client can:
1. **Re-render without server requests**: When tuning settings (liquidity threshold, labels) or adjusting viewport, avoid re-requesting columns from server
2. **Live threshold updates**: Change liquidity threshold and update both past and present columns instantly
3. **Text overlay caching**: Prevent heatmap cell text overlays from being repainted every frame
4. **Multi-asset support**: Cache per-symbol, fetch deltas when switching back to previously viewed assets
5. **Reduce server load**: Minimize redundant requests, especially for non-cached timeframes (1s/2s candles)

### Requirements Breakdown

**Immediate Needs:**
- Cache received heatmap columns (intensity + liquidity) per symbol/timeframe
- Re-apply liquidity threshold filtering to cached columns without server round-trip
- Cache rendered text overlay geometry/quads to avoid per-frame rebuilds
- Track which columns/timeframes are cached vs. need server fetch

**Future Needs:**
- Per-symbol cache with delta fetching (request only new columns since last view)
- Timeframe rollup support (client may request non-cached timeframes, server rolls up on-the-fly)
- Data retention policy (in-memory only? persistent to disk? TTL?)

**Constraints:**
- Server caches 1m+ candles; 1s/2s are computed on-the-fly
- Client requires server connection (no local-only mode)
- Current architecture: `RemoteGridDataSource` → `DataProcessor` → `UnifiedGridRenderer` → GPU
- Threading: Network I/O on separate thread, GUI on main thread, rendering on render thread

### How It Fits Into Sentinel's Mission
- **Core stays pure C++**: Cache logic should live in `libs/core` (no Qt dependencies)
- **GUI owns rendering**: Cache queries/updates happen in `libs/gui`, but cache storage is core
- **GPU-first**: Cache must support fast GPU texture uploads, not CPU-per-cell rendering

---

## Research Findings

### How Professional Trading Terminals Handle Caching

**Bloomberg Terminal:**
- Multi-tier caching: In-memory ring buffers for active symbols, disk-backed cache for historical
- Delta compression: Only fetches new data since last subscription
- Per-symbol, per-timeframe cache keys
- LRU eviction for memory pressure

**TradingView:**
- Client-side cache for chart data (candles, order book snapshots)
- Cache invalidation on symbol/timeframe change
- Aggressive prefetching for adjacent timeframes
- Text rendering cached per zoom level (glyph quads reused until zoom changes)

**MetaTrader:**
- Persistent cache files per symbol/timeframe
- Incremental updates (only new bars)
- Cache survives application restart

**Common Patterns:**
1. **Ring buffer for active data**: Keep N most recent columns in memory
2. **Time-indexed storage**: Columns keyed by `(symbol, timeframe, bucketStartMs)`
3. **Delta fetching**: Track `lastReceivedTime` per symbol/timeframe, request only gaps
4. **Rendered geometry caching**: Cache glyph quads per viewport/zoom level
5. **LRU eviction**: When memory pressure, evict least recently accessed symbols/timeframes

### Relevant Techniques

**Memory-Mapped Files:**
- For persistent cache: mmap allows OS to page cache efficiently
- Good for large historical datasets
- Sentinel already uses binary logging on server side

**Incremental Updates:**
- Track sequence numbers or timestamps per column
- Server can send "gap" messages when client requests historical
- Client requests: `{"type": "request_range", "symbol": "BTCUSD", "timeframe": 1000, "start": 1234567890, "end": 1234567899}`

**Texture Streaming Optimization:**
- Cache intensity columns as raw bytes (ready for GPU upload)
- Cache liquidity columns separately (needed for threshold filtering)
- Cache liquidity scales per column (needed for threshold calculation)

---

## Architectural Proposal

### Layer Assignment

**Core Layer (`libs/core/cache/`):**
- `HeatmapColumnCache` - Pure C++ cache for columns (no Qt)
- `ColumnKey` - Key type: `(symbol, timeframeMs, bucketStartMs)`
- `CacheEntry` - Stores intensity column, liquidity column, liquidity scale, metadata
- `CachePolicy` - LRU eviction, TTL, max size

**GUI Layer (`libs/gui/cache/` or extend `datasources/`):**
- `CachedGridDataSource` - Wraps `RemoteGridDataSource`, adds cache layer
- `CacheQuery` - Query interface for cached columns
- `LabelGeometryCache` - Caches rendered glyph quads per viewport/zoom

### Key Components

**1. HeatmapColumnCache (Core)**
```cpp
// libs/core/cache/HeatmapColumnCache.hpp
class HeatmapColumnCache {
public:
    struct ColumnKey {
        std::string symbol;
        int64_t timeframeMs;
        int64_t bucketStartMs;
        
        bool operator<(const ColumnKey& other) const;
    };
    
    struct ColumnData {
        std::vector<uint8_t> intensityColumn;  // or uint16_t
        std::vector<uint16_t> liquidityColumn; // optional
        double liquidityScale;
        int64_t bucketEndMs;
        int gridHeight;
    };
    
    // Store column
    void put(const ColumnKey& key, ColumnData data);
    
    // Retrieve column (returns nullopt if not cached)
    std::optional<ColumnData> get(const ColumnKey& key) const;
    
    // Query range: returns cached columns in [startMs, endMs]
    std::vector<std::pair<ColumnKey, ColumnData>> getRange(
        const std::string& symbol,
        int64_t timeframeMs,
        int64_t startMs,
        int64_t endMs
    ) const;
    
    // Check what's missing in range (for delta fetching)
    std::vector<std::pair<int64_t, int64_t>> getMissingRanges(
        const std::string& symbol,
        int64_t timeframeMs,
        int64_t startMs,
        int64_t endMs
    ) const;
    
    // Eviction
    void evictSymbol(const std::string& symbol);
    void evictTimeframe(const std::string& symbol, int64_t timeframeMs);
    void evictLRU(size_t maxSizeBytes);
    
private:
    std::map<ColumnKey, ColumnData> m_cache;
    // LRU tracking
    mutable std::map<ColumnKey, std::chrono::steady_clock::time_point> m_accessTimes;
    size_t m_maxSizeBytes = 1024 * 1024 * 1024; // 1GB default
};
```

**2. CachedGridDataSource (GUI)**
```cpp
// libs/gui/datasources/CachedGridDataSource.hpp
class CachedGridDataSource : public IGridDataSource {
    Q_OBJECT
public:
    explicit CachedGridDataSource(
        RemoteGridDataSource* remote,
        HeatmapColumnCache* cache,
        QObject* parent = nullptr
    );
    
    void subscribe(const QString& symbol) override;
    void unsubscribe(const QString& symbol) override;
    
    // New: Request cached range (for re-rendering)
    void requestCachedRange(const QString& symbol,
                           int64_t timeframeMs,
                           int64_t startMs,
                           int64_t endMs);
    
signals:
    // Emitted for cached columns (synchronous, no server round-trip)
    void cachedHeatmapSliceReceived(...);
    
private slots:
    void onRemoteHeatmapSliceReceived(...); // Cache + forward
    void onLiquidityThresholdChanged(double threshold);
    
private:
    RemoteGridDataSource* m_remote;
    HeatmapColumnCache* m_cache;
    double m_currentLiquidityThreshold = 0.0;
};
```

**3. LabelGeometryCache (GUI)**
```cpp
// libs/gui/render/LabelGeometryCache.hpp
class LabelGeometryCache {
public:
    struct CacheKey {
        int viewportX, viewportY, viewportW, viewportH;
        int gridSize;
        double liquidityThreshold;
        int liquidityLabelMode; // 0 = raw, 1 = dollars
        float cellW, cellH; // pixel size affects which glyphs are visible
        
        bool operator<(const CacheKey& other) const;
    };
    
    struct CachedQuads {
        std::vector<HeatmapLabelRenderer::GlyphQuad> whiteQuads;
        std::vector<HeatmapLabelRenderer::GlyphQuad> blackQuads;
        std::chrono::steady_clock::time_point cachedAt;
    };
    
    // Check if cache is valid for current viewport/settings
    bool isValid(const CacheKey& key, 
                 const HeatmapStreamState::LabelSnapshot& snapshot) const;
    
    // Get cached quads (or nullopt if invalid/missing)
    std::optional<CachedQuads> get(const CacheKey& key) const;
    
    // Store cached quads
    void put(const CacheKey& key, CachedQuads quads);
    
    // Invalidate on threshold change
    void invalidateThreshold();
    
private:
    std::map<CacheKey, CachedQuads> m_cache;
};
```

### Data Flow

**Current Flow (No Cache):**
```
Server → SentinelStreamClient → RemoteGridDataSource → DataProcessor 
→ UnifiedGridRenderer → HeatmapStreamState → GPU
```

**New Flow (With Cache):**
```
Server → SentinelStreamClient → CachedGridDataSource → [Cache Check]
  ├─ Cache Hit → CachedGridDataSource → DataProcessor (no server round-trip)
  └─ Cache Miss → RemoteGridDataSource → DataProcessor → Cache.put()
```

**Threshold Change Flow:**
```
User adjusts threshold → UnifiedGridRenderer::setHeatmapLiquidityThreshold()
→ CachedGridDataSource::onLiquidityThresholdChanged()
→ Query cache for current viewport range
→ Re-apply threshold filtering to cached columns
→ Emit cachedHeatmapSliceReceived() signals
→ DataProcessor → UnifiedGridRenderer (re-render)
```

**Symbol Switch Flow:**
```
User switches symbol → CachedGridDataSource::subscribe(newSymbol)
→ Check cache for newSymbol
→ If cached: requestCachedRange() for visible viewport
→ If gaps: requestMissingRanges() from server
→ Server sends deltas only
```

### Communication Pattern

**Cache Queries:**
- Synchronous reads (cache is in-memory, fast)
- No signals needed for cache hits (direct method calls)

**Cache Updates:**
- Asynchronous: Remote data arrives via signals, cache updated in slot
- Thread-safe: Cache uses mutexes (or lock-free if we go that route)

**Threshold Changes:**
- Signal: `UnifiedGridRenderer::heatmapLiquidityThresholdChanged(double)`
- Slot: `CachedGridDataSource::onLiquidityThresholdChanged(double)`
- Qt::QueuedConnection (cross-thread safety)

### Thread Model

**Cache Access:**
- `HeatmapColumnCache`: Thread-safe (mutex-protected)
- Accessed from GUI thread (CachedGridDataSource) and potentially worker thread (if DataProcessor queries cache)

**Label Geometry Cache:**
- Accessed only from render thread (in `updatePaintNode()`)
- No mutex needed (render thread is single-threaded)
- Cache key includes viewport, so invalidation happens naturally on zoom/pan

---

## Implementation Strategy

### Phase 1: Basic Column Caching (MVP)

**Goal:** Cache received columns, enable re-rendering without server requests for current symbol/timeframe.

**Tasks:**
1. Create `HeatmapColumnCache` in `libs/core/cache/`
   - Simple `std::map<ColumnKey, ColumnData>`
   - `put()` and `get()` methods
   - Thread-safe with `std::shared_mutex`

2. Wrap `RemoteGridDataSource` with `CachedGridDataSource`
   - Intercept `heatmapSliceReceived` signals
   - Store in cache before forwarding
   - Add `requestCachedRange()` method

3. Integrate into `UnifiedGridRenderer`
   - On threshold change, query cache for visible columns
   - Re-apply threshold, emit cached slices
   - Re-render without server round-trip

4. Test:
   - Subscribe to symbol, let columns accumulate
   - Change threshold, verify re-render uses cache
   - Verify no server requests during threshold adjustment

**Files to Create:**
- `libs/core/cache/HeatmapColumnCache.hpp`
- `libs/core/cache/HeatmapColumnCache.cpp`
- `libs/gui/datasources/CachedGridDataSource.hpp`
- `libs/gui/datasources/CachedGridDataSource.cpp`

**Files to Modify:**
- `libs/gui/UnifiedGridRenderer.h` - Add cache reference, threshold change handler
- `libs/gui/UnifiedGridRenderer.cpp` - Query cache on threshold change
- `libs/gui/MainWindowGpu.cpp` - Wire up CachedGridDataSource instead of RemoteGridDataSource

### Phase 2: Label Geometry Caching

**Goal:** Cache rendered glyph quads to avoid per-frame rebuilds.

**Tasks:**
1. Create `LabelGeometryCache` in `libs/gui/render/`
   - Cache key includes viewport, threshold, cell size
   - Store `CachedQuads` (white + black glyph quads)

2. Integrate into `UnifiedGridRenderer::updatePaintNode()`
   - Before calling `HeatmapLabelRenderer::buildLabelQuads()`, check cache
   - If cache hit and valid, use cached quads
   - If cache miss, build quads and store in cache

3. Invalidation:
   - On threshold change: `invalidateThreshold()`
   - On viewport change: Natural invalidation (cache key includes viewport)
   - On zoom: Natural invalidation (cell size changes)

**Files to Create:**
- `libs/gui/render/LabelGeometryCache.hpp`
- `libs/gui/render/LabelGeometryCache.cpp`

**Files to Modify:**
- `libs/gui/UnifiedGridRenderer.h` - Add `LabelGeometryCache` member
- `libs/gui/UnifiedGridRenderer.cpp` - Check cache before building quads

### Phase 3: Multi-Symbol & Delta Fetching

**Goal:** Cache per-symbol, fetch only deltas when switching back.

**Tasks:**
1. Extend `HeatmapColumnCache`:
   - `getMissingRanges()` - Identify gaps in cached range
   - Per-symbol tracking: `getLastReceivedTime(symbol, timeframe)`

2. Extend `CachedGridDataSource`:
   - On `subscribe(symbol)`, check cache
   - If symbol cached: `requestCachedRange()` for visible viewport
   - Query `getMissingRanges()` for gaps
   - Request only missing ranges from server

3. Server protocol extension (future):
   - `{"type": "request_range", "symbol": "...", "timeframe": 1000, "start": ..., "end": ...}`
   - Server responds with columns in range (or gaps if not available)

**Files to Modify:**
- `libs/core/cache/HeatmapColumnCache.hpp` - Add `getMissingRanges()`
- `libs/core/protocol/SentinelStreamClient.hpp` - Add range request method
- `libs/core/protocol/SentinelStreamServer.hpp` - Handle range requests

### Phase 4: Data Retention & Eviction

**Goal:** Manage memory, decide on persistence strategy.

**Tasks:**
1. Implement LRU eviction in `HeatmapColumnCache`:
   - Track access times per column
   - `evictLRU(size_t maxSizeBytes)` - Remove least recently used columns
   - Call on memory pressure or cache size limit

2. Add eviction policies:
   - Max cache size (env var: `SENTINEL_CACHE_MAX_SIZE_MB`)
   - Per-symbol limits
   - TTL (time-to-live) for old columns

3. **Decision Point:** Persistent cache?
   - Option A: In-memory only (simpler, lost on restart)
   - Option B: Disk-backed (mmap, survives restart)
   - **Recommendation:** Start with in-memory, add persistence later if needed

**Files to Modify:**
- `libs/core/cache/HeatmapColumnCache.cpp` - Implement LRU eviction
- `docs/ENV_VARS.md` - Document `SENTINEL_CACHE_MAX_SIZE_MB`

---

## Open Questions

### 1. Cache Storage Location
**Question:** Should cache live in `libs/core` (pure C++) or `libs/gui` (Qt-aware)?

**Analysis:**
- **Core:** Cache is pure data structure, no Qt dependencies. Fits core layer.
- **GUI:** Cache is only used by GUI components, no server-side need.

**Recommendation:** **Core layer** (`libs/core/cache/`). Cache is a data structure, not UI logic. Keeps core pure.

### 2. Timeframe Rollup Strategy
**Question:** How should client handle non-cached timeframes (1s/2s)? Request server rollup, or client-side aggregation?

**Analysis:**
- **Server rollup:** Server already does this (HeatmapTwapStreamer). Client requests, server computes on-the-fly.
- **Client aggregation:** Client could cache 1s columns, aggregate to 2s/5s/etc. More complex, duplicates server logic.

**Recommendation:** **Server rollup**. Keep aggregation logic on server. Client requests non-cached timeframes, server computes. Client caches result.

### 3. Cache Invalidation on Range Changes
**Question:** When server sends `reset=true`, should we invalidate cache or keep old columns?

**Analysis:**
- **Invalidate:** `reset=true` means price range changed, old columns may be invalid.
- **Keep:** Old columns might still be valid if price range expanded (not shifted).

**Recommendation:** **Keep cache, mark as potentially stale**. On `reset=true`, don't evict, but when querying cache, verify price range compatibility. If range changed significantly, treat as cache miss.

### 4. Label Cache Granularity
**Question:** Cache per-cell or per-viewport?

**Analysis:**
- **Per-cell:** More memory, but precise (only cache visible cells).
- **Per-viewport:** Less memory, invalidate on any viewport change.

**Recommendation:** **Per-viewport**. Cache key includes viewport bounds. On zoom/pan, natural invalidation. Simpler, sufficient for performance gain.

### 5. Delta Fetching Protocol
**Question:** Should delta fetching be part of initial design or Phase 3?

**Analysis:**
- **Phase 1:** Simpler, cache-only. Good for MVP.
- **Phase 3:** Requires server protocol changes. More complex.

**Recommendation:** **Phase 3**. Start with cache-only (Phase 1), add delta fetching when multi-symbol support is needed.

### 6. Memory Limits
**Question:** What's reasonable cache size? Per-symbol limits?

**Analysis:**
- Typical column: 8192 rows × 2 bytes (u16) = 16KB intensity + 16KB liquidity = 32KB per column
- 1000 columns = 32MB per symbol
- 10 symbols × 32MB = 320MB

**Recommendation:** 
- Default: 1GB total cache (`SENTINEL_CACHE_MAX_SIZE_MB=1024`)
- Per-symbol: No hard limit initially, LRU eviction handles pressure
- Monitor memory usage, adjust if needed

### 7. Thread Safety for Label Cache
**Question:** Label cache accessed from render thread only. Need mutex?

**Recommendation:** **No mutex needed**. Render thread is single-threaded. Cache can be `std::map` without locking. If we ever access from multiple threads, add mutex later.

---

## Risks & Mitigation

### Risk 1: Memory Bloat
**Risk:** Cache grows unbounded, OOM crash.

**Mitigation:**
- LRU eviction with size limit (Phase 4)
- Monitor cache size, log warnings
- Env var to disable cache if needed

### Risk 2: Stale Data
**Risk:** Cached columns become stale (price range changed, but cache not invalidated).

**Mitigation:**
- On `reset=true`, mark cache entries as potentially stale
- When querying cache, verify price range compatibility
- If range changed significantly, treat as cache miss

### Risk 3: Thread Safety Bugs
**Risk:** Cache accessed from multiple threads without proper locking.

**Mitigation:**
- Use `std::shared_mutex` for read-heavy access (readers can parallelize)
- Label cache is render-thread-only (no mutex needed)
- Test with thread sanitizer

### Risk 4: Cache Miss Performance
**Risk:** Cache lookup adds latency even on misses.

**Mitigation:**
- Cache lookup is O(log n) map lookup, negligible
- Only query cache on threshold change (not every frame)
- Label cache check is fast (viewport comparison)

### Risk 5: Server Protocol Changes
**Risk:** Delta fetching requires server changes, breaking compatibility.

**Mitigation:**
- Phase 1-2 don't require server changes (cache-only)
- Phase 3 delta fetching is additive (server can ignore range requests, fall back to streaming)
- Version protocol if needed

---

## Next Steps

1. **Review this brainstorming** - Validate approach, identify gaps
2. **Decide on Phase 1 scope** - Confirm MVP features
3. **Create implementation plan** - Break Phase 1 into concrete tasks
4. **Start implementation** - Begin with `HeatmapColumnCache` in core

---

## References

- `docs/ARCHITECTURE.md` - Overall system design
- `libs/gui/datasources/RemoteGridDataSource.cpp` - Current data source
- `libs/gui/render/HeatmapStreamState.cpp` - Current stream state management
- `libs/core/servermodel/HeatmapTwapStreamer.cpp` - Server-side column generation
