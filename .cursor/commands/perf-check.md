# Performance Audit

Analyze code for performance issues, especially hot paths.

**Focus areas (Sentinel priorities):**
- **Rendering paths:** QSGGeometry updates, buffer allocations, per-frame heap allocations
- **Data processing:** DataProcessor, LiquidityTimeSeriesEngine, HeatmapTwapStreamer
- **Market data:** Message dispatch, cache lookups, transformations
- **Threading:** Lock contention, unnecessary synchronization, blocking operations

**Check for:**
- Heap allocations in hot paths (use stack or preallocated buffers)
- Unnecessary copies (pass by const ref, use move semantics)
- O(n²) or worse algorithms that could be optimized
- Lock contention or excessive synchronization
- Missing const correctness (prevents optimizations)
- Virtual function calls in tight loops (consider final or devirtualization)
- String operations in hot paths (QString allocations)
- Unnecessary Qt signal/slot overhead in tight loops

**Output format:**
- **Hot paths identified:** List functions that run frequently
- **Issues found:** Each with severity (critical/high/medium/low), location, and fix suggestion
- **Quick wins:** Low-effort, high-impact optimizations
- **Deep dives:** Areas that need profiling to confirm

**Rules:**
- Don't optimize prematurely - focus on actual hot paths
- Consider Sentinel's real-time constraints (GPU rendering, market data latency)
- Suggest profiling before major refactors
- Respect the architecture (don't suggest breaking core/GUI separation for performance)

Be pragmatic. Not every allocation is a problem - focus on what matters for real-time performance.
