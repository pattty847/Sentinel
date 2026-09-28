# Trace Data Flow

Trace how data flows from point A to point B in the codebase.

**Usage:**
- "Trace how market data reaches the heatmap renderer"
- "Trace how viewport changes propagate to GPU"
- "Trace how symbol changes flow through widgets"

**Process:**
1. Identify the entry point (where data originates)
2. Follow the call chain through:
   - Function calls
   - Signal/slot connections
   - Data transformations
   - Thread boundaries (mark these!)
   - Cache layers
3. Identify the exit point (where data is consumed/rendered)

**Output format:**
- Visual flow: `Entry → Component1 → Component2 → Exit`
- For each step: file, function, what happens, thread context
- Mark thread boundaries explicitly
- Highlight any caching, buffering, or async boundaries
- Note any potential bottlenecks or race conditions

**Sentinel-specific paths to know:**
- Market data: `Transport → Dispatch → Cache → ServerDataModel → HeatmapTwapStreamer → WebSocket`
- Rendering: `RemoteGridDataSource → DataProcessor → UnifiedGridRenderer → HeatmapIntensityNode → GPU`
- Viewport: `setViewport()` → `viewportVersion++` → grid rebuild

If the path isn't clear, ask clarifying questions rather than guessing.
