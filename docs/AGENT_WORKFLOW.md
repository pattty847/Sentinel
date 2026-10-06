# Agent Workflow Manual (read on demand)

Rules live in `AGENTS.md` (section 10 is the multi-agent protocol); live state lives in `docs/STATUS.md`.
This file holds the how-to detail that only some tasks need. It describes tools; it does not override
AGENTS.md. Remove a note here when it stops being true.

## The loop in practice

1. **Discuss.** The owner raises a goal or problem; the conductor investigates (or dispatches a read-only
   investigation) and returns the cause and options with a recommended default.
2. **Plan.** Anything cross-cutting gets a plan in `docs/research/` with the real decisions listed. The owner
   answers; the answers are appended as "Owner decisions (approved)". No dispatch before agreement on
   anything that changes behaviour, data or looks.
3. **Dispatch** one task packet per writer (AGENTS.md section 10).
4. **Review** the frozen tip (`git diff main...<tip>`). Findings ranked blocker/major/minor, each with a
   failure scenario. Fixes go to the same writer thread (Codex `exec resume`, Claude `SendMessage`); the same
   reviewer checks closure.
5. **Land** with `scripts/dev/agent-worktree.sh land <branch>`, one branch at a time.
6. **Deploy** services only through `scripts/dev/deploy-runtime.sh` (AGENTS.md section 4b), then watch the run
   log and metrics against the reviewer's watch list.
7. **Record.** Failure modes and invariants go to `_agent/`; current state replaces the old state in STATUS;
   the owner gets one short digest.

## Running and seeing the app

### GUI host (sandboxed agents and isolated runs)

- `scripts/dev/gui-host.py` runs OUTSIDE any sandbox on loopback (127.0.0.1:17190). The conductor starts it
  detached from the main checkout: `nohup scripts/dev/gui-host.py >/dev/null 2>&1 & disown`. Stop it on purpose
  with `pkill -TERM -f gui-host.py`. The host's 30-minute idle timeout does not bound a GUI orphaned by a
  SIGKILLed host: end that with `pkill -f 'sentinel-gui.*--agent-host'`. Status: `curl -s 127.0.0.1:17190/status`.
- Agents use `scripts/dev/gui-shot.sh`:
  - `launch [--renderer gpu|legacy] [--replace] [--build <worktree path>] [--fresh-profile]` starts the GUI on a
    spare API port with `--no-screener`.
  - `api GET|POST /api/v1/...` drives state, viewport and heatmap settings.
  - `shot <name> [--after <op>] [--settle]` returns the absolute PNG path to open.
  - `docks focus <id>` shows that dock and hides others. Hosted dock changes persist in
    `gui-host/profile/docks.ini`; `profile-reset` or `launch --fresh-profile` deletes only that file.
  - `stop` ends the session.
- By default the host runs main's build. `launch --build <worktree path>` runs only the fixed `sentinel-gui`
  binary under a direct child of `/Volumes/T7/sentinel-worktrees` or `<main>/.claude/worktrees`, after realpath
  containment and executable/`--agent-host` checks. Agents build their own branch through the build queue first;
  the host never builds or runs CMake. It never starts a server (the recorder must be up).
- Trust: the owner accepted on 2026-10-03 that unreviewed agent GUI builds run outside the sandbox through this
  host with the owner's privileges. Host-enforced boundary: path containment, fixed argv, allowlisted
  environment, one session, idle timeout. The in-GUI guardrails below live in the agent-compiled binary, so for
  `--build` runs they are not host guarantees, and an agent GUI still talks to the owner's recorder on :8080.
