#!/bin/bash
# Count lines of code and estimate LLM tokens in all .cpp, .h, and .hpp files under libs/

LIBS_DIR="$(dirname "$0")/../libs"

# Function to estimate tokens (rough approximation: 4 characters per token)
estimate_tokens() {
    local file="$1"
    local char_count=$(wc -c < "$file")
    echo $((char_count / 4))
}

echo "Per-file line counts and token estimates:" >&2
echo "File: Lines | Tokens" >&2
echo "---------------------" >&2

total_lines=0
total_tokens=0

# Process each file individually to get both line and token counts
while IFS= read -r -d '' file; do
    if [ -f "$file" ]; then
        lines=$(wc -l < "$file")
        tokens=$(estimate_tokens "$file")
        echo "$(basename "$file"): $lines lines | ~$tokens tokens" >&2
        
        total_lines=$((total_lines + lines))
        total_tokens=$((total_tokens + tokens))
    fi
done < <(find "$LIBS_DIR" \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print0)

echo -e "\nSummary:" >&2
echo "Total lines of code in libs/ (.cpp/.h/.hpp): $total_lines"
echo "Estimated LLM tokens: ~$total_tokens"
echo "Token estimation uses ~4 characters per token approximation" 