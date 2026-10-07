# AGENTS.md — Sentinel Agent Operating Rules

Do not flatter, validate, or agree by default.
Your job is to be correct, not agreeable.
Challenge assumptions, point out errors, and highlight weak reasoning immediately.
If the user is wrong, say it plainly and explain why.
If uncertain, state uncertainty instead of guessing.
Avoid praise unless it is explicitly earned and relevant.
Optimize for truth, clarity, and usefulness—never for likability.

Source of truth for every coding agent on Sentinel. Claude Code and Codex both read this file
(`CLAUDE.md` only imports it); edit rules here, never in a copy. Live state is in `docs/STATUS.md`.
How-to detail (GUI host, Codex CLI, lessons) is in `docs/AGENT_WORKFLOW.md`; read it when a task needs it.
Default goal: make the requested change safely.

## 0) Fast Start (Read Minimum First)

Use the smallest context that can solve the task.

Modes:
- **Lite** (questions, docs, tooling, read-only review): do not preload `_agent/` or `docs/TODO.md`
- **CodeChange** (implement/refactor): `rg -n '<area|file|symbol>' _agent/INVARIANTS.md` for what you touch; read the matching entries, not the whole file
- **Debug/Perf/Regression**: the same search in `_agent/INVARIANTS.md` and `_agent/FAILURE_MODES.md`

Rules:
- Start with targeted search (`rg -n`, `rg --files`), not broad file reads.
- Before first concrete action, read at most ~200 lines total unless the user asks for a deep review.
- If the request is ambiguous, start in Lite mode and escalate only if needed.
- **Read-only scope** (review, audit, investigation): edit nothing, including `_agent/`, docs and memory. Report durable findings to whoever asked. An explicit owner scope always overrides the write rules in this file.
- Write tasks: if you discover a durable invariant/failure mode/decision, update `_agent/` before stopping.

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

If a change crosses these boundaries, state why before implementing it. Stop and ask only when it
changes behaviour, data or looks (owner decisions) or needs an approval listed in this file.

## 4) Commands and Testing

