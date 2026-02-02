# Sentinel Debug Mode — Hypothesis-Driven Performance Investigation

**Purpose**: Systematically diagnose performance issues through instrumented hypothesis testing.

---

## When to Use Debug Mode

- User reports subjective lag/stutter despite reasonable FPS metrics
- Profiling tools are unavailable or impractical (e.g., WSL, headless environments)
- Multiple potential bottlenecks exist (rendering, I/O, threading, Qt event loop)
- Need to correlate events across threads and subsystems
- Traditional breakpoint debugging would disrupt real-time behavior

---

## Debug Mode Workflow

### 1. Read the Problem Statement
- Get specific symptoms from user: "Toolbar hover lags", "Mouse trails behind cursor", "Feels like 10fps but shows 60fps"
- Identify what's working vs. what's broken
- Note environmental context (OS, WSL, GPU, Qt version)

### 2. Analyze Codebase for Likely Issues
Read relevant source files and identify:
- **Hot paths**: Rendering loops, data processing pipelines, event handlers
- **Threading boundaries**: Main thread ↔ render thread ↔ worker threads
- **Blocking operations**: File I/O, network calls, mutex locks, frame swap
- **Timer-driven logic**: QTimer callbacks, animation loops, auto-scroll
- **GPU sync points**: QSG geometry updates, texture uploads, frameSwapped signals

### 3. Create Hypotheses
For each potential bottleneck, formulate a testable hypothesis:

```
H1: 16ms render timer causes unnecessary update() calls
H2: updatePaintNode takes >16ms blocking render thread
H3: Data processing stalls before geometry updates
H4: GUI event pulse has >100ms gaps (jank)
H5: frameSwapped (frame swap/present) blocks >40ms
H6: CPU metrics reading blocks main thread >10ms
H7: QSG render stage takes >100ms
H8: QSG sync stage takes >100ms
H9: Graphics API detection at startup
```

Each hypothesis should:
- Target a specific code location
- Have a measurable threshold (e.g., ">40ms", ">100 iterations")
- Be independently testable

### 4. Instrument Code with Debug Logs
Add targeted logging at hypothesis locations using this JSON structure:

```cpp
// Append to /home/pepe/projects/Sentinel/.cursor/debug.log
{
  "sessionId": "debug-session",          // Fixed session identifier
  "runId": "experiment-name",            // Current experiment run
  "hypothesisId": "H5",                  // Which hypothesis (H1, H2, ...)
  "location": "PerformanceMonitor.cpp:82", // File:line
  "message": "frame_swap_slow",          // Event name (snake_case)
  "data": {                              // Metric-specific payload
    "frameTimeMs": 673,
    "thread": 131958543752640
  },
  "timestamp": 1769975882648             // Unix epoch milliseconds
}
```

**Helper Pattern** (inline append):
```cpp
#include <chrono>
#include <fstream>
#include <sstream>

namespace {
inline int64_t debugNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline void appendDebugLog(const char* location,
                           const char* message,
                           const char* hypothesisId,
                           const std::string& dataJson,
                           const char* runId = "default-run") {
    std::ofstream out("/home/pepe/projects/Sentinel/.cursor/debug.log", std::ios::app);
    if (!out.is_open()) return;

    out << "{\"sessionId\":\"debug-session\",\"runId\":\"" << runId
        << "\",\"hypothesisId\":\"" << hypothesisId
        << "\",\"location\":\"" << location
        << "\",\"message\":\"" << message
        << "\",\"data\":" << dataJson
        << ",\"timestamp\":" << debugNowMs() << "}\n";
}
}  // namespace
```

