# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends. Keep under 8 KB.

- **Updated:** 2026-11-09 ~12:30 UTC. Conductor hand-off point: this conductor's context is long; next conductor starts here. Nothing running (no writer, reviewer, build or land). main `698c2c1c` pushed. **main** pushed (see `git log -1`). Work order: `docs/TODO.md` Waves.
- **Conductor:** Claude Opus 5.5, wave 3 (started 2026-11-08).
- **Review policy:** 1 + 1 cross-provider (AGENTS.md section 10); Fable for high risk. Owner 2026-11-05: spend all Codex, including reset credits before they expire.

## Next

0. **Next action for the incoming conductor:** (a) S8a: owner APPROVED the look 2026-11-09. Conductor rebased onto `535ccd24` and re-blessed `10.trade-bubbles` (tip `726d84ef`). Land gate failed: the bubble golden depends on wall-clock throttles under full-ctest load (stable alone 3/3). Determinism fix sent to the writer (packet `astra-s8a-fix4.txt`); when READY: Opus checks the delta, conductor reruns the golden alone and under load, re-blesses once if the settled hash differs, then lands. Same land also failed CaptureFanoutTests `MalformedIngressIsolatesOnlyItsProduct` (passes 3/3 alone; load flake unrelated to S8a, file a TODO backlog item if it recurs). (b) Dispatch S6 footprint-cells-layer and S7 cvd-layer (plan section 7; slot order S6|S7; both GUI chart layers, AGENTS 3a; owner decision 3 = visual pass with the owner in the lab before goldens, so ship looks behind `layers.*` knobs and do not re-bless). Then S5 (delete the old footprint path; touches `MainWindowGpu.cpp`, `DataProcessor.cpp`), S8b, S9, S10, S11. (c) Deploy (item 2) only with the owner at the Mac; afterwards measure server RSS before/after the flow store's first pass and read the run log for `Flow store` lines. Packets of this wave are in the outgoing conductor's scratchpad `/private/tmp/claude-501/-Users-copeharder-Programming-Sentinel/2c2daa86-3835-4ef4-bfd7-01c5f5fdb82d/scratchpad/` (astra-*.txt).
1. Wave 3 plan: `../Sentinel-Private/research/2026-11-08-why-it-moved-plan.md` (12 slices, section 7 order). Landed: S4 flow-client `89656fcb` (GUI decode/buffer/paging, agent-host guarded; deferred: a refused flow_live_subscribe still marks the live edge covered), S3b flow-history `a60df021` (store bars final per fully sealed bucket; `source` store|live; `sealed_through_ms` = store built-through from the live retention floor; 1 s replay one at a time, cancellable), heatmap saturation + per-asset liquidity range + per-asset manual tick `8afd4412` (owner to rebuild the GUI and check BTC/ETH/SOL/DOGE/PEPE/FARTCOIN; deferred minors: pending-range slider handles unscaled for non-BTC octaves, setTickMemory clears a held fallback, QString alloc per viewport change, per-switch warning timers; fit includes dust cells for the aesthetic pass), S2 flow-store `67d3644a` (deferred: stop() bounded by one uncancelled JournalReader header walk, ~0.3-3 s cold, hourly; after deploy measure server RSS before/after the first pass for catalogs_), S3a flow-live-model `9beb557c` (deferred: loss flag from 1m only; S4 needs a re-fetch rule when `complete` turns false without a revision bump; sweep test name), S1 flow-bar-core `131596b4`, heatmap seam `da486efd`, axis drag `b77e7fb9`. Conductor decision: `revision` is not in FLB1 content; the wire envelope carries `revisionAt`. S8a is unblocked (heatmap-saturation landed). Owner decisions (plan section 6): 1 flow store = derived store like rollups (owner APPROVED 2026-11-08; normal deploy; measured 0.51 GB per BTC-year), landed; 2 bubble defaults: owner judges both modes in the lab (S8a ships both behind a setting); 3 footprint/CVD look: visual pass with the owner in the lab before goldens. Palette waits for the owner's colour variants. Owner to check the seam fix live: 1h, zoom near the now column.
2. With the owner present: build main, then `scripts/dev/deploy-runtime.sh server` for the trade-id window `87c9ccbc`, S3a live flow `9beb557c` S2 flow store `67d3644a` and S3b flow history `a60df021` (new derived dir `recording.flow_dir`, default `<roller_shadow.dir>-flow`; no RAWL2/HMC2 change). The script refuses a stale or dirty build.
3. After landing any GUI change, build main: the owner's GUI loads QML from the source tree, so a stale binary fails to load the chart.
4. Squash any branch whose history touched `docs/TODO.md` before land (AGENTS section 7).

