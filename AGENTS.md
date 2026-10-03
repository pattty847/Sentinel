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
- Privacy: agents use `target=heatmap` (or the lab's own `--screenshot`) for screenshots. `target=main` can capture whatever window covers the GUI on the owner's screen, including other apps' private content; if a shot shows anything that is not Sentinel, delete it at once and tell the orchestrator.
- Viewing it: Claude Code reads the PNG directly; Codex opens local images mid-task on its own (verified 2026-09-27) or takes them up front with `codex exec -i <png>`. A sandboxed Codex run cannot launch the GUI itself (`Cannot create window: no screens available`), but it can ask the GUI host to (next bullet), so it takes its own screenshots.
- GUI host (sandboxed agents): `scripts/dev/gui-host.py` runs OUTSIDE any sandbox, loopback only (127.0.0.1:17190). The orchestrator starts it detached from the main checkout (`nohup scripts/dev/gui-host.py >/dev/null 2>&1 & disown`; nohup survives the session, so stop it on purpose with `pkill -TERM -f gui-host.py`; the 30-minute idle timeout is enforced by the host, so it does not bound a GUI orphaned by a SIGKILLed host: end it with `pkill -f 'sentinel-gui.*--agent-host'`). Agents use `scripts/dev/gui-shot.sh`: `launch [--renderer gpu|legacy] [--replace]` starts the GUI on a spare API port with `--no-screener`; `api GET|POST /api/v1/...` drives state, viewport and heatmap settings; `shot <name> [--after <op>] [--settle]` returns the absolute PNG path to open; `stop` ends it. `docks focus heatmap` hides other docks; hosted dock changes persist in `gui-host/profile/docks.ini` across runs, while general QSettings and screenshots stay fresh per session; `profile-reset` or `launch --fresh-profile` deletes only the dock file. TRUST: the host executes with the owner's privileges and an agent can write its own worktree build, so it runs ONLY the main checkout's build (the orchestrator builds landed main; rebuild main after a GUI change lands, or the host refuses a build without `--agent-host`). It never runs a path an agent names, never runs a worktree build, never builds (CMake runs code). So Codex sees landed work, not its own unlanded branch: branch visuals come from a Claude subagent (it runs its own GUI) or after landing. A per-branch "bless the worktree build" design was tried and dropped: a blessed copy is not bound to the reviewed source; the safe form (the orchestrator builds the reviewed commit in a checkout agents cannot write) is not built. The GUI runs with `--agent-host <session dir> --agent-host-symbols <list>` (`AgentHostMode`): screen grabs (`target=main`) and unknown targets are refused by the GUI itself; screenshots and general QSettings (INI) stay in the fresh session dir, while only dock visibility uses the persistent profile file (the owner's preferences are never touched); no trade command and no algo start/stop is sent (the choke point is `RemoteGridDataSource`: `sendTradeCommand`, `sendAlgoCommand`; `/api/v1/input` can reach the chart's TP/SL controls); no outbound request names a symbol outside `--agent-host-symbols` (default: the 7 recorded products): enforced at the `RemoteGridDataSource` send boundary, so startup (hard-coded BTC-USD, the server's default symbol) and every reconnect resubscribe are covered, and the GUI starts on the first allowed symbol or stays unsubscribed (a source-scan test fails when a new `m_client.<sender>` is unclassified or unguarded); the child gets a minimal environment. Do not add a native-format `QSettings("org","app")` to the GUI: use `QSettings(QSettings::defaultFormat(), QSettings::UserScope, "org", "app")` (a test enforces it). One session at a time; idle timeout 30 min. It never starts a server (the recorder must be up). If the host is not running, report the visual check as unverified and ask the orchestrator. Screenshots still fail with `grab_failed` while the Mac screen is locked. Note: the Agent API of the OWNER's own GUI (`gui.api_port`, 17100) has no authentication and a sandboxed agent can reach it over localhost; do not run agents with a live owner session unless that is intended.
- Input: the dev build is a raw binary with no app bundle, so computer-use tools cannot drive it. Ask the owner to pan, zoom or click, then read the run log and screenshot.
- Linux / Claude Code on the web (no display): `scripts/dev/cloud-gui.sh start` runs Xvfb + server + GUI, `scripts/dev/cloud-gui.sh shot <name> [main|heatmap|lab]` saves `screenshots/<name>.png`, `logs` shows W/E/F lines, `stop` tears down. Build with `cmake --build --preset linux-cloud` (deps from `scripts/setup/bootstrap-cloud.sh`, run by the SessionStart hook). Mesa llvmpipe: pixels are real, frame timings are not. GPU heatmap tests and the lab pick the QRhi backend at run time (`SENTINEL_RHI_BACKEND=d3d11|d3d12|vulkan|opengl|metal`; default opengl on Linux) and print `GPU case skipped: <backend>: <reason>` when it cannot create a QRhi with compute; a skipped case still reports Passed, so read the output. llvmpipe is not GPU verification. See `docs/WINDOWS_GPU_TESTS.md`. Live data needs `advanced-trade-ws.coinbase.com` and `api.coinbase.com` allowed in the environment's network settings; never put exchange API keys in a cloud environment.
- Always-on services (owner's Mac): the recorder (`com.sentinel.recorder`) and the raw capture (`com.sentinel.capture`) run under launchd from `~/Sentinel-runtime/bin`, signed with the local "Sentinel Local Code Signing" identity so Full Disk Access survives redeploys. Deploy ONLY with `scripts/dev/deploy-runtime.sh server|capture|both` (copy, sign, restart, verify writes within 60 s, auto-rollback). Agents never stop, restart or replace these services, and never run a bare `sentinel-server` from the repo (it contends for the recorder's data locks).
- Drive the app: the Agent API (`docs/AGENT_API.md`) sets symbol, timeframe, viewport and layers, waits for the change to render (`/api/v1/operations/<id>?waitMs=`), then screenshots that frame (`afterOperation=`). Screenshots fail with `grab_failed` while the Mac screen is locked (`ioreg -n Root -d1 -a | grep -A1 ScreenIsLocked`).
- Recorded book data (recording v2): `build/mac-clang/tests/servermodel/hmc2_dump <recording.dir> BTC-USD deep|near [tfMs] [lastN] [topK]` prints recorded columns and the biggest walls.
- Frame cost: `SENTINEL_FRAME_PROFILE=1` prints per-stage `updatePaintNode` timings once per second into the run log (section 4a).
- CPU: `sample <pid> <seconds> -file <out>` (macOS). Work that happens outside `updatePaintNode` (Qt texture uploads, QML, other threads) only shows up here.

## 4c) Metrics (after `ops/monitoring/install.sh` has run)

- Instant: `curl -s 127.0.0.1:8090/metrics` (sentinel-server). History: `curl -s 127.0.0.1:8428/api/v1/query --data-urlencode 'query=<promql>'` (VictoriaMetrics, 1 y). Dashboards: http://127.0.0.1:3000.
- Examples: `up`, `max by (product,layer) (sentinel_recorder_column_overdue_seconds)`, `increase(sentinel_recorder_invalidations_total[1h])`.
- Metric list, alerts, install and how to add a metric or panel: `ops/monitoring/README.md`. Hot paths only touch `Counter`/`Gauge` (one relaxed atomic); samplers run on the main thread at scrape.

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
- `_agent/` is shared by every worktree and parallel agent: re-read the file tail right before appending and take the next free id (`rg -o '^- FM-[0-9]+' _agent/FAILURE_MODES.md | sort -V | tail -1`)
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
The orchestration loop around these rules (roles, plan -> dispatch -> cross-vendor review -> land -> deploy) is described in `docs/AGENT_WORKFLOW.md`.

Routing (starting defaults; the orchestrator recalibrates them as results come in):
- **Orchestrator (Claude Code session the owner is talking to):** direction, cross-cutting design, audits, merges, anything touching hot paths or several subsystems at once.
- **Codex `gpt-6-sol`, effort high:** a well-specified bug fix or small feature with clear acceptance checks, in its own worktree.
- **Codex `gpt-6-astra`, effort high or above:** harder self-contained work: deeper reasoning, larger isolated refactors, second-opinion reviews (`-s read-only`).
- **Claude subagents (the orchestrator's Agent tool, model `opus` or `sonnet`):** the same kinds of tasks as the Codex lieutenants, used to spread usage across the owner's Claude and ChatGPT subscriptions. Write tasks follow the same hand-off protocol in their own worktree (branch `lt-claude/...`); reviews run read-only.
- **Claude Fable (Agent tool model `fable`):** reserved for work where a miss is expensive or judgment matters most: an extra final review for changes to the always-on recorder or the GPU heatmap core (alongside, not instead of, the cross-vendor review), plans for large cross-cutting slices (S6), visual A/B QA through the Agent API, and data/trade-off analysis. Not for mechanical slices or routine fix rounds.
- **UI / visual write tasks go to Claude subagents:** they can launch the GUI or lab and read their own screenshots; a sandboxed Codex run cannot (no window server, no Metal). Codex takes non-visual work and reviews.
- **Read-only review before merge:** a different model from the one that wrote the change; prefer the other vendor (Claude reviews Codex work, Codex reviews Claude work).
- A delegated agent does not delegate further unless its prompt explicitly allows it, and never merges its own branch.

The owner's ChatGPT subscription can run Codex agents headless, to spread work across subscriptions.
Verified 2026-09-27 with codex-cli 0.158.0-alpha.2.1.

- Binary: `/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex`. `codex` is only a zsh alias; use the full path in scripts.
- Models that answered a live `codex exec` call: `gpt-6-sol` (config default) and `gpt-6-astra`. Also listed for this account: `gpt-5.6-sol`, `gpt-5.6-terra`, `gpt-5.6-luna`, `gpt-5.5`. Reasoning effort: `low`..`max`, plus `ultra` on 6-astra and 5.6-sol/terra. Re-check `~/.codex/models_cache.json` before relying on a model.
- Run: `codex exec -C <dir> -s read-only|workspace-write -m gpt-6-sol -c model_reasoning_effort='"high"' -c approval_policy='"never"' --json -o <last-message.txt> "<prompt>"`. Always pass `approval_policy="never"` for lieutenants: the owner's config uses `on-request` with `approvals_reviewer="auto_review"`, so without it a headless run can ask to leave the sandbox and another model may approve.
- Sandbox (verified 2026-09-29): `workspace-write` has network (the config sets `network_access = true`; localhost, HTTPS and Coinbase WebSockets all worked) and can read the whole home directory; it only limits writes. It cannot use the window server or Metal, so no GUI or GPU runs.
- Thread id: the first JSONL event is `{"type":"thread.started","thread_id":"<uuid>"}`. Continue with `codex exec resume <uuid> -m <same model> -c model_reasoning_effort='"high"' -c sandbox_mode='"workspace-write"' -c approval_policy='"never"' "<follow-up>"` (or `--last`). Resume does not keep the original model or sandbox: without `-m` it falls back to the config default model, and it has no `-s` flag.
- Reviews: `codex exec review` runs a code review of the current repo.
- Worktree write tasks: pass `--add-dir <repo>/_agent` and close stdin (`< /dev/null`). A linked worktree keeps its index and refs in the main `.git`; Codex's sandbox protects `.git` paths even inside a writable root, so with or without the flag it cannot stage, commit or rebase (`index.lock: Operation not permitted`, re-verified 2026-10-02): the orchestrator commits and rebases for Codex lieutenants. `--add-dir <repo>/_agent` does work (verified 2026-10-02), so Codex can append to `_agent/` itself. A backgrounded `codex exec` with stdin open can wait forever for input.
- Isolation: write tasks run in their own git worktree (`--worktree`, or `-C` into a `git worktree add` path) so two agents never edit the same checkout. Audits and reviews use `-s read-only`.
- Never pass `--dangerously-bypass-approvals-and-sandbox`.
- Prompts must stand alone: point the agent at `AGENTS.md`, the files, the acceptance checks, and the build/test commands.
- The delegating agent reviews the resulting diff and runs the verification ladder (section 4) before anything merges.

Hand-off protocol (every delegated write task):
1. Work in your own worktree and branch, created from current `main` with `scripts/dev/agent-worktree.sh create <branch>` (branches: `lt-sol/...` for Codex gpt-6-sol, `lt-astra/...` for gpt-6-astra, `lt-claude/...` for Claude subagents). It puts the worktree on the T7 drive when mounted, supplies `VCPKG_ROOT` and ninja, and configures the build; ccache makes the first build take seconds. Remove it after merge with `... remove <branch>`.
2. Before reporting ready, rebase onto the latest `main` (`git rebase main`; rerere is enabled for the repo, so a conflict you resolve once is reused), resolve any conflicts yourself, rebuild, and run `ctest` in `build/mac-clang`. Do not merge `main` into agent branches: they are local, single-owner and short-lived, so a rebase keeps history linear. Do not track `main` while you work; rebase once, at hand-off.
3. Finish with a message whose first line is `READY: <branch>`, followed by: what changed and why, the tests you ran with their summary line, and anything you could not verify (visual checks, live runs). End with one line `WORKFLOW: <what cost time, surprised you, or the docs got wrong; or none>`. Say `BLOCKED: <branch>` with the reason instead if you cannot finish.
4. The orchestrator reviews the diff with a different model, then lands it with `scripts/dev/agent-worktree.sh land <branch>`: rebase onto the current `main`, build, ctest, a `git range-diff` review gate when the rebase changed commits, a `--no-ff` merge, and worktree removal. Never merge your own branch. Only the orchestrator pushes `main` (after clean landings and a secret scan, never forced); lieutenants never push.
5. Merge queue rule: land one branch at a time. After each landing, every other READY branch is rebased and retested (run `land` on it) before it can land, even when it touched different files; that is what catches semantic conflicts (a renamed function, a duplicated test name). Bisect with `git bisect --first-parent` so it walks tested landings, not untested commits inside rebased branches.
6. Machine budget: the owner's Mac has 16 GB of unified memory. Every build, ctest run or benchmark goes through the FIFO queue: `scripts/dev/build-queue.sh --label <branch> -- <command>` (e.g. `-- cmake --build --preset mac-clang -j 4`). It waits for your turn, prints your place in line (`#2 of 3; running now: <label>`), runs, and releases; `build-queue.sh status` shows the line; tickets live in /tmp (writable from Codex sandboxes) and a dead owner's ticket expires after 120 s. `agent-worktree.sh land` uses it too. Read-only reviews and planning need no ticket.
7. Parallel dispatch: before sending two tasks out at once, check they do not both need the same hot file (`MainWindowGpu.cpp`, `DataProcessor.cpp`, `HeatmapTwapStreamer.cpp`); if they do, serialize them or draw the boundary in both prompts. A task that depends on another is dispatched after the first lands (or stacked, then rebased with `--update-refs`). Merging `main` into a branch instead of rebasing is still right for long-lived or shared branches that others have pulled.
