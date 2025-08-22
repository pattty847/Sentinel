#!/bin/bash

# quick_cpp_overview.sh - Quick C++ file overview using standard Unix tools
# Usage: ./quick_cpp_overview.sh <filename>

if [[ $# -eq 0 ]]; then
    echo "Usage: $0 <cpp_file> [--detailed]"
    echo "Quick overview of C++ file structure"
    exit 1
fi

FILENAME="$1"
DETAILED="$2"

if [[ ! -f "$FILENAME" ]]; then
    echo "Error: File '$FILENAME' not found"
    exit 1
fi

echo "📁 File: $FILENAME"
echo "📏 Lines: $(wc -l < "$FILENAME")"
echo

# Function signatures with return types
echo "🔧 FUNCTIONS BY RETURN TYPE:"
echo "─────────────────────────────"

# Extract and categorize functions
grep -E "^[[:space:]]*[a-zA-Z_][a-zA-Z0-9_<>*&:]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*::[a-zA-Z_~][a-zA-Z0-9_]*[[:space:]]*\\(" "$FILENAME" | \
    grep -v "#include" | \
    sed 's/^[[:space:]]*//' | \
    awk '{
        # Extract return type (first word)
        return_type = $1
        # Extract function name (after ::)
        if (match($0, /::([a-zA-Z_~][a-zA-Z0-9_]*)/)) {
            func_name = substr($0, RSTART+2, RLENGTH-2)
            print return_type "|" func_name
        }
    }' | \
    sort | \
    awk -F'|' '
    {
        if ($1 != prev_type) {
            if (prev_type != "") print ""
            print "  📋 " $1 ":"
            prev_type = $1
        }
        print "    • " $2
    }'

echo
echo "📦 CUSTOM TYPES FOUND:"
echo "─────────────────────"

# Extract Qt types, custom types, smart pointers
grep -oE '\b(Q[A-Z][a-zA-Z0-9]*|std::[a-zA-Z_][a-zA-Z0-9_]*|[A-Z][a-zA-Z0-9]*::[a-zA-Z0-9_]*)\b' "$FILENAME" | \
    grep -v "std::string\|std::vector\|std::shared_ptr\|std::unique_ptr" | \
    sort -u | \
    head -20 | \
    while read type; do
        echo "  📋 $type"
    done

echo
echo "🏗️ CLASSES & STRUCTS:"
echo "─────────────────────"

# Extract class/struct definitions
grep -E "^[[:space:]]*(class|struct)[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*" "$FILENAME" | \
    sed 's/^[[:space:]]*//' | \
    while read line; do
        echo "  📦 $line"
    done

echo
echo "📄 INCLUDES:"
echo "────────────"

# Show includes
grep "^#include" "$FILENAME" | sort | while read inc; do
    echo "  📁 $inc"
done

if [[ "$DETAILED" == "--detailed" ]]; then
    echo
    echo "🔍 DETAILED FUNCTION ANALYSIS:"
    echo "──────────────────────────────"
    
    # More detailed function analysis
    grep -n -E "^[[:space:]]*[a-zA-Z_][a-zA-Z0-9_<>*&:]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*::[a-zA-Z_~][a-zA-Z0-9_]*[[:space:]]*\\(" "$FILENAME" | \
        grep -v "#include" | \
        head -20 | \
        while IFS=: read -r line_num content; do
            echo "  📍 Line $line_num: $(echo "$content" | sed 's/^[[:space:]]*//' | cut -c1-80)..."
        done
fi

echo
echo "📊 STATISTICS:"
echo "─────────────"
func_count=$(grep -cE "^[[:space:]]*[a-zA-Z_][a-zA-Z0-9_<>*&:]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*::[a-zA-Z_~][a-zA-Z0-9_]*[[:space:]]*\\(" "$FILENAME")
class_count=$(grep -cE "^[[:space:]]*(class|struct)[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*" "$FILENAME")
include_count=$(grep -c "^#include" "$FILENAME")

echo "  🔧 Functions: $func_count"
echo "  🏗️ Classes: $class_count"
echo "  📄 Includes: $include_count"
echo "  📏 Total Lines: $(wc -l < "$FILENAME")"