## Services (owner's Mac, launchd)

- **Server** (`com.sentinel.recorder`): pid 97899, built from `037021cd`, deployed 2026-11-08 18:03 EST. Not yet deployed: `87c9ccbc` trade-id window. Startup replays the journal from UTC midnight minus margin to rebuild open 1m..1d bars (BTC ~21-27 s; that product's live feed waits).
- **Capture** (`com.sentinel.capture`): binary from 2026-10-03; 7 products; fan-out to the server. Rollback: `deploy-runtime.sh rollback capture`.
- **Monitoring:** VictoriaMetrics :8428, Grafana :3000 (alerts include live feed down, roller retry exhausted), tailnet only via `tailscale serve`; public Funnel off. Grafana loads alert rules only on restart (`launchctl kickstart -k gui/$(id -u)/com.sentinel.grafana`).
- **Shadow self-check** (owner: keep it): hourly checks roll only the new hour from the oracle's anchor; oracle at `hmc2/shadow-oracle/<product>/<dayMs>` (current day only; exclude from backups).
- Startup `Shadow journal invalidated ... reason=journal boundary` warnings at server start are routine.

## Land gate

`agent-worktree.sh land` checks out a private verified clone on T7, rebases, runs chart-health, doc-health, the build and full `ctest` with `SENTINEL_REQUIRE_GPU=1`, then merges `--no-ff` the tested SHA. Evidence per attempt in `~/Library/Logs/Sentinel/land/<attempt-id>/`. Exit 4 = merged but evidence incomplete: finish with `agent-worktree.sh remove <branch>`, never re-land. Chart slices: native `SENTINEL_REQUIRE_GPU=1` ChartGolden/ChartUi/UgrGpu before READY.

## In flight



- `lt-astra/bubble-placement` (worktree `/Volumes/T7/sentinel-worktrees/lt-astra-bubble-placement`): plan S8a, from `7d209c3e`. READY `77609f54` (golden 10.trade-bubbles will change: adaptive sizing + median filter also in the default individual mode; owner sees it in the lab before any re-bless). Writer gpt-6-astra thread `01a1bfd4-223a-7362-8b9d-e1d21cdca8c0`; Native: only golden 10.trade-bubbles differs (expected). Opus review FAIL (slot marks all at the cap; per-frame sort on the render thread; size/filter flicker; sweep capsules vanish zoomed in; deleted regression tests). Fix round 1 `e48c40e9`: Opus re-review FAIL (pan still recomputes per frame; slot notables at the cap). Fix round 2 `24011254`: Opus PASS (pan 4 recomputes/s; notables below aggregates). Follow-up `2d71c3e8` (slot reference on first entry) native 8/9, only `10.trade-bubbles` differs. WAITING on the owner's OK to re-bless and land (image sent; modes compared in the owner's GUI after landing). Packet `astra-s8a.txt`: setting `bubblePlacement` individual|auto|slot (default individual), sweep capsules, auto size reference, tradeMinNotional auto. Reviewer Claude Opus. MEDIUM.


Lab branches that never land: `lt-astra/hmc2-rollup-lab`, `lt-claude/hmc2-layout-lab` (results in `/Volumes/T7/sentinel-datalab/hmc2-*`).

## Waiting on the owner

1. **v3.0.0-alpha macOS release:** branch `lt-claude/mac-release-v3` tip `5082065a`, review PASS (gpt-6-astra thread `01a16c06-c945-7260-89c1-cafa71c6f81b`); waits on the owner's second-account test of the zip in `/Users/Shared/Sentinel-alpha-test/` (`TEST-STEPS.md`). Then land, release notes, tag only on the owner's yes.
2. Revoke the old Coinbase key; then the conductor deletes `key.json` at the next restart. Decide the config overlay question (Sentinel-Private security notes).

## Owner constraints in force

- Builds and tests run `-j 6` through the FIFO build queue, one at a time. No automated GUI windows while the owner works at the Mac unless asked.
- The owner hardens `scripts/dev/build-queue.sh` and its tests (FM-195); agents do not change them.
- Docs archive on the T7: `/Volumes/T7/sentinel-backup/docs-archive/`.
