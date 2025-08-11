#!/usr/bin/env bash
# Count lines of code and estimate LLM tokens in all .cpp, .h, and .hpp files under libs/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIBS_DIR="$SCRIPT_DIR/../libs"

sort_by="lines"   # lines | tokens | name
order="desc"       # asc | desc
use_full_path=0    # 0=basename, 1=relative path under libs
output_mode="table" # table | csv | md
ext_csv="cpp,h,hpp,qml" # default extensions

usage() {
  cat <<USAGE
Usage: $(basename "$0") [options]

Options:
  --by <lines|tokens|name>   Sort by column (default: lines)
  --asc                      Sort ascending (default: descending)
  --full-path                Show paths relative to libs/ instead of basenames
  --csv                      Output CSV (no header for easy piping)
  --md                       Output Markdown table
  --ext <csv>                Only include these extensions (comma-separated). Default: cpp,h,hpp,qml
  -h, --help                 Show this help and exit
USAGE
}

while [[ ${1:-} != "" ]]; do
  case "$1" in
    --by)
      sort_by="${2:-}"; shift || true
      case "$sort_by" in lines|tokens|name) ;; *) echo "Invalid --by: $sort_by" >&2; usage; exit 1;; esac
      ;;
    --asc) order="asc" ;;
    --full-path) use_full_path=1 ;;
    --csv) output_mode="csv" ;;
    --md) output_mode="md" ;;
    --ext)
      ext_csv="${2:-}"; shift || true
      if [[ -z "$ext_csv" ]]; then echo "--ext requires a value" >&2; exit 1; fi
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
  shift
done

# Function to estimate tokens (rough approximation: 4 characters per token)
estimate_tokens() {
  local file="$1"
  local char_count
  char_count=$(wc -c < "$file")
  echo $(( char_count / 4 ))
}

rows_tmp="$(mktemp)"
trap 'rm -f "$rows_tmp"' EXIT

total_lines=0
total_tokens=0

# Build find name expression from ext_csv before scanning files
IFS=',' read -r -a exts <<< "$ext_csv"
name_args=( "(" )
first=1
for ext in "${exts[@]}"; do
  [[ -n "$ext" ]] || continue
  if [[ $first -eq 0 ]]; then
    name_args+=( -o )
  fi
  name_args+=( -name "*.${ext}" )
  first=0
done
name_args+=( ")" )

while IFS= read -r -d '' file; do
  [[ -f "$file" ]] || continue
  lines=$(wc -l < "$file")
  tokens=$(estimate_tokens "$file")
  total_lines=$(( total_lines + lines ))
  total_tokens=$(( total_tokens + tokens ))

  if [[ $use_full_path -eq 1 ]]; then
    # Trim the absolute prefix up to libs/
    display_name="${file#"$LIBS_DIR/"}"
  else
    display_name="$(basename "$file")"
  fi

  # Tab-separated for easy alignment/sorting later
  printf '%s\t%d\t%d\n' "$display_name" "$lines" "$tokens" >> "$rows_tmp"

done < <(find "$LIBS_DIR" "${name_args[@]}" -print0)

# Decide sort key
sort_args=( -t $'\t' )
case "$sort_by" in
  lines)  sort_key='-k2,2' ;;
  tokens) sort_key='-k3,3' ;;
  name)   sort_key='-k1,1f' ;;
esac
if [[ "$sort_by" != "name" ]]; then
  sort_key+="n"
fi
if [[ "$order" == "desc" ]]; then
  sort_args+=( -r )
fi

sorted_tmp="$(mktemp)"
trap 'rm -f "$rows_tmp" "$sorted_tmp"' EXIT
sort "${sort_args[@]}" $sort_key "$rows_tmp" > "$sorted_tmp"

if [[ "$output_mode" == "csv" ]]; then
  # CSV: file,lines,tokens + totals row
  echo "file,lines,tokens"
  sed 's/\t/,/g' "$sorted_tmp"
  echo "TOTAL,$total_lines,$total_tokens"
  exit 0
fi

if [[ "$output_mode" == "md" ]]; then
  # Markdown table
  echo "| File | Lines | Tokens |"
  echo "|:-----|------:|-------:|"
  awk -F '\t' '{printf("| %s | %d | %d |\n", $1, $2, $3)}' "$sorted_tmp"
  echo "| TOTAL | $total_lines | $total_tokens |"
  exit 0
fi

# Default pretty table
{
  echo -e "File\tLines\tTokens"
  echo -e "----\t-----\t------"
  cat "$sorted_tmp"
  echo -e "TOTAL\t$total_lines\t$total_tokens"
} | column -s $'\t' -t

echo
echo "Token estimation uses ~4 characters per token approximation"