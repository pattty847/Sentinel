# 🔍 C++ Code Analysis Tools

This directory contains several tools for analyzing C++ code structure and extracting function signatures, types, and classes.

## 📋 Available Tools

### 1. `quick_cpp_overview.sh` - Fast Overview (Recommended)
**Best for: Quick understanding of any C++ file**

```bash
# Basic overview
./scripts/quick_cpp_overview.sh libs/gui/UnifiedGridRenderer.cpp

# Detailed analysis with line numbers
./scripts/quick_cpp_overview.sh libs/gui/UnifiedGridRenderer.cpp --detailed
```

**Features:**
- ✅ Functions grouped by return type (void, QSGNode*, QString, etc.)
- ✅ Custom types and Qt types found
- ✅ Classes and structs
- ✅ Include statements
- ✅ Statistics summary
- ✅ Works on any C++ file
- ✅ Fast and reliable

### 2. `analyze_cpp.py` - Advanced Python Analyzer
**Best for: Detailed analysis and JSON output**

```bash
# Complete analysis
python3 scripts/analyze_cpp.py libs/gui/UnifiedGridRenderer.cpp

# Functions only
python3 scripts/analyze_cpp.py libs/gui/UnifiedGridRenderer.cpp --functions --names-only --sort

# Summary by return type
python3 scripts/analyze_cpp.py libs/gui/UnifiedGridRenderer.cpp --summary

# JSON output for integration
python3 scripts/analyze_cpp.py libs/gui/UnifiedGridRenderer.cpp --json
```

**Features:**
- ✅ Precise regex-based parsing
- ✅ JSON output for automation
- ✅ Constructor/destructor detection
- ✅ Multiple output formats
- ✅ Sorting and filtering options

### 3. `extract_functions.sh` - Simple Function Extractor
**Best for: Quick function name extraction**

```bash
# All functions
./scripts/extract_functions.sh libs/gui/UnifiedGridRenderer.cpp

# Just function names, sorted
./scripts/extract_functions.sh libs/gui/UnifiedGridRenderer.cpp --names-only --sort

# Count functions
./scripts/extract_functions.sh libs/gui/UnifiedGridRenderer.cpp --count

# Filter by return type
./scripts/extract_functions.sh libs/gui/UnifiedGridRenderer.cpp --type void
```

## 🎯 Use Cases

### Understanding New Files
```bash
# Get quick overview of any file
./scripts/quick_cpp_overview.sh libs/core/MarketDataCore.cpp
```

### Documentation Generation
```bash
# Generate function list for docs
python3 scripts/analyze_cpp.py libs/gui/UnifiedGridRenderer.cpp --functions --names-only --sort
```

### Code Review
```bash
# Check function distribution
./scripts/quick_cpp_overview.sh src/MyClass.cpp --detailed
```

### API Surface Analysis
```bash
# See all public methods and their types
python3 scripts/analyze_cpp.py libs/gui/UnifiedGridRenderer.cpp --summary
```

## 🔧 Why These Tools vs VS Code?

**VS Code (`Cmd+Shift+O`) Limitations:**
- ❌ Only shows `void` functions reliably
- ❌ Misses return types like `QSGNode*`, `QString`, etc.
- ❌ Doesn't group by type
- ❌ No custom type extraction
- ❌ No include analysis

**Our Tools:**
- ✅ Show ALL function signatures with return types
- ✅ Group functions by return type for easy understanding
- ✅ Extract custom types (Qt types, smart pointers, etc.)
- ✅ Analyze includes and dependencies
- ✅ Work on any file, any size
- ✅ Fast command-line operation
- ✅ Perfect for documentation and code review

## 🚀 Quick Examples

### See All QSGNode Functions
```bash
./scripts/quick_cpp_overview.sh libs/gui/UnifiedGridRenderer.cpp | grep -A 10 "QSGNode"
```

### Find All Qt Types Used
```bash
./scripts/quick_cpp_overview.sh libs/gui/UnifiedGridRenderer.cpp | grep "Q[A-Z]"
```

### Count Functions by Type
```bash
python3 scripts/analyze_cpp.py libs/gui/UnifiedGridRenderer.cpp --summary | grep "("
```

### Generate Function List for Documentation
```bash
echo "## Functions in UnifiedGridRenderer" > functions.md
echo "" >> functions.md
python3 scripts/analyze_cpp.py libs/gui/UnifiedGridRenderer.cpp --functions --names-only --sort | sed 's/^/- /' >> functions.md
```

## 💡 Pro Tips

1. **Start with `quick_cpp_overview.sh`** - gives you everything you need 90% of the time
2. **Use `--detailed` for line numbers** when you need to jump to specific functions
3. **Pipe to `grep`** to filter for specific types: `| grep "QString"`
4. **Use JSON output** for automation: `--json | jq '.functions[].function_name'`
5. **Combine with other tools**: `| head -20`, `| sort`, `| uniq -c`

## 🔍 Advanced Usage

### Find All Error Handling Functions
```bash
./scripts/quick_cpp_overview.sh src/MyFile.cpp | grep -i "error\|exception"
```

### Compare Two Files
```bash
diff <(./scripts/quick_cpp_overview.sh file1.cpp) <(./scripts/quick_cpp_overview.sh file2.cpp)
```

### Find Memory Management Patterns
```bash
./scripts/quick_cpp_overview.sh src/MyFile.cpp | grep -E "shared_ptr|unique_ptr|delete|new"
```

---

These tools solve the exact problem you mentioned - VS Code's symbol search is limited, but these give you comprehensive insight into any C++ file's structure! 🎉
