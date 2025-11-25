#!/bin/bash

# extract_functions.sh - Extract function signatures from C++ files
# Usage: ./extract_functions.sh <filename> [options]
# Options:
#   --names-only     Show only function names
#   --signatures     Show full signatures (default)
#   --count          Show count only
#   --sort           Sort alphabetically
#   --class <name>   Filter by specific class name
#   --type <type>    Filter by return type (void, int, double, etc.)

set -e

# Default values
FILENAME=""
SHOW_NAMES_ONLY=false
SHOW_COUNT_ONLY=false
SORT_OUTPUT=false
CLASS_FILTER=""
TYPE_FILTER=""

# Function to show usage
show_usage() {
    echo "Usage: $0 <filename> [options]"
    echo ""
    echo "Extract function signatures from C++ files"
    echo ""
    echo "Options:"
    echo "  --names-only     Show only function names"
    echo "  --signatures     Show full signatures (default)"
    echo "  --count          Show count only"
    echo "  --sort           Sort alphabetically"
    echo "  --class <name>   Filter by specific class name"
    echo "  --type <type>    Filter by return type (void, int, double, etc.)"
    echo ""
    echo "Examples:"
    echo "  $0 libs/gui/UnifiedGridRenderer.cpp"
    echo "  $0 libs/gui/UnifiedGridRenderer.cpp --names-only"
    echo "  $0 libs/gui/UnifiedGridRenderer.cpp --class UnifiedGridRenderer --type void"
    echo "  $0 libs/gui/UnifiedGridRenderer.cpp --count"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --names-only)
            SHOW_NAMES_ONLY=true
            shift
            ;;
        --signatures)
            SHOW_NAMES_ONLY=false
            shift
            ;;
        --count)
            SHOW_COUNT_ONLY=true
            shift
            ;;
        --sort)
            SORT_OUTPUT=true
            shift
            ;;
        --class)
            CLASS_FILTER="$2"
            shift 2
            ;;
        --type)
            TYPE_FILTER="$2"
            shift 2
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            if [[ -z "$FILENAME" ]]; then
                FILENAME="$1"
            else
                echo "Error: Unknown option $1"
                show_usage
                exit 1
            fi
            shift
            ;;
    esac
done

# Check if filename is provided
if [[ -z "$FILENAME" ]]; then
    echo "Error: No filename provided"
    show_usage
    exit 1
fi

# Check if file exists
if [[ ! -f "$FILENAME" ]]; then
    echo "Error: File '$FILENAME' not found"
    exit 1
fi

# Build the grep pattern - more precise to avoid false positives
# Look for: return_type ClassName::functionName(parameters)
PATTERN="^[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*::[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\\("

# Add class filter if specified
if [[ -n "$CLASS_FILTER" ]]; then
    PATTERN="^[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]+${CLASS_FILTER}::[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\\("
fi

# Add type filter if specified
if [[ -n "$TYPE_FILTER" ]]; then
    PATTERN="^[[:space:]]*${TYPE_FILTER}[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*::[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\\("
fi

# If both class and type filters are specified
if [[ -n "$CLASS_FILTER" && -n "$TYPE_FILTER" ]]; then
    PATTERN="^[[:space:]]*${TYPE_FILTER}[[:space:]]+${CLASS_FILTER}::[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\\("
fi

# Alternative simpler pattern that's more reliable
SIMPLE_PATTERN="^[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*::"

# Extract functions
if [[ "$SHOW_COUNT_ONLY" == "true" ]]; then
    # Just show count - filter out includes, comments, and declarations
    count=$(grep -E "$SIMPLE_PATTERN" "$FILENAME" | grep -v "#include" | grep -v "//" | grep -v ";" | grep -v "std::" | wc -l)
    echo "$count functions found"
elif [[ "$SHOW_NAMES_ONLY" == "true" ]]; then
    # Show only function names
    if [[ "$SORT_OUTPUT" == "true" ]]; then
        grep -E "$SIMPLE_PATTERN" "$FILENAME" | grep -v "#include" | grep -v "//" | grep -v ";" | grep -v "std::" | sed 's/.*::\([a-zA-Z_][a-zA-Z0-9_]*\).*/\1/' | sort
    else
        grep -E "$SIMPLE_PATTERN" "$FILENAME" | grep -v "#include" | grep -v "//" | grep -v ";" | grep -v "std::" | sed 's/.*::\([a-zA-Z_][a-zA-Z0-9_]*\).*/\1/'
    fi
else
    # Show full signatures
    if [[ "$SORT_OUTPUT" == "true" ]]; then
        grep -E "$SIMPLE_PATTERN" "$FILENAME" | grep -v "#include" | grep -v "//" | grep -v ";" | grep -v "std::" | sed 's/^[[:space:]]*//' | sort
    else
        grep -E "$SIMPLE_PATTERN" "$FILENAME" | grep -v "#include" | grep -v "//" | grep -v ";" | grep -v "std::" | sed 's/^[[:space:]]*//'
    fi
fi
