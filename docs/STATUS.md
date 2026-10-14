# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends.

- **Updated:** 2026-10-14. main `787453b`. Conductor: Opus session of 2026-10-14 (memory and tracker cleanup wave).
- **Conductor:** a Claude Opus session on the owner's Mac (AGENTS.md section 10).
- **Review policy:** 1 + 1 cross-provider (AGENTS.md section 10); Fable for high-risk. Owner: use Codex down to ~1% before spending a reset credit (holds 3).

## Land gate

`agent-worktree.sh land` runs chart-health, the build, then `ctest -j 4` with `SENTINEL_REQUIRE_GPU=1` (an unavailable GPU is fatal). Resource locks, labels and `PROCESSORS` live in `tests/TestResources.cmake` (CMake >= 3.28). Every land attempt keeps `landing.json`, `build.log` and `ctest.log` in `~/Library/Logs/Sentinel/land/<attempt-id>/` (180 days / newest 300); the land prints the paths. Exit 4 = merged but evidence incomplete: worktree kept, finish with `agent-worktree.sh remove <branch>`, never re-land. `lizard` is at `~/.local/bin/lizard` (uv tool).

Pre-land evidence for chart slices: native `SENTINEL_REQUIRE_GPU=1` run of ChartGolden/ChartUi/UgrGpu (+ GuardMalloc teardowns for render-path slices); high-risk slices add isolated GUI-host screenshots main vs branch and 3 interleaved settled samples per side (only when the owner is away, input idle >= 10 min and screen unlocked, or on a typed OK).

## Services (owner's Mac, launchd)

- **Recorder/server:** slice E build `96e4f2f`, pid 37884, deployed 2026-10-12. **Do not roll back** with `deploy-runtime.sh rollback server`: older binaries do not match main's `config/server_config.yaml`; fix forward.
- **Capture** deployed 2026-10-03; 7 products; fan-out to the server. Rollback: `deploy-runtime.sh rollback capture`.
- **Monitoring:** VictoriaMetrics :8428, Grafana :3000 (A1b "live feed down" on `sentinel_live_feed_up`), shared on the tailnet only: `https://patricks-mac-mini.taildf7d06.ts.net:3000` (`tailscale serve`, anonymous Viewer; off: `tailscale serve --https=3000 off`). Public Funnel is off (owner 2026-10-13). GUI host :17190.
- **Shadow self-check (incremental, owner: keep it):** hourly checks roll only the new hour; a fresh midnight replay only for 00Z and after an oracle reset. Oracle at `hmc2/shadow-oracle/<product>/<dayMs>` (~19 MB/hour, ~450 MB at 23Z; current day only; exclude from backups). `Shadow oracle reset` in the server log means a reset happened.

## In flight

