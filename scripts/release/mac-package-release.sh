#!/usr/bin/env bash
# Compatibility wrapper — use scripts/release_macos.sh directly.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/release_macos.sh" "$@"
