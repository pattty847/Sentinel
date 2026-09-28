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

## 4a) Logs and Probes (Debug What the User Saw)

Every run of `sentinel-gui` and `sentinel-server` writes its own log file. Read it before guessing.

Where:
- macOS: `~/Library/Logs/Sentinel/sentinel-gui-latest.log`, `~/Library/Logs/Sentinel/sentinel-server-latest.log` (symlinks to the newest run)
- Older runs: `<app>-YYYYMMDD-HHMMSS-<pid>.log` in the same dir (last 20 kept, `SENTINEL_LOG_KEEP`)
- Windows/Linux: `<GenericDataLocation>/Sentinel/logs`. `SENTINEL_LOG_DIR` overrides. stderr prints `[sentinel] log file: <path>` at startup.

Read:
- Header lines start with `#`: version, pid, `exe=... built=...` (check the binary is not stale vs your change), args, cwd, `SENTINEL_*`/`QT_*`/`QSG_*` env.
- Line format: `<local time> <D/I/W/E/F> <category> <thread> <file:line> | <message>`
- Categories: `app`, `data`, `render`, `debug`, `probe`, plus Qt's own (`qt.*`, `default` for plain qDebug).
- Start with `rg ' [WEF] ' <log>`, then narrow by the time the user describes, category, and thread (`main`, `QSGRenderThread`, ...).

Probes (values for a specific behavior, off by default):
- List them: `rg -o 'sLog_Probe\("[^"]+"' libs apps | sort -u`
- Enable by name or prefix: `SENTINEL_PROBES=tpo,heatmap.window` (`all` enables every probe). A prefix enables its children (`tpo` -> `tpo.ingest`).
- If the log lacks the values you need, ask the user to rerun with the probes on, or add a probe and ask them to reproduce. Example: `SENTINEL_PROBES=heatmap ./build/mac-clang/apps/sentinel-gui/sentinel-gui`

Write logs (`libs/core/SentinelLogging.hpp`):
- `sLog_App/Data/Render/Debug(...)`: every call prints. State changes and one-off events. Include identifying values as `key=value` (symbol, tf, range, counts).
- `sLog_Warning/sLog_Error(...)`: problems. Do not swallow a failure silently.
- `sLog_Probe("area.detail", "k=" << v)`: anything per frame, per message, or per bucket. Name is a string literal; checked once per call site, free when off.
- `sLog_*N(ms, ...)`: per-site time throttle, only for an always-on recurring line.
- Do not gate logging with ad-hoc env vars, and do not write side files (`/tmp/*.log`, `.cursor/debug.log`). Everything goes through Qt logging so it lands in the run log.
- Too noisy: `QT_LOGGING_RULES="sentinel.render.debug=false"` silences a category.

## 4b) See and Measure the Running App

Look before you claim a visual or performance result.

- Screenshot: with `sentinel-gui` running, `curl -s 'http://127.0.0.1:17100/screenshot?name=<name>'` returns `{"ok":true,"path":"./screenshots/<name>.png"}` (relative to the GUI's cwd, normally the repo root). Port is `gui.api_port` in `config/client_config.yaml`.
- Agent API state, viewport, and screenshot routes: see `docs/AGENT_API.md`.
- Viewing it: Claude Code reads the PNG directly; Codex opens local images mid-task on its own (verified 2026-09-27) or takes them up front with `codex exec -i <png>`. A sandboxed Codex run cannot launch the GUI (`Cannot create window: no screens available`), so it cannot take its own screenshots: report the visual check as unverified and the orchestrator runs it.
- Input: the dev build is a raw binary with no app bundle, so computer-use tools cannot drive it. Ask the owner to pan, zoom or click, then read the run log and screenshot.
- Frame cost: `SENTINEL_FRAME_PROFILE=1` prints per-stage `updatePaintNode` timings once per second into the run log (section 4a).
- CPU: `sample <pid> <seconds> -file <out>` (macOS). Work that happens outside `updatePaintNode` (Qt texture uploads, QML, other threads) only shows up here.

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
- Line endings: LF everywhere (`.gitattributes` enforces it; `.ps1`/`.bat` check out as CRLF). Never write CRLF into other files.

## 9) References (Read on Demand Not By Default)

- `docs/ARCHITECTURE.md`
- `docs/MARKETDATA.md`
- `docs/TODO.md`

Read these only if the task actually needs them.

## 10) Cross-Agent Delegation (Codex CLI)

This is the single agent-instructions file: Claude Code and Codex both read it (`CLAUDE.md` only imports it). Edit rules here, never in a copy.

Routing (starting defaults; the orchestrator recalibrates them as results come in):
- **Orchestrator (Claude Code session the owner is talking to):** direction, cross-cutting design, audits, merges, anything touching hot paths or several subsystems at once.
- **Codex `gpt-6-sol`, effort high:** a well-specified bug fix or small feature with clear acceptance checks, in its own worktree.
- **Codex `gpt-6-astra`, effort high or above:** harder self-contained work: deeper reasoning, larger isolated refactors, second-opinion reviews (`-s read-only`).
- **Read-only review before merge:** a different model from the one that wrote the change.
- A delegated agent does not delegate further unless its prompt explicitly allows it, and never merges its own branch.

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

Hand-off protocol (every delegated write task):
1. Work in your own worktree and branch, created from current `main` with `scripts/dev/agent-worktree.sh create <branch>` (branches: `lt-sol/...` for Codex gpt-6-sol, `lt-astra/...` for gpt-6-astra). It puts the worktree on the T7 drive when mounted, supplies `VCPKG_ROOT` and ninja, and configures the build; ccache makes the first build take seconds. Remove it after merge with `... remove <branch>`.
2. Before reporting ready, merge the latest `main` into your branch (`git merge main`), resolve any conflicts yourself, rebuild, and run `ctest` in `build/mac-clang`.
3. Finish with a message whose first line is `READY: <branch>`, followed by: what changed and why, the tests you ran with their summary line, and anything you could not verify (visual checks, live runs). Say `BLOCKED: <branch>` with the reason instead if you cannot finish.
4. The orchestrator reviews the diff with a different model, merges with `--no-ff`, and removes the worktree. Never merge your own branch.