- **FAILURE_MODES pruning** (owner go 2026-10-14; was Next 0). Rules: entry <= ~300 chars; archive when code is gone or a test enforces the guardrail and the entry adds nothing; product bugs to TODO; target < 25 KB active. Keep test: would a future agent repeat the mistake before a test catches it. No tag format yet (owner is designing "LOD for docs" across threads). Done: FM-023, 036, 037, 050, 124, 129, 222 archived to `../Sentinel-Private/archive/agent/FAILURE_MODES.md` (269 active, 120 KB). Running: Fable writer classifies every entry into `../Sentinel-Private/research/2026-10-14-fm-pruning/` (proposal only); then gpt-6-astra reviews; then the conductor applies. Fable also adds an area column (input for the owner's LOD design, decision on hold). Next: INVARIANTS, then DECISIONS trimmed to decisions in force (owner 2026-10-14: nothing ancient).
- **doc-health gate** (owner go 2026-10-14): branch `lt-sol/doc-health`, worktree `/Volumes/T7/sentinel-worktrees/lt-sol-doc-health`, base `787453b`. Writer Codex gpt-6.1-sol (running); reviewer a Claude model after READY. Ratcheted size caps (STATUS 8 KB, TODO 12 KB, each _agent file 25 KB), 350-char limit on new entries, unique and never-reused ids, dead-pointer warnings; waiver trailer `Doc-Health-Waiver`.
- Audit reports from 2026-10-12 (tests, workflow) are in `../Sentinel-Private/research/2026-10-12-audits/`. A Windows Codex thread is pruning redundant tests and will open a PR; reconcile by gtest name.
- Workflow review with GPT-6 (owner relays): dossier `../Sentinel-Private/research/2026-10-13-agentops-dossier.md`; section 12 proposals, section 13 open owner questions. **Nothing in it is policy until the owner adopts it.** AgentOps S2-S4 need a new owner decision. S1 known gaps: SentinelBar does not see sandbox events until land; read-only reviewers write no events; no appender lock; Codex candidates are branch matches only.
- Neutrality evidence: main's startup `viewportVersion` and candle-history paging vary run to run; do not use them as neutrality pins; use the heatmap target hash plus pin tests. The `window` target is never byte-stable.
- Open check: at the next server restart, confirm the 7 startup `Shadow journal invalidated ... reason=journal boundary` warnings seen after the slice E deploy do not recur (likely the one-time v1-anchor fallback).
- Open FMs: FM-256 (TP/SL exit order left New), FM-259 (bubbles may not draw if the overlay attaches before trades), FM-260 (overlay dangling provider at teardown), FM-267 (startup auto tick 100 -> 500 -> 100 bounce: candle flicker; settled tick/price rows vary run to run on main; do not chase price-row diffs in screenshots; owner-visible), FM-268 (binner self-test zero-readback flake).
- Old sandbox events: the 96 id-less `build_failed` events from `/tmp/sentinel-events.jsonl` were moved (owner 2026-10-14, not migrated) to `/Volumes/T7/sentinel-backup/agent-events/2026-10-13-tmp-sandbox-events.jsonl`.

## Waiting on the owner

1. Decision explained, to revisit: whole-pixel rows and columns (DECISIONS 2026-10-08; landed as slices A, A2, A3, B).
2. Parked: nightly copy of `/Volumes/T7/sentinel-data` to the MacBook Pro over Tailscale (needs Remote Login + SSH key).

## Owner constraints in force

- The owner is hardening `scripts/dev/build-queue.sh` and its tests (FM-195). Agents do not change them or sweep the owner's changes into commits.
- Builds run at `-j 6` through the FIFO queue, one at a time. No automated GUI windows while the owner is at the Mac unless the owner asks. Hold queued builds while the owner is feel-testing a GUI.

## Next (priority order; pull the next item FIFO from `docs/TODO.md` "Queue")

1. Test cleanup from the Astra tests audit (see In flight).
2. The two deferred option B minors (`49b4c9a`; small, one Codex Sol branch): replace the source-grep test `UgrCandleCpu.WindowAndQmlPassUnroundedMilliseconds` with a fake `IGridDataSource`; remove the one startup refusal log line for the 100 ms default. Touches `tests/render/test_ugr_gpu.cpp`: coordinate with the owner's Windows test-cleanup PR.
3. B2 option A (sub-second candles) plan revision: `../Sentinel-Private/research/2026-10-13-b2-subsecond-candles.md` with the owner's answers; Astra REVISE r1 in `.claude/acting-orchestrator/b2-plan/astra-review-r1.md`; Fable revises in a new thread before any slice. Owner's call when.

Conductor notes from 2026-10-13 (owner discussion, not yet policy): fix rounds that only add a small test may go to a fresh short Codex thread instead of resuming a long one (resume re-reads the whole thread: 7-8M input tokens per round); `scripts/dev/chart-health.sh` should check uncommitted diffs so sandboxed writers stop building temporary committed copies; planners must cite file:line for every number or mark it unmeasured, and include a threads/locks/failure section.