**Example Usage**:
```cpp
// H5: Frame swap blocking >40ms
void PerformanceMonitor::onFrameSwapped() {
    const qint64 frameTimeMs = currentMs - lastFrameMs;

    if (frameTimeMs > 40) {  // Hypothesis threshold
        std::ostringstream payload;
        payload << "{"
                << "\"frameTimeMs\":" << frameTimeMs
                << ",\"thread\":" << reinterpret_cast<quintptr>(QThread::currentThreadId())
                << "}";
        appendDebugLog("PerformanceMonitor.cpp:82", "frame_swap_slow", "H5", payload.str());
    }

    // ... rest of logic
}
```

### 5. Run & Collect Data
Tell user:
1. **Clear old logs**: `rm /home/pepe/projects/Sentinel/.cursor/debug.log`
2. **Start application**: `./build/apps/sentinel_gui/sentinel_gui`
3. **Reproduce issue**: Interact for 30-60 seconds:
   - Hover toolbar icons
   - Pan/zoom viewport
   - Let auto-scroll run
   - Resize windows
   - Switch modes
4. **Stop application**: Ctrl+C or normal exit
5. **Provide log file**: Share `.cursor/debug.log` contents

### 6. Analyze Logs
Parse JSON logs and look for:

**Frequency**: How often does each hypothesis trigger?
```bash
grep -o '"hypothesisId":"H[0-9]"' debug.log | sort | uniq -c
# 1 "hypothesisId":"H9"
# 245 "hypothesisId":"H1"
# 89 "hypothesisId":"H5"
# 12 "hypothesisId":"H8"
```

**Severity**: What are the worst-case values?
```bash
# H5 max frame time
jq -r 'select(.hypothesisId == "H5") | .data.frameTimeMs' debug.log | sort -n | tail -5
# 219
# 673
# 4504  ← CRITICAL SPIKE
```

**Timeline correlation**: Do events coincide?
```bash
# Events within 1 second window
jq -s 'sort_by(.timestamp) | .[]' debug.log | less
```

**Thread analysis**: Which thread is blocking?
```bash
jq -r 'select(.hypothesisId == "H7" or .hypothesisId == "H8") | "\(.timestamp) \(.message) \(.data.thread)"' debug.log
```

### 7. Iterate
Based on findings:
- **Confirm**: If hypothesis triggers frequently with severe values → Root cause candidate
- **Reject**: If hypothesis rarely triggers or values are acceptable → Not the issue
- **Refine**: If hypothesis is inconclusive → Adjust threshold, add more data fields, instrument adjacent code

**Example from WSL Investigation**:
- H1 (16ms timer): Mitigated early (shows `update:false` after fix)
- H5 (frame_swap_slow): **CRITICAL** — 40-400ms stalls, 4.5s spike → WSL compositor bottleneck
- H7/H8 (sync/render): Slow (12s) but secondary to H5
- **Conclusion**: WSL graphics forwarding is the root cause, not application code

---

## Log Structure Reference

### Required Fields
- `sessionId` (string): Fixed identifier for the debugging campaign
- `runId` (string): Experiment name (`"pre-fix"`, `"post-fix"`, `"baseline"`)
- `hypothesisId` (string): `"H1"`, `"H2"`, ... `"H99"`
- `location` (string): `"File.cpp:line"`
- `message` (string): Event name (`"frame_swap_slow"`, `"gpu_upload_stall"`)
- `data` (object): Metric-specific payload (duration, counts, IDs, etc.)
- `timestamp` (integer): Unix epoch milliseconds

### Common Data Patterns

**Duration measurement**:
```json
"data": {
  "durationMs": 156,
  "thread": 131957182678720
}
```

**Delta tracking** (gaps between events):
```json
"data": {
  "deltaMs": 2000,
  "expectedMs": 1000,
  "thread": 131958543752640
}
```

**State snapshot**:
```json
"data": {
  "dragging": false,
  "autoScroll": true,
  "viewportVersion": 42,
  "update": false
}
```

**GPU metrics**:
```json
"data": {
  "graphicsApi": 3,
  "vertexCount": 65536,
  "textureUploadBytes": 4194304
}
```

---

## Integration with Sentinel

