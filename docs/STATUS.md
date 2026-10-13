# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends.

- **Updated:** 2026-10-13 ~04:55 UTC. main `f8026ad`. Phase 5: B1, B3-B7 landed (`f8026ad`); only B2 is left (plan first, then owner sign-off). Chart-core refactor (S0a-S15) and one-world (through slice E) are complete. Focus now: test-suite cleanup and the workflow audit, then B1, B3-B7 (B2 waits for the owner's candle-interval spec).
- **Conductor:** a Claude Opus session on the owner's Mac (AGENTS.md section 10). The cloud Projects conductor + Mac operator trial (2026-10-11/12) is ended (owner, 2026-10-12).
- **Review policy:** 1 + 1 cross-provider (AGENTS.md section 10); Fable for high-risk. 2026-10-13 04:45Z: Claude weekly 100% (reset); Codex weekly 13% with a deficit. Owner: keep Codex writing down to ~1%, then the owner spends a reset credit (holds 3); Claude subagents may be used for other work. Landing trailers include `*-Tokens` from ma-panel again.

## Land gate

`agent-worktree.sh land` runs chart-health, the build, then `ctest -j 4` with `SENTINEL_REQUIRE_GPU=1` (an unavailable GPU is fatal). Resource locks, labels and `PROCESSORS` live in `tests/TestResources.cmake` (CMake >= 3.28; older CMake only loses them). No RUN_SERIAL list remains (FM-261 resolved `30c09cf`, FM-262 resolved `d87aa13`; RollerCorruptBlockTests TIMEOUT 300, `2cdb8ed`). The land removes the worktree and its `.land-ctest.log`: tee it during the land to quote the ChartGolden/UgrGpu/ChartUi lines. `lizard` is at `~/.local/bin/lizard` (uv tool).

## Chart-core refactor (complete; plan `docs/research/2026-10-11-chart-core-architecture.md`)

S0a `9e32fe4` through S15 `f5982b9`, one merge per slice (`git log --first-parent`); Fable reviewed S3, S10, S11, S12, S15. S11 carried the one owner-approved visible change (overlays always under chart text, DECISIONS 2026-10-12).

UGR after S15: `UnifiedGridRenderer.cpp` 382 lines (cap 450), `.h` 228 (cap 350), `m_` 14 (cap 15). QML reaches the chart through the `view`, `candles`, `tpo`, `debug` and `layers` groups; the Agent API viewport goes through `mainwindow/ChartAgentPort`. Every slice so far: Metal goldens byte-identical, no re-bless. Pre-land evidence for chart slices: native `SENTINEL_REQUIRE_GPU=1` run of ChartGolden/ChartUi/UgrGpu (+ GuardMalloc teardowns for render-path slices); high-risk slices add isolated GUI-host screenshots main vs branch and 3 interleaved settled samples per side (only when the owner is away, input idle >= 10 min and screen unlocked, or on a typed OK).

## One-world (complete through slice E `96e4f2f`)

Left: README/ARCHITECTURE pass (`data/market` deleted by the owner 2026-10-12).

## Services (owner's Mac, launchd)


- **Recorder/server:** slice E build `96e4f2f`, pid 37884, deployed 2026-10-12 03:45Z (anchor sidecar format v2). Smoke: 0 E/F lines; 14 W lines in the first 4 s: 7 expected `Roller anchor fallback ... section schema/version` (v1 sidecars ignored) and 7 `Shadow journal invalidated ... reason=journal boundary` (pid 45525 logged these only at 00Z, here at restart; conductor to judge). v2 anchors written within 20 s, anchor_failures_total 0 for all 7, live_feed_up 1 for all 7, no `sentinel_mdc_*`, shadow lag 0.47-1.04 s, `up` 1 for all 5 jobs. Fanout not checked (no client connected). **Do not roll back** with `deploy-runtime.sh rollback server`: older binaries do not match main's `config/server_config.yaml`; fix forward instead.
- **Capture** deployed 2026-10-03 22:13; 7 products; fan-out to the server. Rollback: `deploy-runtime.sh rollback capture`.
- **Monitoring:** VictoriaMetrics :8428, Grafana :3000 (restarted 2026-10-11 07:06Z; A1b is "A1b live feed down" on `sentinel_live_feed_up`, 7 series Normal). GUI host :17190.
- **Shadow self-check (incremental since bae9a85, owner: keep it):** each hourly check rolls only the new hour per product; a fresh midnight replay happens only for 00Z and after an oracle reset. Hours 2-24 resume from the oracle's own hourly anchor, so an anchor-resume defect would also show as a mismatch. The oracle lives at `hmc2/shadow-oracle/<product>/<dayMs>` (about 19 MB per hour, so about 450 MB at the 23Z peak; current day only; exclude it from backups). `Shadow oracle reset` in the server log means a reset happened.

## Hand-off: chart-core Phase 5 (B items), written 2026-10-13 by the outgoing conductor

Plan: `docs/research/2026-10-11-chart-core-architecture.md` rows B1-B8 (line ~771) and section 6 Q3 (line ~845). After PR 44 lands it moves to `../Sentinel-Private/research/` (same file name). SentinelBar shows 21/29; the true count is 21/28 (B8 was done inside S11; the bar only counts trailers).

State after `f8026ad` (2026-10-13):
- Landed in `f8026ad` (Codex Sol writer, Opus reviewer, 2 rounds, 1 minor fixed): B1 (seed/bootstrap use `liveEndCeil`, `nextBucketEnd` deleted), B3 (`autoPriceTick_` reset on controller destroy and `forgetService`), B4 (test rename), B5 (one auto fit per live step: `ViewCommands::refitAfterHeatmapLive` skips when follow already fitted; depends on the `liveChanged` connect order, pinned by `UgrZoomCpu.LiveChangedFitsOnceAndRefitsWithoutFollowShift`), B7 (glyph-atlas dump deleted). B6: nothing to remove (`AxisModel::setTarget` can detach the view state, so its checks are live). Metal goldens unchanged, no re-bless.
- B2 candle-interval rule: owner chose option A (2026-10-13): real sub-second candles (25-500 ms) built from trades, live and from the journal; tfMs in milliseconds end to end, nothing rounds it; seconds only at a boundary that truly speaks seconds, where an unrepresentable interval is refused with a logged reason, never rounded; ranges the journal does not cover show no candles (option B as fallback). Today three sites round differently (`PriceFitter.cpp:35,95,236`, `PriceCarryPolicy.cpp:49` `(tf+500)/1000`; `MainWindowGpu.cpp` history request `(tf+999)/1000`; `DepthChartView.qml:103` `Math.round`). Larger than the other B items and likely crosses the server candle path: first a short plan (where sub-second candles are built, history granularities the server accepts, cost), owner sign-off, then its own slice.
- B8: done in S11 (`d231a11`).

Next: the B2 plan (a Claude subagent may write it; owner allows Claude for non-writer work). Also hand over: PR 44 (docs to Sentinel-Private) review and land when the Windows Codex rework reports READY. Conflicts: the owner's Windows test-cleanup PR touches `tests/render/test_ugr_gpu.cpp` (B4); land it first or rebase B4 after. PR 44 (docs move to Sentinel-Private, rework requested) changes AGENTS.md and the plan path; it is docs-only and does not conflict with B code.

## In flight

- Read-only Codex Astra audits (2026-10-12, reports to the conductor, then a docs/research file): tests (re-check `docs/research/2026-10-11-code-health/audit-tests.md`, scan S0-S15 tests; thread `01a13321-9031-70b3-a3ef-7bcda74bed58`) and workflow (30-day trailers, review yield, conductor token cost, conductor hand-off rule; thread `01a13321-9031-7ce0-b709-54d79b8c710b`). A Windows Codex thread is also pruning redundant tests and will open a PR; reconcile by gtest name.
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

1. B2 option B (in flight or next): `tfMs` end to end in the GUI, nothing rounds; a timeframe that is not a whole second gets no history request (logged reason) and draws no candles; no retry loop. Owner 2026-10-13.
2. Test cleanup from the Astra audit.
3. B2 option A is a feature for a later date: revise `docs/research/2026-10-13-b2-subsecond-candles.md` with the Astra review (`.claude/acting-orchestrator/b2-plan/astra-review-r1.md`, REVISE 2/5/1) and the owner answers recorded in the plan, then schedule.
