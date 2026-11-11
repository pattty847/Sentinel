# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends. Keep under 8 KB.

- **Updated:** 2026-11-11 ~04:45 UTC. main `9e32edba` (pushed with this commit). Conductor hand-off at a clean boundary: no writer in flight. Work order: `docs/TODO.md` Waves.
- **Conductor:** ended 2026-11-11 ~04:45 UTC (context far past the hard limit; owner kept it until all three workstreams finished). Next conductor: owner names it.
- **Review policy:** 1 + 1 cross-provider (AGENTS.md section 10); Fable for high risk. Owner 2026-11-05: spend all Codex, including reset credits before they expire.

## Next

0. **Hand-off.** Landed this conductor: S9 trade history `0a7f5aa6` + server deploy; square spine ends `a2066fc0`; footprint every-zoom `5587cb81`; 2 logical px zoom-out rule `cb4a178c`; footprint aggressor colours `6b5c485d`; paper position fix `7db093a7` + server deploy (pid 53751, 2026-11-11 04:00 UTC, smoke clean; owner verified the stuck position cleared after a GUI restart); footprint cells only (q16 GUI path deleted) `9e32edba`. **Next, in order:** (a) **repo split**: plan approved with conductor changes + owner decisions in `../Sentinel-Private/research/2026-11-11-repo-split-plan.md` section 8 (D1/D2/D3/D4 private); planner session `local_9196557b-bb63-49b6-be30-fa61a90b3024`; run only while no writer is in flight; one writer + Codex review per step. (b) **q16 slice 2** (server publisher, footprint wire messages, `footprintTimeframeMs` config, client parse-then-drop branch; list in the cells-only hand-off, `9e32edba` merge message) + server deploy with the owner. (c) **paper slice** (owner 2026-11-11): algo Stop cancels the algo's limits, then prompts "Flatten all?" Yes/No with an "On algo stop" setting (Ask default / Always / Never); GUI must apply server algo-stop cancellations (ghost quote lines, algo panel stuck UNCONFIRMED; manual Cancel/Flatten already work); trading snapshot on reconnect (stale position after a server restart). Per-algo position attribution waits for a paper-backend rework. (d) **TPO style-model slice** from `/Volumes/T7/sentinel-audit/ui-2026-11/labs/L-TPO-decisions.md`; Flow Lab picks B1 bubbles, C1 fat candles (85%, max 71 px), U1 tooltip 360 px in `L-FLOW-decisions.md`. Labs: TPO https://claude.ai/artifact/1KKy4dFm5t6x5E1n6i44X6, Flow https://claude.ai/artifact/BsudxbhrvvoVTAmZuV6uh9 (picks in each page's `picks` db). Owner video assets: `/Volumes/T7/sentinel-media/raw/footprint-colours/{before,after}` (GUI-host shots, same viewports). Owner rules: one job at a time while the owner uses the Mac; check `uptime`; -j 4 when active.
1. Wave 3 plan: `../Sentinel-Private/research/2026-11-08-why-it-moved-plan.md` (section 7 order). Landed slices and their deferred items are in `git log --first-parent` trailers (`Deferred:`). Owner decisions there: flow store approved; bubble and footprint/CVD looks judged in the lab (now done via the labs above).
2. Deployed 2026-11-10 00:10 UTC (see Services). Startup logs 4 `Flow store lag exceeds live retention` warnings with lagMs = epoch (store not built, no live trade yet): misleading wording, TODO.
3. After landing any GUI change, build main: the owner's GUI loads QML from the source tree, so a stale binary fails to load the chart.
4. Squash any branch whose history touched `docs/TODO.md` before land (AGENTS section 7).

## Services (owner's Mac, launchd)

- **Server** (`com.sentinel.recorder`): pid 53751, built from `7db093a7` (paper fix + trade-command logging), deployed 2026-11-11 04:00 UTC with the owner; smoke clean (RSS 56-217 MB, overdue 0). Earlier transient 1-1.7 GB RSS peaks: profile if they recur. Not yet deployed: nothing.
- **Capture** (`com.sentinel.capture`): binary from 2026-10-03; 7 products; fan-out to the server. Rollback: `deploy-runtime.sh rollback capture`.
- **Monitoring:** VictoriaMetrics :8428, Grafana :3000 (alerts include live feed down, roller retry exhausted), tailnet only via `tailscale serve`; public Funnel off. Grafana loads alert rules only on restart (`launchctl kickstart -k gui/$(id -u)/com.sentinel.grafana`).
- **Shadow self-check** (owner: keep it): hourly checks roll only the new hour from the oracle's anchor; oracle at `hmc2/shadow-oracle/<product>/<dayMs>` (current day only; exclude from backups).
- Startup `Shadow journal invalidated ... reason=journal boundary` warnings at server start are routine.

## Land gate

`agent-worktree.sh land` checks out a private verified clone on T7, rebases, runs chart-health, doc-health, the build and full `ctest` with `SENTINEL_REQUIRE_GPU=1`, then merges `--no-ff` the tested SHA. Evidence per attempt in `~/Library/Logs/Sentinel/land/<attempt-id>/`. Exit 4 = merged but evidence incomplete: finish with `agent-worktree.sh remove <branch>`, never re-land. Chart slices: native `SENTINEL_REQUIRE_GPU=1` ChartGolden/ChartUi/UgrGpu before READY.

## In flight

- None. Linked app sessions (notes reached the old conductor only; message by id): repo-split planner `local_9196557b-bb63-49b6-be30-fa61a90b3024` (plan done), paper-close `local_bcd47e33-f034-4a41-8375-bef0fa81e972` (landed), UI audit `local_a0b0cb41-c6cc-4e10-bd7b-a7f748a42eb6` (done; brief APPROVED). Codex threads (resumable): Astra footprint `01a1c76a-15d7-78e1-a76b-9415630a9a48`, Astra paper `01a1c8a9-6679-7283-bde0-716b73036839`, Sol cells-only `01a1c8b0-3621-7c91-9d9b-443e64a3f22a`.

Lab branches that never land: `lt-astra/hmc2-rollup-lab`, `lt-claude/hmc2-layout-lab` (results in `/Volumes/T7/sentinel-datalab/hmc2-*`).

## Waiting on the owner

1. **v3.0.0-alpha macOS release:** branch `lt-claude/mac-release-v3` tip `5082065a`, review PASS (gpt-6-astra thread `01a16c06-c945-7260-89c1-cafa71c6f81b`); waits on the owner's second-account test of the zip in `/Users/Shared/Sentinel-alpha-test/` (`TEST-STEPS.md`). Then land, release notes, tag only on the owner's yes.
2. Revoke the old Coinbase key; then the conductor deletes `key.json` at the next restart. Decide the config overlay question (Sentinel-Private security notes).

## Owner constraints in force

- Builds and tests run `-j 6` through the FIFO build queue, one at a time. No automated GUI windows while the owner works at the Mac unless asked.
- The owner hardens `scripts/dev/build-queue.sh` and its tests (FM-195); agents do not change them.
- Docs archive on the T7: `/Volumes/T7/sentinel-backup/docs-archive/`.
