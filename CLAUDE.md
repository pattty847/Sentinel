# CLAUDE.md

**Sentinel**: High-performance real-time crypto trading terminal. C++20, Qt6, OpenGL, WebSocket. V2 Grid-Based Architecture with GPU-accelerated rendering.

## 🚨 CRITICAL CONSTRAINTS

### File Size Limits - STRICTLY ENFORCED
- **300 LOC maximum per file** (no exceptions without refactoring first)
- **Current violations needing immediate attention:**
  - `MarketDataCore.cpp` (608 LOC) → Extract WebSocket/data handling 
  - `LiquidityTimeSeriesEngine.cpp` (536 LOC) → Separate aggregation logic
  - `SentinelMonitor.cpp` (508 LOC) → Extract monitoring strategies
  - `UnifiedGridRenderer.cpp` (461 LOC) → Break into coordinating modules
  - `DataProcessor.cpp` (413 LOC) → Break into pipeline stages

**Before ANY changes:** `wc -l [target_file]` → REFUSE if >300 LOC without refactoring first.

### Architecture Boundaries - NON-NEGOTIABLE
```
libs/core/     # Pure C++ business logic - NO Qt except QtCore
libs/gui/      # Qt/QML adapters only - DELEGATES to core/
apps/          # Initialization only - NO business logic
```

**Violations → REJECT immediately:**
- `#include <QQml*>` in `libs/core/`
- Business logic in `libs/gui/`
- Direct QML ↔ core communication

## ⚡ FAST WORKFLOW TOOLS

### Build & Test
```bash
# Build (macOS current platform)
cmake --preset mac-clang && cmake --build --preset mac-clang

# Test everything
cd build-mac-clang && ctest --output-on-failure

# Run app
./build-mac-clang/apps/sentinel_gui/sentinel_gui
```

### Code Analysis - Avoid Reading Massive Files
```bash
# Quick overview (recommended)
./scripts/quick_cpp_overview.sh libs/gui/UnifiedGridRenderer.cpp

# Function count check
./scripts/extract_functions.sh [file] --count

# Detailed with line numbers
./scripts/quick_cpp_overview.sh [file] --detailed
```

### Logging Control
```bash
# Clean output (prod-like)
export QT_LOGGING_RULES="*.debug=false"

# Debug specific system
export QT_LOGGING_RULES="sentinel.render.debug=true"  # GPU/charts
export QT_LOGGING_RULES="sentinel.data.debug=true"    # Network/cache
```

## 🏗️ ARCHITECTURE ESSENTIALS

### File Targeting Patterns
- **Core logic**: `libs/core/[Feature]*.{hpp,cpp}`
- **GUI adapters**: `libs/gui/[Feature]*.{h,cpp}` 
- **Render strategies**: `libs/gui/render/strategies/[Mode]Strategy.{hpp,cpp}`
- **QML controls**: `libs/gui/qml/controls/[Feature]*.qml`

### Data Flow Pipeline
```
WebSocket → MarketDataCore → UnifiedGridRenderer → DataProcessor → LiquidityTimeSeriesEngine → IRenderStrategy → GridSceneNode → GPU
```

### Dependency Chains
- **Data**: WebSocket → MarketDataCore → DataProcessor → Strategy → GPU
- **UI**: QML → UnifiedGridRenderer → GridViewState → Strategy  
- **Config**: SentinelLogging → All components (import via .hpp)

### Common Change Patterns
- **Add data field**: `TradeData.h` → `DataProcessor` → `Strategy`
- **Add visualization**: Create `[Name]Strategy.{hpp,cpp}` in `render/strategies/`
- **Add QML control**: Create in `qml/controls/` → Import in main QML
- **WebSocket changes**: `MarketDataCore.{hpp,cpp}` → Update signal connections

## 🛡️ CHANGE PROTOCOL

### MANDATORY Pre-Change Checklist
1. **File size check**: `wc -l [target_file]` → Refactor if >300 LOC
2. **Architecture context**: `codebase_search "How does [system] work?"`
3. **Find dependencies**: `grep -r "class_name\|function_name" libs/`
4. **Use code analysis tools** instead of reading massive files

