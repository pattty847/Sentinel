# AGENTS.md — Sentinel Agent Operating Rules (Slim)

Do not flatter, validate, or agree by default.
Your job is to be correct, not agreeable.
Challenge assumptions, point out errors, and highlight weak reasoning immediately.
If the user is wrong, say it plainly and explain why.
If uncertain, state uncertainty instead of guessing.
Avoid praise unless it is explicitly earned and relevant.
Optimize for truth, clarity, and usefulness—never for likability.

Source of truth for coding agents working on Sentinel.
Default goal: make the requested change safely.

## 0) Fast Start (Read Minimum First)

Use the smallest context that can solve the task.

Modes:
- **Lite** (questions, docs, tooling, no code edits): do not preload `_agent/` or `docs/TODO.md`
- **CodeChange** (implement/refactor): read `_agent/INVARIANTS.md`
- **Debug/Perf/Regression**: read `_agent/INVARIANTS.md` + `_agent/FAILURE_MODES.md`

Rules:
- Start with targeted search (`rg -n`, `rg --files`), not broad file reads.
- Before first concrete action, read at most ~200 lines total unless user asks for deep review.
- If request is ambiguous, start in Lite mode and escalate only if needed.
- If you discover a durable invariant/failure mode/decision, update `_agent/` before stopping.

## 1) Sentinel Non-Negotiables (Core Identity)

Protect these at all times:
- **Core stays free of GUI Qt** (QtCore is allowed: QObject/signals, QTimer, QByteArray, QString; no QtGui, QtQuick, QML or QSG in core)
- **GUI owns Qt/QML/QSG behavior**
- **Rendering is GPU-first, deterministic, and low-lag**

Prefer simpler/faster designs over preserving weak legacy patterns unless compatibility is explicitly required.

## 2) Critical Invariants

- **Viewport updates must go through `setViewport()`** so `viewportVersion` increments.
  Do not mutate viewport fields directly. If `viewportVersion` does not change, rebuild logic can fail.

- **Threading**
  - Network and data processing stay off GUI thread
  - Cross-thread communication uses `Qt::QueuedConnection`
  - QSG/render-thread code must not touch GUI `QObject` graphs

- **Render path behavior**
  - Preallocate/reuse QSG geometry and nodes where practical
  - Validate inputs (skip NaN/inf)
  - Avoid unnecessary node/geometry churn in hot paths

## 3) Where Things Belong (Ownership Boundaries)

- `libs/core`: market data transport, dispatchers, order books, DTOs, transforms (non-UI logic)
- `libs/gui`: Qt/QML/QSG, rendering strategies, window/widget behavior
- `apps/`: thin bootstraps only (no business logic)

If a change crosses these boundaries, stop and justify it before proceeding.

## 4) Commands (Use These First)

Build:
- `cmake --build --preset windows-msvc-vs/mac-clang`

Runtime/testing:
- Prefer targeted validation for touched area first
- Full-suite or long-running passes only if requested or clearly necessary

Cheap verification ladder:
1. Format/lint touched scope only
2. Build affected target(s)
3. Run targeted tests / targeted repro
4. Ask before broad/full runs unless user asked for it

## 5) Hot Paths (Treat Like Live Wires)

Changes here require performance caution and small diffs:
- `DataProcessor`
- `HeatmapTwapStreamer`
- `HeatmapLabelRenderer`
- QSG geometry updates

Before editing hot paths, explicitly check for per-frame risk:
- allocations
- object creation/destruction
- signal emissions
- binding churn / unnecessary recomputation

## 6) Agent Memory (`_agent/`) — Durable, Minimal, One-Line

Path: `_agent/` (gitignored AI scratchpad)

Files:
- `_agent/INVARIANTS.md` → `- INV-### | <statement>`
- `_agent/FAILURE_MODES.md` → `- FM-### | Symptom: <...> | Root: <...> | Guardrail: <...>`
- `_agent/REPO_MAP.md` → `- AREA: <path> | Owns: <...> | Touch with: <...> | Notes: <...>`
- `_agent/DECISIONS.md` → `- YYYY-MM-DD | Decision | Why: <...> | Rejected: <...>`

Rules:
- One line per entry, ASCII only
- Do not add new `_agent` files unless user asks
- Prefer durable guardrails over session chatter
- Qdrant indexing is directory-targeted only (`libs/`, `docs/`, optional source dirs), never repo root, never `build/`

## 7) Task Tracking (`docs/TODO.md`) — Only When Relevant

Read/update `docs/TODO.md` only when:
- user asks for TODO / feature / session-log work
- task changes scope or priorities
- ending session and log update is needed

Use targeted reads, not full dumps.
Do not reorder or renumber feature blocks.

## 8) Canonical Docs to Update When Needed

Update only the relevant canonical doc in the same change:
- Protocol / wire / DTO semantics → `docs/MARKETDATA.md`
- Architecture / ownership / dependency direction → `docs/ARCHITECTURE.md`
- Feature scope/progress → `docs/TODO.md`
- Config keys/defaults/semantics → config docs (if present)
- New durable invariant/regression guardrail → `_agent/INVARIANTS.md` or `_agent/FAILURE_MODES.md`
- Non-trivial design decision → `_agent/DECISIONS.md` (vault detail optional)

## 8a) Commit Checkpoints

- Prefer manual git commits after a coherent batch of logic lands and verifies cleanly.
- Group commits by feature or infrastructure slice, not by file type.
- Default checkpoint rule: if a meaningful feature seam is implemented and targeted validation passed, make a commit unless the user says not to.
- Do not sweep unrelated modified files into the same commit; leave unrelated worktree changes alone.
- Commit messages should say what changed and why at the feature level, not just "fix stuff".

## 9) References (Read on Demand Not By Default)

- `docs/ARCHITECTURE.md`
- `docs/MARKETDATA.md`
- `docs/TODO.md`

Read these only if the task actually needs them.

## 10) Cross-Agent Delegation (Codex CLI)

The owner's ChatGPT subscription can run Codex agents headless, to spread work across subscriptions.
Verified 2026-09-27 with codex-cli 0.158.0-alpha.2.1.

- Binary: `/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex`. `codex` is only a zsh alias; use the full path in scripts.
- Models that answered a live `codex exec` call: `gpt-6-sol` (config default) and `gpt-6-astra`. Also listed for this account: `gpt-5.6-sol`, `gpt-5.6-terra`, `gpt-5.6-luna`, `gpt-5.5`. Reasoning effort: `low`..`max`, plus `ultra` on 6-astra and 5.6-sol/terra. Re-check `~/.codex/models_cache.json` before relying on a model.
- Run: `codex exec -C <dir> -s read-only|workspace-write -m gpt-6-sol -c model_reasoning_effort='"high"' --json -o <last-message.txt> "<prompt>"`
- Thread id: the first JSONL event is `{"type":"thread.started","thread_id":"<uuid>"}`. Continue with `codex exec resume <uuid> "<follow-up>"` (or `--last`).
- Reviews: `codex exec review` runs a code review of the current repo.
- Isolation: write tasks run in their own git worktree (`--worktree`, or `-C` into a `git worktree add` path) so two agents never edit the same checkout. Audits and reviews use `-s read-only`.
- Never pass `--dangerously-bypass-approvals-and-sandbox`.
- Prompts must stand alone: point the agent at `AGENTS.md`, the files, the acceptance checks, and the build/test commands.
- The delegating agent reviews the resulting diff and runs the verification ladder (section 4) before anything merges.
