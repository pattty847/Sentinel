#!/bin/bash
# Print AI subscription budget left (Claude, Codex) from the CodexBar CLI, one line per window.
# The orchestrator runs this before dispatching work to route by remaining budget.
# Usage: scripts/dev/budget.sh [--json]
set -u
CB=$(command -v codexbar || echo /Applications/CodexBar.app/Contents/Helpers/CodexBarCLI)
[ -x "$CB" ] || { echo "budget: CodexBar CLI not found" >&2; exit 1; }
# codexbar exits non-zero when any one provider fails; the JSON for the others is still valid.
out=$("$CB" --format json --provider both 2>/dev/null || true)
[ -n "$out" ] || out=$("$CB" --format json 2>/dev/null || true)
[ "${1:-}" = "--json" ] && { printf '%s\n' "$out"; exit 0; }
printf '%s' "$out" | python3 -c '
import json, sys, datetime as dt
try:
    data = json.load(sys.stdin)
except Exception:
    print("budget: could not parse CodexBar output"); sys.exit(1)
names = {"primary": "session", "secondary": "weekly", "tertiary": "extra"}
now = dt.datetime.now(dt.timezone.utc)
for p in (data if isinstance(data, list) else [data]):
    prov = p.get("provider")
    if prov not in ("claude", "codex"):
        continue
    usage = p.get("usage") or {}
    labels = usage.get("rateWindowLabels") or {}
    for key in ("primary", "secondary", "tertiary"):
        w = usage.get(key)
        if not isinstance(w, dict) or "usedPercent" not in w:
            continue
        left = 100 - w["usedPercent"]
        reset = w.get("resetsAt") or ""
        eta = ""
        if reset:
            try:
                h = (dt.datetime.fromisoformat(reset.replace("Z", "+00:00")) - now).total_seconds() / 3600
                eta = f" resets in {h/24:.1f}d" if h >= 24 else f" resets in {h:.1f}h"
            except ValueError:
                pass
        label = labels.get(key) or names[key]
        print(f"{prov:6} {label:10} {left:3.0f}% left{eta}")
'
