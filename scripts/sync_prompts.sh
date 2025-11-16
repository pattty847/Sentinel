#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AGENTS_FILE="$ROOT_DIR/AGENTS.md"
CLAUDE_FILE="$ROOT_DIR/CLAUDE.md"
GEMINI_FILE="$ROOT_DIR/GEMINI.md"
SENTINEL_ARCH_FILE="$ROOT_DIR/.cursor/rules/sentinel-architecture.mdc"
DETAIL_MARKER="# Sentinel Architecture Details"
HEADER='---
alwaysApply: true
---'

if [[ ! -f "$AGENTS_FILE" ]]; then
  echo "AGENTS.md not found at $AGENTS_FILE" >&2
  exit 1
fi

# Sync Claude prompt (exact copy)
cp "$AGENTS_FILE" "$CLAUDE_FILE"

# Capture existing sentinel architecture detail section if present
DETAILS=""
if [[ -f "$SENTINEL_ARCH_FILE" ]] && grep -q "$DETAIL_MARKER" "$SENTINEL_ARCH_FILE"; then
  DETAILS="$(awk -v marker="$DETAIL_MARKER" 'index($0, marker) {flag=1} flag {print}' "$SENTINEL_ARCH_FILE")"
fi

# Rebuild sentinel architecture rule with AGENTS content + optional details
{
  printf "%s\n\n" "$HEADER"
  cat "$AGENTS_FILE"
  if [[ -n "$DETAILS" ]]; then
    printf "\n%s\n" "$DETAILS"
  fi
} > "$SENTINEL_ARCH_FILE"

echo "Prompts synchronized with AGENTS.md"
