# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends.

- **Updated:** 2026-10-13 ~23:30 UTC. main `8aacec9` (pushed). Since the last hand-off: Grafana ingest panels landed `19f7099`; PR 44 (docs index, plans and evidence moved to Sentinel-Private) landed `8aacec9`; PRs 43 and 44 closed on GitHub. This conductor ended past its 200k soft limit at the owner's request; nothing in flight.
- **Conductor:** a Claude Opus session on the owner's Mac (AGENTS.md section 10). The cloud Projects conductor + Mac operator trial (2026-10-11/12) is ended (owner, 2026-10-12).
- **Review policy:** 1 + 1 cross-provider (AGENTS.md section 10); Fable for high-risk. 2026-10-13: both weekly windows reset (Codex ~95%+, Claude ~100%). Owner: use Codex down to ~1% before spending a reset credit (holds 3).

## Land gate

`agent-worktree.sh land` runs chart-health, the build, then `ctest -j 4` with `SENTINEL_REQUIRE_GPU=1` (an unavailable GPU is fatal). Resource locks, labels and `PROCESSORS` live in `tests/TestResources.cmake` (CMake >= 3.28; older CMake only loses them). No RUN_SERIAL list remains (FM-261 resolved `30c09cf`, FM-262 resolved `d87aa13`; RollerCorruptBlockTests TIMEOUT 300, `2cdb8ed`). The land removes the worktree and its `.land-ctest.log`: tee it during the land to quote the ChartGolden/UgrGpu/ChartUi lines. `lizard` is at `~/.local/bin/lizard` (uv tool).

## Chart-core refactor (complete; plan `../Sentinel-Private/research/2026-10-11-chart-core-architecture.md`)

S0a `9e32fe4` through S15 `f5982b9`, one merge per slice (`git log --first-parent`); Fable reviewed S3, S10, S11, S12, S15. S11 carried the one owner-approved visible change (overlays always under chart text, DECISIONS 2026-10-12).

UGR after S15: `UnifiedGridRenderer.cpp` 382 lines (cap 450), `.h` 228 (cap 350), `m_` 14 (cap 15). QML reaches the chart through the `view`, `candles`, `tpo`, `debug` and `layers` groups; the Agent API viewport goes through `mainwindow/ChartAgentPort`. Every slice so far: Metal goldens byte-identical, no re-bless. Pre-land evidence for chart slices: native `SENTINEL_REQUIRE_GPU=1` run of ChartGolden/ChartUi/UgrGpu (+ GuardMalloc teardowns for render-path slices); high-risk slices add isolated GUI-host screenshots main vs branch and 3 interleaved settled samples per side (only when the owner is away, input idle >= 10 min and screen unlocked, or on a typed OK).

## One-world (complete through slice E `96e4f2f`)

Left: README/ARCHITECTURE pass (`data/market` deleted by the owner 2026-10-12).

## Services (owner's Mac, launchd)


- **Recorder/server:** slice E build `96e4f2f`, pid 37884, deployed 2026-10-12 03:45Z (anchor sidecar format v2). Smoke: 0 E/F lines; 14 W lines in the first 4 s: 7 expected `Roller anchor fallback ... section schema/version` (v1 sidecars ignored) and 7 `Shadow journal invalidated ... reason=journal boundary` (pid 45525 logged these only at 00Z, here at restart; conductor to judge). v2 anchors written within 20 s, anchor_failures_total 0 for all 7, live_feed_up 1 for all 7, no `sentinel_mdc_*`, shadow lag 0.47-1.04 s, `up` 1 for all 5 jobs. Fanout not checked (no client connected). **Do not roll back** with `deploy-runtime.sh rollback server`: older binaries do not match main's `config/server_config.yaml`; fix forward instead.
- **Capture** deployed 2026-10-03 22:13; 7 products; fan-out to the server. Rollback: `deploy-runtime.sh rollback capture`.
- **Monitoring:** VictoriaMetrics :8428, Grafana :3000 (A1b "live feed down" on `sentinel_live_feed_up`). Grafana is shared on the owner's tailnet only: `https://patricks-mac-mini.taildf7d06.ts.net:3000` (`tailscale serve`, anonymous Viewer; off: `tailscale serve --https=3000 off`). The public Funnel on :8443 was turned off 2026-10-13 (owner). The recorder dashboard has an "Ingest (capture)" row: queue bytes, pool %, reconnects/h, fan-out drops/overflow, client queue bytes. GUI host :17190.
- **Shadow self-check (incremental since bae9a85, owner: keep it):** each hourly check rolls only the new hour per product; a fresh midnight replay happens only for 00Z and after an oracle reset. Hours 2-24 resume from the oracle's own hourly anchor, so an anchor-resume defect would also show as a mismatch. The oracle lives at `hmc2/shadow-oracle/<product>/<dayMs>` (about 19 MB per hour, so about 450 MB at the 23Z peak; current day only; exclude it from backups). `Shadow oracle reset` in the server log means a reset happened.

