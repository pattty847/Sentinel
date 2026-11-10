# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends. Keep under 8 KB.

- **Updated:** 2026-11-09 ~23:55 UTC. main `fc00db5a`+ (pushed). Nothing running. Work order: `docs/TODO.md` Waves.
- **Conductor:** Claude Opus 5.5, wave 3 (started 2026-11-08).
- **Review policy:** 1 + 1 cross-provider (AGENTS.md section 10); Fable for high risk. Owner 2026-11-05: spend all Codex, including reset credits before they expire.

## Next

0. **Now (conductor 2026-11-09 ~23:55 UTC):** Landed today: S8a bubbles `15037e99`, wall radar zoom `c57fa211`, S6 footprint cells `6f19eccf` (`layers.footprintSource` q16 default | cells), wall badges size-only `f1d26f24`, S7 CVD `fc00db5a` (`layers.cvdStripPx` default 0; `cvdReset` visible default | utcDay | tpoSession; `cvdStyle` line | area). No writers in flight. Server deployed 00:10 UTC; conductor tracks RSS until the flow store's first pass ends. Next: the owner's lab session for footprint cells + CVD (both need flow bars from the deployed server; lab checks: hollow candles may flip each minute in follow-live; pick looks, then goldens). Then S5 (only after the owner picks cells over q16), S8b, S9, S10, S11. Packets: `/private/tmp/claude-501/-Users-copeharder--claude-spares-spare-gNlv6b/a05a9ad7-d309-4157-a239-a22a0e973070/scratchpad/`.
1. Wave 3 plan: `../Sentinel-Private/research/2026-11-08-why-it-moved-plan.md` (12 slices, section 7 order). Landed: S4 flow-client `89656fcb` (GUI decode/buffer/paging, agent-host guarded; deferred: a refused flow_live_subscribe still marks the live edge covered), S3b flow-history `a60df021` (store bars final per fully sealed bucket; `source` store|live; `sealed_through_ms` = store built-through from the live retention floor; 1 s replay one at a time, cancellable), heatmap saturation + per-asset liquidity range + per-asset manual tick `8afd4412` (owner checked all assets live 2026-11-09: all good; deferred minors: pending-range slider handles unscaled for non-BTC octaves, setTickMemory clears a held fallback, QString alloc per viewport change, per-switch warning timers; fit includes dust cells for the aesthetic pass), S2 flow-store `67d3644a` (deferred: stop() bounded by one uncancelled JournalReader header walk, ~0.3-3 s cold, hourly; after deploy measure server RSS before/after the first pass for catalogs_), S3a flow-live-model `9beb557c` (deferred: loss flag from 1m only; S4 needs a re-fetch rule when `complete` turns false without a revision bump; sweep test name), S1 flow-bar-core `131596b4`, heatmap seam `da486efd`, axis drag `b77e7fb9`. Conductor decision: `revision` is not in FLB1 content; the wire envelope carries `revisionAt`. S8a is unblocked (heatmap-saturation landed). Owner decisions (plan section 6): 1 flow store = derived store like rollups (owner APPROVED 2026-11-08; normal deploy; measured 0.51 GB per BTC-year), landed; 2 bubble defaults: owner judges both modes in the lab (S8a ships both behind a setting); 3 footprint/CVD look: visual pass with the owner in the lab before goldens. Palette waits for the owner's colour variants. Owner to check the seam fix live: 1h, zoom near the now column.
2. Deployed 2026-11-10 00:10 UTC (see Services). Startup logs 4 `Flow store lag exceeds live retention` warnings with lagMs = epoch (store not built, no live trade yet): misleading wording, TODO.
3. After landing any GUI change, build main: the owner's GUI loads QML from the source tree, so a stale binary fails to load the chart.
4. Squash any branch whose history touched `docs/TODO.md` before land (AGENTS section 7).

## Services (owner's Mac, launchd)

- **Server** (`com.sentinel.recorder`): pid 46971, built from `861f442d`, deployed 2026-11-10 00:10 UTC with the owner present (trade-id window, S3a live flow, S2 flow store, S3b flow history). Flow store root `/Volumes/T7/sentinel-data/hmc2-flow`; first pass building (~1.1 MB/min); RSS 64 MB before, 76-155 MB sawtooth during the pass, no growth trend in the first 10 min. Startup replays the journal from UTC midnight minus margin to rebuild open 1m..1d bars (BTC ~21-27 s; that product's live feed waits).
- **Capture** (`com.sentinel.capture`): binary from 2026-10-03; 7 products; fan-out to the server. Rollback: `deploy-runtime.sh rollback capture`.
- **Monitoring:** VictoriaMetrics :8428, Grafana :3000 (alerts include live feed down, roller retry exhausted), tailnet only via `tailscale serve`; public Funnel off. Grafana loads alert rules only on restart (`launchctl kickstart -k gui/$(id -u)/com.sentinel.grafana`).
- **Shadow self-check** (owner: keep it): hourly checks roll only the new hour from the oracle's anchor; oracle at `hmc2/shadow-oracle/<product>/<dayMs>` (current day only; exclude from backups).
- Startup `Shadow journal invalidated ... reason=journal boundary` warnings at server start are routine.

## Land gate

`agent-worktree.sh land` checks out a private verified clone on T7, rebases, runs chart-health, doc-health, the build and full `ctest` with `SENTINEL_REQUIRE_GPU=1`, then merges `--no-ff` the tested SHA. Evidence per attempt in `~/Library/Logs/Sentinel/land/<attempt-id>/`. Exit 4 = merged but evidence incomplete: finish with `agent-worktree.sh remove <branch>`, never re-land. Chart slices: native `SENTINEL_REQUIRE_GPU=1` ChartGolden/ChartUi/UgrGpu before READY.

## In flight

None.

Lab branches that never land: `lt-astra/hmc2-rollup-lab`, `lt-claude/hmc2-layout-lab` (results in `/Volumes/T7/sentinel-datalab/hmc2-*`).

## Waiting on the owner

1. **v3.0.0-alpha macOS release:** branch `lt-claude/mac-release-v3` tip `5082065a`, review PASS (gpt-6-astra thread `01a16c06-c945-7260-89c1-cafa71c6f81b`); waits on the owner's second-account test of the zip in `/Users/Shared/Sentinel-alpha-test/` (`TEST-STEPS.md`). Then land, release notes, tag only on the owner's yes.
2. Revoke the old Coinbase key; then the conductor deletes `key.json` at the next restart. Decide the config overlay question (Sentinel-Private security notes).

## Owner constraints in force

- Builds and tests run `-j 6` through the FIFO build queue, one at a time. No automated GUI windows while the owner works at the Mac unless asked.
- The owner hardens `scripts/dev/build-queue.sh` and its tests (FM-195); agents do not change them.
- Docs archive on the T7: `/Volumes/T7/sentinel-backup/docs-archive/`.