Build (owner's Mac): `scripts/dev/build-queue.sh --label <branch> -- cmake --build --preset mac-clang -j 2`
- Every build, ctest run or benchmark on the Mac goes through the FIFO build queue, one at a time.
- Use `-j 2` until the owner raises it (the desktop froze under heavier load on 2026-10-05).
- Windows: `cmake --build --preset windows-msvc-vs`.

Verification ladder:
1. Format/lint touched scope only
2. Build affected target(s)
3. Run targeted tests / targeted repro. Writers stop here unless their task packet says otherwise.
4. The full `ctest` suite runs in the conductor's landing gate (`agent-worktree.sh land`). It is part of an authorized landing and needs no extra ask. Ask before any other full or long-running pass unless the task requests it.

Reading results:
- A skipped GPU case still reports Passed. Read the output for `GPU case skipped`; llvmpipe is not GPU verification.
- Synthetic fixtures are not live-provider proof. Say which one you ran.
- Check the log header `exe=... built=...` so you do not test a stale binary.

## 4a) Logs and Probes (Debug What the User Saw)

Every run of `sentinel-gui` and `sentinel-server` writes its own log file. Read it before guessing.

Where:
- macOS: `~/Library/Logs/Sentinel/sentinel-gui-latest.log`, `~/Library/Logs/Sentinel/sentinel-server-latest.log` (symlinks to the newest run)
- Older runs: `<app>-YYYYMMDD-HHMMSS-<pid>.log` in the same dir (last 20 kept, `SENTINEL_LOG_KEEP`)
- Windows/Linux: `<GenericDataLocation>/Sentinel/logs`. `SENTINEL_LOG_DIR` overrides. stderr prints `[sentinel] log file: <path>` at startup.

Read:
- Header lines start with `#`: version, pid, `exe=... built=...`, args, cwd, `SENTINEL_*`/`QT_*`/`QSG_*` env.
- Line format: `<local time> <D/I/W/E/F> <category> <thread> <file:line> | <message>`
- Categories: `app`, `data`, `render`, `debug`, `probe`, plus Qt's own (`qt.*`, `default` for plain qDebug).
- Start with `rg ' [WEF] ' <log>`, then narrow by the time the user describes, category, and thread (`main`, `QSGRenderThread`, ...).

Probes (values for a specific behavior, off by default):
- List them: `rg -o 'sLog_Probe\("[^"]+"' libs apps | sort -u`
- Enable by name or prefix: `SENTINEL_PROBES=tpo,heatmap.window` (`all` enables every probe). A prefix enables its children (`tpo` -> `tpo.ingest`).
- If the log lacks the values you need, ask the user to rerun with the probes on, or add a probe and ask them to reproduce.

Write logs (`libs/core/SentinelLogging.hpp`):
- `sLog_App/Data/Render/Debug(...)`: every call prints. State changes and one-off events. Include identifying values as `key=value` (symbol, tf, range, counts).
- `sLog_Warning/sLog_Error(...)`: problems. Do not swallow a failure silently.
- `sLog_Probe("area.detail", "k=" << v)`: anything per frame, per message, or per bucket. Name is a string literal; checked once per call site, free when off.
- `sLog_*N(ms, ...)`: per-site time throttle, only for an always-on recurring line.
- Do not gate logging with ad-hoc env vars, and do not write side files (`/tmp/*.log`, `.cursor/debug.log`). Everything goes through Qt logging so it lands in the run log.
- Too noisy: `QT_LOGGING_RULES="sentinel.render.debug=false"` silences a category.

## 4b) Running the App, Screenshots and Services (Hard Rules)

Look before you claim a visual or performance result. Manual (GUI host commands, Agent API, cloud GUI,
recorded-data dumps, profiling): `docs/AGENT_WORKFLOW.md` "Running and seeing the app", `docs/AGENT_API.md`.

Screenshots and GUI runs:
- Allowed targets: `window`, `heatmap`, a retained dock ID (`orderBook`, `watchlist`, `screener`, `stockChart`, `paperTrading`, `sec`, `copenet`, `telemetry`), `statusBar`, `toolbar`, `chartmenu`, `settings[:Tab]`. Show or focus a hidden dock through the dock API first.
- Never use `target=main`: it captures whatever window covers the GUI, including other apps' private content. If any shot shows something that is not Sentinel, delete it at once and tell the conductor. This includes `scripts/dev/cloud-gui.sh`: pass `window` explicitly, because its `shot` defaults to `main`.
- Every agent-run GUI is isolated: use the GUI host (`scripts/dev/gui-shot.sh`), or launch with `--agent-host <scratch dir>`, its own `--api-port` (17110 + n), `--no-screener` and its own scratch directory. Never run a GUI against the owner's settings.
- Never drive the owner's own GUI: its Agent API (`gui.api_port`, 17100) has no authentication.
- One hosted GUI session at a time. No automated GUI windows while the owner is working at the Mac unless the owner asks.
- Never run the GUI binary with `--help` (it starts a full GUI).
- Do not add a native-format `QSettings("org","app")` to the GUI: use `QSettings(QSettings::defaultFormat(), QSettings::UserScope, "org", "app")` (a test enforces it).
- Screenshots fail with `grab_failed` while the Mac screen is locked (`ioreg -n Root -d1 -a | grep -A1 ScreenIsLocked`).

Always-on services (recorder `com.sentinel.recorder`, capture `com.sentinel.capture`, launchd, `~/Sentinel-runtime/bin`):
- Writers and reviewers never stop, restart, replace or deploy them, and never run a bare `sentinel-server` from the repo (it contends for the recorder's data locks).
- Only the conductor deploys, only with `scripts/dev/deploy-runtime.sh server|capture|both` (copy, sign, restart, verify writes from the restarted PID's log: server 150 s, capture 60 s; auto-rollback; no server deploy 23:55-00:05 UTC), one service at a time, then reads the run log and metrics.
- The conductor may redeploy a change that is reviewed and landed on `main`, then tells the owner.
- The recorder reads `config/server_config.yaml` (and `config/.server_config.yaml`) from the main checkout at every start. Keep the main checkout on `main` and clean; never check out another branch there. A server config change is a reviewed commit on `main` and takes effect at the next restart, so treat it like a deploy.
- Never read, copy, export or change the signing identity ("Sentinel Local Code Signing") or other keys; only `deploy-runtime.sh` uses it.
- Cutovers (for example roller slice D), recording or on-disk format changes and data deletions need explicit owner approval with the owner present at the Mac.
- Never delete, move or rewrite recorded data under `data/` or `/Volumes/T7` (recordings, journals, `hmc2`). The only exception is the conductor carrying out an owner-approved deletion or cutover plan with the owner present.
- Any review of code or config that runs outside a sandbox (GUI host, deploy, runtime scripts) must ask about every runtime load of agent-writable code or config (QML from source dirs, plugin paths, config files, caches), not only the diff.

## 4c) Metrics (after `ops/monitoring/install.sh` has run)

- Instant: `curl -s 127.0.0.1:8090/metrics` (sentinel-server), `127.0.0.1:8091/metrics` (capture). History: `curl -s 127.0.0.1:8428/api/v1/query --data-urlencode 'query=<promql>'` (VictoriaMetrics, 1 y). Dashboards: http://127.0.0.1:3000.
- Examples: `up`, `max by (product,layer) (sentinel_recorder_column_overdue_seconds)`, `increase(sentinel_recorder_invalidations_total[1h])`.
- Metric list, alerts and how to add a metric: `ops/monitoring/README.md`. Hot paths only touch `Counter`/`Gauge` (one relaxed atomic); samplers run on the main thread at scrape.

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

Path: `_agent/` (gitignored AI scratchpad, shared by every worktree and agent). Write tasks only.

Files:
- `_agent/INVARIANTS.md` → `- INV-### | <statement>`
- `_agent/FAILURE_MODES.md` → `- FM-### | Symptom: <...> | Root: <...> | Guardrail: <...>`
- `_agent/REPO_MAP.md` → `- AREA: <path> | Owns: <...> | Touch with: <...> | Notes: <...>`
- `_agent/DECISIONS.md` → `- YYYY-MM-DD | Decision | Why: <...> | Rejected: <...>`

Rules:
- One line per entry, ASCII only
- Do not add new `_agent` files unless user asks
- Re-read the file tail right before appending and take the next free id (`rg -o '^- FM-[0-9]+' _agent/FAILURE_MODES.md | sort -V | tail -1`)
- Prefer durable guardrails over session chatter; when an entry you touch says "open" and the fix has landed, mark it resolved with the commit
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
- Live state (conductor, services, in-flight branches, waiting decisions) → `docs/STATUS.md` (conductor only)
- New durable invariant/regression guardrail → `_agent/INVARIANTS.md` or `_agent/FAILURE_MODES.md`
- Non-trivial design decision → `_agent/DECISIONS.md` (vault detail optional)

`docs/research/*` files are dated plans and evidence. Owner decisions are appended when a plan is
approved; after the work lands the file is frozen and nobody maintains it. Current truth is the code,
this file and `docs/STATUS.md`.

## 8a) Commits and Git

- Commit a coherent, verified feature slice; do not sweep unrelated modified files into it. Messages say what changed and why at the feature level.
- Writers whose harness can write `.git` may commit on their own branch. Sandboxed writers (Codex `workspace-write` cannot write a linked worktree's index) leave a clean diff and report it; the conductor commits for them.
- Writers never rebase onto `main`, merge or push. The conductor rebases during landing, lands and pushes.
- Push `main` only after clean landings and a secret scan; never force.
- Never commit secrets (the ntfy topic lives only in `ops/monitoring/ntfy.env`), `.codex/`, screenshots, or changes the owner is making in parallel.
- Line endings: LF everywhere (`.gitattributes` enforces it; `.ps1`/`.bat` check out as CRLF). Never write CRLF into other files.

## 9) References (Read on Demand Not By Default)

- `docs/ARCHITECTURE.md`, `docs/MARKETDATA.md`, `docs/TODO.md`
- `docs/AGENT_WORKFLOW.md` (GUI host, Codex CLI, lessons), `docs/AGENT_API.md`

## 10) Multi-Agent Work

Roles:
- **Owner:** decides behaviour, data and looks; picks the conductor; sets or ends policy overrides; approves visual defaults, cutovers, format changes and data deletions. Silence, elapsed time or a preselected option is never approval.
- **Conductor:** one session per repo at a time, chosen by the owner. Claude Opus 5.5 is the default; Codex conducts when the owner hands over (for example for computer use or work outside a sandbox). Plans, dispatches, decides non-owner questions with a stated default, commits for sandboxed writers, lands, pushes, deploys (section 4b) and keeps `docs/STATUS.md` current. Other sessions answer questions only.
- **Writers and reviewers:** Claude subagents (`opus`, `sonnet`) or Codex (`gpt-6-sol` for bounded work, `gpt-6-astra` for harder work). Claude Fable (`fable`) for high-risk review and large cross-cutting plans.
- Route by capability, not vendor. Visual or native-GPU work needs a harness that can run the GUI or Metal: Claude subagents run both; sandboxed Codex sees its own branch through the GUI host but cannot run Metal tests.

Per feature, 1 + 1:
- One writer and one reviewer from the other provider. The conductor is not counted. No second writer or extra reviewer on the same feature; a PASS with no findings is done.
- Fix rounds go back to the same writer thread. The same reviewer thread checks each fix and any integration delta that the standalone review did not cover.
- After two substantive fix rounds, stop and diagnose (scope, missing scenario, wrong contract) before another round.
- **High risk** (recorder, capture, roller, recording format, GPU heatmap core): the reviewer is the other provider's strongest model (Fable for Codex-written work; `gpt-6-astra` at high or above for Claude-written work). The hand-off must also carry scenario or native evidence: realistic outage, retry, restart and reconnect sequences for services; actual-Metal native runs and screenshots for GPU work. Review does not replace that evidence. A writer that cannot produce it (sandboxed Codex has no Metal) reports it as unverified; the conductor or a native-capable agent attaches it before landing.
- **Fallback** (the other provider is out of usage): a different model of the same provider reviews (Sol and Astra review each other; Opus or Sonnet and Fable review each other). The conductor notes the fallback in STATUS; it ends when the other provider is back. High-risk work waits for a cross-provider review unless the owner waives it.

Limits on the 16 GB Mac:
- At most two active writers across all features; one queued build/test at a time (`-j 2`); one hosted GUI session.
- Hot files (`MainWindowGpu.cpp`, `DataProcessor.cpp`, `HeatmapTwapStreamer.cpp`, `MarketDataCoreEngine.cpp`, `UnifiedGridRenderer.cpp`) belong to one branch at a time; serialize or split tasks that need the same one.
- At most 2-3 items wait on the owner at once, sent in one digest.
- Run `scripts/dev/budget.sh` (CodexBar) before a dispatch batch and route to the subscription with room. Tell the owner when Codex is near 0 (they hold reset credits). Exact token and agent status come from the owner's local `ma-panel` (`.claude/skills/ma-panel/`); agents build no other usage tracking.
- Conductor: report the plan, dispatches, phase changes, review findings and landings to the panel with `workflow_update` (intent, not numbers); STATUS stays the durable record.

Task packet (the dispatch prompt, self-contained): base commit; branch and worktree; files owned and files another agent is touching; interfaces; frozen UX spec for UI work; acceptance checks (each must fail without its fix); validation scope; risk class; for GUI runs, its own API port and scratch directory; the hand-off format. Point at this file and only the plan sections that apply. A delegated agent does not delegate further unless the packet allows it.

Hand-off protocol:
1. The conductor creates the worktree from current `main`: `scripts/dev/agent-worktree.sh create <branch>` (`lt-sol/...`, `lt-astra/...`, `lt-claude/...`).
2. The writer works on that base (no rebase, no merging `main`), builds and runs focused checks through the build queue.
3. The writer finishes with `READY: <branch>` and its tip (or "uncommitted diff" when sandboxed), what changed and why, the checks run with their summary lines, and what it could not verify. Last line: `WORKFLOW: <what cost time, surprised you, or the docs got wrong; or none>`. Use `BLOCKED: <branch>` with the reason if it cannot finish.
4. After review PASS (and owner approval where required), the conductor lands with `scripts/dev/agent-worktree.sh land <branch>`, stamping audit trailers with repeated `--trailer 'Key: value'`: `Slice`, `Plan`, `Risk`, `Writer`, `Reviewer`, `Review-Rounds`, `Findings: blocker=N major=N minor=N`, `Deferred`, `Writer-Tokens`, `Reviewer-Tokens`, `Planner-Tokens` (exact, from `ma-panel`), `Evidence`: rebase onto `main`, build, full ctest, `git range-diff` gate, `--no-ff` merge, worktree removal. One branch at a time; every other READY branch re-runs `land` before it lands. Bisect with `git bisect --first-parent`.

Codex CLI (hard rules; usage and quirks in `docs/AGENT_WORKFLOW.md`):
- Binary: `/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex` (`codex` is only a zsh alias).
- Lieutenants always run sandboxed: `-s workspace-write` (writers, with `--add-dir <repo>/_agent`) or `-s read-only` (reviews), always `-c approval_policy='"never"'`, stdin closed (`< /dev/null`). Never pass `--dangerously-bypass-approvals-and-sandbox`.
- `codex exec resume` keeps neither model nor sandbox: pass `-m` and the sandbox settings again.

Continuity:
- Start a fresh conductor at a completed wave boundary. It reads this file and `docs/STATUS.md`, then only the plan sections the next task needs.
- Keep unfinished writer and reviewer threads alive for fix rounds; record their thread IDs and tips in STATUS.
- STATUS holds current facts only and is replaced, not appended. History is `git log --first-parent` and `_agent/`.
- Handover: the outgoing conductor stops dispatching and brings STATUS up to date; the owner names the incoming conductor.