### Implementation Rules
- **Edit existing files** over creating new ones
- **CamelCase filenames** to match includes
- **Modern C++20**: RAII, smart pointers, ranges, concepts
- **Strategy pattern** for pluggable components
- **SentinelLogging only**: `sentinel.app|data|render|debug`

### Thread Safety
- **Worker thread**: WebSocket networking (`MarketDataCore`)
- **GUI thread**: Qt UI and GPU rendering only  
- **Lock-free SPSC queues** for inter-thread transfer
- **NO blocking operations** on GUI thread

## ❌ ANTI-PATTERNS - NEVER DO THIS

- **Direct QML → Core**: Must go through thin GUI adapters
- **Business logic in UnifiedGridRenderer**: Delegate to `core/`
- **Multiple inheritance**: Strategy pattern only
- **Qt dependencies in core/**: Except QtCore
- **Comments everywhere**: Only when absolutely critical
- **Manual memory management**: RAII everywhere

## 🔧 CRITICAL ARCHITECTURE NOTES

- **MarketDataCore timing**: Create BEFORE connecting signals (lazy initialization)
- **Strategy pattern**: Pluggable visualization modes without code changes
- **GPU rendering**: Qt Scene Graph with triple buffering
- **All UI state**: Flows through GridViewState for consistency
- **Performance monitoring**: Built-in via RenderDiagnostics

## ✅ VALIDATION REQUIREMENTS

**After ANY change:**
```bash
cmake --build --preset mac-clang              # Must compile
cd build-mac-clang && ctest --output-on-failure  # All tests pass
./apps/sentinel_gui/sentinel_gui              # Functional verification
```

**Test coverage required** for all new functionality. **Performance regression testing** for critical paths.

## 🤖 CLAUDE SUBAGENTS - AUTONOMOUS TASK SPECIALISTS

Claude Code supports **specialized AI subagents** for task-specific workflows with independent context management. These are pre-configured AI assistants that can be invoked automatically or explicitly to handle specific types of tasks.

### Quick Setup
```bash
# Open subagents interface
/agents

# Available built-in subagents for Sentinel:
# - agents/cleanup: Legacy code removal and performance optimization
# - agents/refactor: Code restructuring and architectural decomposition  
# - agents/structure: Architecture analysis and system visualization
# - agents/tailor: External plan adaptation for codebase-specific implementation
# - agents/tests: Performance baseline analysis and comprehensive unit testing
```

### Key Benefits
- **Context preservation**: Each subagent operates independently, keeping main conversation focused
- **Specialized expertise**: Domain-specific prompts and tools for higher success rates
- **Reusability**: Once created, available across projects and shareable with team
- **Flexible permissions**: Granular tool access control per subagent

### Configuration Locations
| Type | Location | Scope | Priority |
|------|----------|-------|----------|
| **Project** | `.claude/agents/` | Current project only | Highest |
| **User** | `~/.claude/agents/` | All projects | Lower |

### File Format
```markdown
---
name: sentinel-optimizer
description: Proactively optimizes C++ performance for trading latency
tools: read_file, grep, search_replace, run_terminal_cmd
---

You are a C++ performance optimization specialist for high-frequency trading systems.
Focus on: memory allocation patterns, cache efficiency, lock-free algorithms, 
and SIMD optimizations. Always benchmark before/after changes.
```

### Usage Patterns
```bash
# Automatic delegation (recommended)
"Optimize the DataProcessor for lower latency"
→ Claude automatically uses agents/cleanup or agents/tests

# Explicit invocation
"Use the refactor subagent to break down UnifiedGridRenderer.cpp"
"Have the structure subagent analyze the current architecture"
```

### Best Practices for Sentinel
- **Start with built-in agents**: Use provided specialist agents first
- **Performance-focused**: Create subagents for C++ optimization, latency testing
- **Architecture-aware**: Ensure subagents understand the 300 LOC constraint
- **Tool limitations**: Restrict powerful tools to specific subagent types only

---

**🎯 Mission**: Professional trading terminal matching Bloomberg/Bookmap standards through clean architecture, performance optimization, and comprehensive testing.