- `AgentHostMode`: the host passes `--agent-host <session dir>` and `--agent-host-symbols <list>` (default: seven
  recorded products, override with `GUI_HOST_SYMBOLS`). `--agent-host` without a symbol list means an empty
  allowlist: startup stays unsubscribed and every symbol-named request is refused. In this mode `target=main`,
  retired `lab`/`aiCommentary` and unknown targets are refused; screenshots and general QSettings stay in the
  fresh session dir; no trade command or algo start/stop is sent (choke point `RemoteGridDataSource`:
  `sendTradeCommand`, `sendAlgoCommand`; note `/api/v1/input` can reach the chart's TP/SL controls); no outbound
  request names a symbol outside the list (enforced at the `RemoteGridDataSource` send boundary, including
  startup and reconnect resubscribes; a source-scan test fails when a new `m_client.<sender>` is unguarded).
- If the host is not running, report the visual check as unverified and ask the conductor.

### Agent API, screenshots and input

- With a GUI running: `curl -s 'http://127.0.0.1:<port>/screenshot?name=<name>&target=window'` returns
  `{"ok":true,"path":"./screenshots/<name>.png"}` relative to the GUI's cwd. Routes: `docs/AGENT_API.md`.
- Drive the app through the Agent API: set symbol, timeframe, viewport and layers, wait for the change to render
  (`/api/v1/operations/<id>?waitMs=`), then screenshot that frame (`afterOperation=`).
- The standalone lab has its own `--screenshot`.
- Claude Code reads PNGs directly; Codex opens local images mid-task or takes them with `codex exec -i <png>`.
- The dev build is a raw binary with no app bundle; computer-use tools have not been verified to drive it. Ask
  the owner to pan, zoom or click, then read the run log and screenshot.

### Linux / cloud (no display)

- `scripts/dev/cloud-gui.sh start` runs Xvfb + server + GUI; `shot <name> window|heatmap|<dock ID>` saves (always pass a target: the script defaults to `main`, which AGENTS.md forbids)
  `screenshots/<name>.png`; `logs` shows W/E/F lines; `stop` tears down. Build with
  `cmake --build --preset linux-cloud` (deps from `scripts/setup/bootstrap-cloud.sh`, run by the SessionStart hook).
- Mesa llvmpipe: pixels are real, frame timings are not. GPU tests pick the QRhi backend at run time
  (`SENTINEL_RHI_BACKEND=d3d11|d3d12|vulkan|opengl|metal`) and print `GPU case skipped: <backend>: <reason>` when
  they cannot create a QRhi with compute. See `docs/WINDOWS_GPU_TESTS.md`.
- Live data needs `advanced-trade-ws.coinbase.com` and `api.coinbase.com` allowed; never put exchange API keys in
  a cloud environment.

### Data and performance

- Recorded book data (recording v2):
  `build/mac-clang/tests/servermodel/hmc2_dump <recording.dir> BTC-USD deep|near [tfMs] [lastN] [topK]`.
- Frame cost: `SENTINEL_FRAME_PROFILE=1` prints per-stage `updatePaintNode` timings once per second into the run log.
- CPU: `sample <pid> <seconds> -file <out>` (macOS) shows work outside `updatePaintNode` (texture uploads, QML,
  other threads).
- Services and metrics: `curl -s 127.0.0.1:8090/metrics | rg 'sentinel_roller_shadow|recorder_column_overdue'`,
  `curl -s 127.0.0.1:8091/metrics | rg capture_feed_up`.

## Services, deploys and pushes

- Recorder `com.sentinel.recorder` (`~/Sentinel-runtime/bin/sentinel-server --require-recording`, working directory =
  repo root) and capture `com.sentinel.capture` (7 products) run under launchd with KeepAlive. Both should show
  advancing `Recording v2 stats` / `Capture stats` lines in their run logs.
- After a deploy: `rg ' [WEF] '` the new run log, check `Recording v2 stats`, and dump the newest columns with
  `hmc2_dump`. Prefer deploying when the owner can respond: a macOS permission prompt once froze T7 I/O for 39 min
  before local signing existed (FM-127).
- Manual `launchctl kickstart -k gui/501/com.sentinel.recorder` or `launchctl bootout ...` only when the owner asks.
- Before pushing `main`, scan the outgoing range (`git log -p origin/main..main`) for keys, tokens, ntfy topics and
  PEM blocks.

## Build queue and worktrees

- `scripts/dev/build-queue.sh --label <branch> -- <command>` waits its turn, prints its place
  (`#2 of 3; running now: <label>`), runs and releases. `build-queue.sh status` shows the line. Tickets live in
  /tmp (writable from Codex sandboxes); a dead owner's ticket expires after 120 s.
- `scripts/dev/agent-worktree.sh create <branch>` puts the worktree on the T7 drive when mounted, supplies
  `VCPKG_ROOT` and ninja, and configures the build; ccache makes the first build fast. `land` uses the queue.
  `remove <branch>` cleans up.
- The conductor rebases agent branches (during `land`); nobody merges `main` into them (they are local and short-lived). Stack a dependent task
  and rebase with `--update-refs`. Merging `main` is right only for long-lived branches others have pulled.
- Sandboxed CMake regeneration can try to lock the shared vcpkg root even when dependencies are unchanged. After
  root has installed the manifest dependencies, reuse the worktree's installed tree with
  `VCPKG_MANIFEST_INSTALL=OFF` and a sandbox-writable ccache path. Dependency changes still need root to
  configure/install through the queue.

## Codex CLI

Verified 2026-09-27 with codex-cli 0.158; 0.160 was installed by 2026-10-05. Re-check
`~/.codex/models_cache.json` before relying on a model.

- Models that answered: `gpt-6-sol` (config default), `gpt-6-astra`. Also listed: `gpt-5.6-sol`, `gpt-5.6-terra`,
  `gpt-5.6-luna`, `gpt-5.5`. Effort `low`..`max`, plus `ultra` on 6-astra and 5.6-sol/terra.
- Run: `codex exec -C <dir> -s read-only|workspace-write -m gpt-6-sol -c model_reasoning_effort='"high"'
  -c approval_policy='"never"' --json -o <last-message.txt> "<prompt>" < /dev/null`. Without
  `approval_policy="never"` the owner's config (`on-request`, `approvals_reviewer="auto_review"`) lets a headless
  run ask to leave the sandbox and another model may approve.
- Sandbox: `workspace-write` has network (localhost, HTTPS, Coinbase WebSockets) and can read the whole home
  directory; it only limits writes. No window server or Metal. `.git` stays read-only even inside a writable
  root, so it cannot stage, commit or rebase in a linked worktree (`index.lock: Operation not permitted`);
  `--add-dir <repo>/_agent` does let it append to `_agent/`.
- Thread id: the first JSONL event is `{"type":"thread.started","thread_id":"<uuid>"}`. Continue with
  `codex exec resume <uuid> -m <same model> -c model_reasoning_effort='"high"' -c sandbox_mode='"workspace-write"'
  -c approval_policy='"never"' "<follow-up>"` (or `--last`); add
  `-c sandbox_workspace_write.writable_roots=[...]` when needed.
- `codex exec review` runs a code review of the current repo.
- A sub-agent spawned inside an interactive Codex session (multi-agent v2) cannot be resumed with
  `codex exec resume`: it fails with "cannot resume an unloaded multi-agent v2 sub-agent through its parent"
  (2026-10-06). For fix rounds a different conductor can reach, launch writers with `codex exec`, not as sub-agents.
- Never put backticks in a double-quoted prompt (the shell eats them); use a heredoc or a prompt file.
- Model-capacity errors can interrupt a run mid-draft: keep the draft and resume the same thread with an
  available model; keep a different-provider reviewer.

## Claude from a shell

- Read-only review: `cd <worktree> && claude -p --model fable "<review prompt: base/tip, spec, what to check,
  VERDICT format>"`. Verify the alias with a one-line call first.
- Cloud session from Bash (verified 2026-09-29): `claude --cloud` needs a TTY, so wrap it:
  `(script -q <log> claude --cloud "<prompt>" </dev/null >/dev/null 2>&1 &)`; it clones the pushed branch.
- A reviewer launched inside a worktree may not be able to read main-checkout docs under its permissions: put
  the exact document diff into the prompt rather than treating a code-only PASS as review of the docs.

## Lessons still in force

- Every GUI-running agent gets its own `--api-port` (17110 + n) and its own scratch directory; two agents once
  shared port 17110 and one drove the other's GUI (2026-10-02).
- A reviewer that runs a branch GUI without `--agent-host` uses the owner's QSettings (2026-10-03).
- Freeze UX specs before dispatching UI work; an unfrozen spec cost two review rounds and was replaced
  (2026-10-02), and an approved settings branch was stranded by a later toolbar change (2026-10-05).
- A standalone PASS does not approve a later integration patch; review the integration delta.
- Fix rounds can introduce new bugs: check the changed seam and nearby lifecycle, resize and refresh states.
- Static review and passing suites missed short-outage, socket-failure and watchdog behaviour in the recorder
  (slice C, FM-139), and native A/B runs found GPU span and live-source defects after review (FM-134/135). That
  is why high-risk work carries scenario/native evidence.
- A conductor session restart kills its background `codex exec` runs and its Claude subagents. Codex `exec`
  threads resume with `codex exec resume`; Claude subagents do not, so a new reviewer needs the previous
  findings in its prompt (2026-10-06).
- Offscreen sandbox tests can pass where native Cocoa fails: key propagation and focus inside menus differed
  (2026-10-06). UI work that touches focus or keyboard handling needs the native test run before landing.
- The build-queue wait/cancel path had a TERM-trap issue (FM-195); the owner is hardening the queue.