## Hand-off (2026-10-13, outgoing conductor)

- Chart-core Phase 5 is done except B2 option A, which is a feature for later. Its plan is `../Sentinel-Private/research/2026-10-13-b2-subsecond-candles.md` with the owner's answers; Astra REVISE r1 is in `.claude/acting-orchestrator/b2-plan/astra-review-r1.md`. Fable revises in a new thread before any slice. Two minors from option B (`49b4c9a`) are deferred:
  - replace the source-grep test `UgrCandleCpu.WindowAndQmlPassUnroundedMilliseconds` with a fake `IGridDataSource`;
  - remove the one startup refusal log line for the 100 ms default.
- Workflow review with GPT-6 (owner relays messages; owner-initiated, read-only so far): the dossier is `../Sentinel-Private/research/2026-10-13-agentops-dossier.md` (Sentinel-Private `e2d6fe1`). Section 12 has proposals and section 13 open owner questions. **Nothing in it is policy until the owner adopts it.** Continue in a fresh conductor.
- Private plans repo: a sibling of the main checkout. From a worktree: `"$(git rev-parse --path-format=absolute --git-common-dir)/../../Sentinel-Private"` (AGENTS section 0).
- Deferred from PR 44 review: SentinelBar `PlanLocation.resolve` misses `initiatives/` trailers and relative pinned paths outside `research/` (minor).

## In flight

- Audit reports from 2026-10-12 (tests, workflow) are in `../Sentinel-Private/research/2026-10-12-audits/`. Nobody has acted on them yet. Test cleanup is Next 1. A Windows Codex thread is pruning redundant tests and will open a PR; reconcile by gtest name.
- Neutrality evidence: main's startup `viewportVersion` and candle-history paging (request count and limits) vary run to run (12/17/15 and 2-3 requests in three fresh-profile runs on 2026-10-12). Do not use them as neutrality pins; use the heatmap target hash plus pin tests. The `window` target is never byte-stable (live clock and status).
- Open check: at the next server restart, confirm the 7 startup `Shadow journal invalidated ... reason=journal boundary` warnings seen after the slice E deploy (pid 37884) do not recur (likely the one-time v1-anchor fallback).
- Open FMs: FM-256 (TP/SL exit order left New), FM-259 (bubbles may not draw if the overlay attaches before trades), FM-260 (overlay dangling provider at teardown), FM-267 (startup auto tick 100 -> 500 -> 100 bounce: candle flicker, and the settled tick/price rows vary run to run on main itself; do not chase price-row diffs in screenshots; fix after S15, owner-visible), FM-268 (binner self-test zero-readback flake).
- Test-hygiene leftover: ShadowTest.HandoffLatencyBenchmark opt-in (test_shadow.cpp; slice E has landed, so it is unblocked).

## Waiting on the owner

1. Parked: nightly copy of `/Volumes/T7/sentinel-data` to the MacBook Pro over Tailscale (needs Remote Login + SSH key).

## Owner constraints in force

- The owner is hardening `scripts/dev/build-queue.sh` and its tests (FM-195). Agents do not change them or
  sweep the owner's changes into commits.
- Builds run at `-j 6` through the FIFO queue, one at a time (owner 2026-10-08, AGENTS section 4). No automated GUI windows while the owner is at the Mac unless the owner asks. Hold queued builds while the owner is feel-testing a GUI (they compete for CPU).

## Next (priority order; pull the next item FIFO from `docs/TODO.md` "Queue")

0. Agent-memory pruning. The owner approved this on 2026-10-13. He is designing a long-term "LOD for docs" system with GPT-6 and may replace this item; ask first.
   - `_agent/` now lives in Sentinel-Private (`7092015`); FAILURE_MODES is 123 KB with 282 entries.
   - Approved rules:
     - one entry is at most ~300 characters;
     - archive an entry when its code is gone or a test or script enforces its guardrail, keeping the FM id and moving it to an archive file in Sentinel-Private;
     - product bugs go to the tracker, not `_agent/`;
     - give INVARIANTS and DECISIONS the same treatment.
   - Target: under 25 KB of active FMs.
   - Process: one Sonnet writer classifies every entry (keep, archive or tracker) and cites a file, test or commit for each; a Codex reviewer spot-checks the archive pile.
1. Test cleanup from the Astra tests audit (see In flight).
2. The two deferred option B minors (small, one Codex Sol branch; touches `tests/render/test_ugr_gpu.cpp`, so coordinate with the owner's Windows test-cleanup PR).
3. B2 option A plan revision (later date, owner's call when).

Conductor notes from 2026-10-13 (owner discussion, not yet policy): fix rounds that only add a small test may go to a fresh short Codex thread instead of resuming a long one (resume re-reads the whole thread: 7-8M input tokens per round here); `scripts/dev/chart-health.sh` should check uncommitted diffs so sandboxed writers stop building temporary committed copies; planners must cite file:line for every number or mark it unmeasured, and include a threads/locks/failure section (Fable's B2 plan assumed 8 history workers; there are 2).