### File Locations
- **Log file**: `/home/pepe/projects/Sentinel/.cursor/debug.log` (gitignored)
- **Helper utilities**: Inline in each file (see PerformanceMonitor.cpp:14-38, StatusBar.cpp:127-145)
- **Active instrumentation**:
  - `libs/gui/PerformanceMonitor.cpp` (H5, H6)
  - `libs/gui/widgets/StatusBar.cpp` (H4)
  - `libs/gui/MainWindowGpu.cpp` (H7, H8, H9) — *requires verification*
  - `libs/gui/UnifiedGridRenderer.cpp` (H1, H2) — *requires verification*

### Environment Variable (Future)
```yaml
# sentinel.yaml
debug:
  enabled: true
  log_path: ".cursor/debug.log"
  session_id: "perf-investigation-2025-02"
  run_id: "post-msdf-cache"
```

### Cleanup
After debugging session:
1. **Comment out or remove** debug log calls (mark with `// #region agent log` / `// #endregion`)
2. **Keep helper functions** if they're reusable
3. **Document findings** in commit message or `docs/PERFORMANCE.md`
4. **Delete log file**: `rm .cursor/debug.log`

---

## Case Study: WSL Performance Investigation (2025-02-01)

### Problem
- User reports lag: "feels like 10-30fps", toolbar hover laggy, mouse trails behind
- FPS counter shows 50-63fps (contradictory)
- Environment: WSL2 + Windows compositor

### Hypotheses Created
- **H1**: 16ms timer causes unnecessary updates → MITIGATED (fixed early, shows `update:false`)
- **H5**: Frame swap blocks >40ms → **CRITICAL ROOT CAUSE** (40-400ms, max 4504ms)
- **H7/H8**: Sync/render stages block >100ms → CONFIRMED but secondary (12-37s cumulative)

### Key Findings
```json
// H5: Frame swap taking 4.5 seconds (!)
{"hypothesisId":"H5","message":"frame_swap_slow","data":{"frameTimeMs":4504},"timestamp":...}

// H8/H7: Sync and render accumulating 12+ seconds
{"hypothesisId":"H8","message":"sync_stage_slow","data":{"durationMs":12395},"timestamp":...}
```

### Resolution
- WSL graphics layer (frame swap/present) is the bottleneck, not application code
- Application optimizations (MSDF caching, timer fixes) were correct but couldn't overcome WSL overhead
- User decision: Move off WSL to native Windows/Linux for production

### Lessons
- Subjective lag ≠ low FPS (frame pacing matters)
- Instrumentation reveals platform-specific issues that profilers miss
- Hypothesis-driven approach prevents premature optimization

---

## Best Practices

### DO
- Clear old logs between runs to avoid confusion
- Use descriptive `message` names (`"frame_swap_slow"`, not `"slow"`)
- Include thread IDs for multi-threaded analysis
- Set meaningful thresholds (not too noisy, not too quiet)
- Correlate timestamps across hypotheses to find causality

### DON'T
- Log every frame (only log violations/anomalies)
- Use debug logs in production builds (wrap in `#ifdef SENTINEL_DEBUG_MODE`)
- Forget to flush/close file handles (use `std::ios::app` and RAII)
- Hardcode file paths (use config or env vars)
- Leave debug code uncommented after investigation

---

## Future Enhancements

1. **Structured Logger Class**: Replace inline helpers with `DebugLogger::log(hypothesis, message, data)`
2. **Compile-time toggle**: `#ifdef SENTINEL_DEBUG_MODE` to disable in release builds
3. **Real-time viewer**: Web UI to visualize logs as they're written (via tail -f)
4. **Automatic analysis**: Script to parse logs and highlight critical hypotheses
5. **Remote logging**: Send logs to localhost:port for distributed debugging

---

**Remember**: Debug mode is a scalpel, not a hammer. Use it when traditional tools fail or environmental constraints prevent standard profiling. Always clean up instrumentation after the investigation concludes.
