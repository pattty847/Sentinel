# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends. Keep under 8 KB.

- **Updated:** 2026-11-09, wave 3 RESUMED (S3a writer resumed, saturation Fable review re-run, S2 dispatched). **main** pushed (see `git log -1`). Work order: `docs/TODO.md` Waves.
- **Conductor:** Claude Opus 5.5, wave 3 (started 2026-11-08).
- **Review policy:** 1 + 1 cross-provider (AGENTS.md section 10); Fable for high risk. Owner 2026-11-05: spend all Codex, including reset credits before they expire.

## Next

1. Wave 3 plan: `../Sentinel-Private/research/2026-11-08-why-it-moved-plan.md` (12 slices, section 7 order). Landed: S2 flow-store `67d3644a` (deferred: stop() bounded by one uncancelled JournalReader header walk, ~0.3-3 s cold, hourly; after deploy measure server RSS before/after the first pass for catalogs_), S3a flow-live-model `9beb557c` (deferred: loss flag from 1m only; S4 needs a re-fetch rule when `complete` turns false without a revision bump; sweep test name), S1 flow-bar-core `131596b4`, heatmap seam `da486efd`, axis drag `b77e7fb9`. Conductor decision: `revision` is not in FLB1 content; the wire envelope carries `revisionAt`. S8a waits for `lt-astra/heatmap-saturation` (both edit `HeatmapChartSettings.hpp`). Owner decisions (plan section 6): 1 flow store = derived store like rollups (owner APPROVED 2026-11-08; normal deploy; measured 0.51 GB per BTC-year), landed; 2 bubble defaults: owner judges both modes in the lab (S8a ships both behind a setting); 3 footprint/CVD look: visual pass with the owner in the lab before goldens. Palette waits for the owner's colour variants. Owner to check the seam fix live: 1h, zoom near the now column.
2. With the owner present: build main, then `scripts/dev/deploy-runtime.sh server` for the trade-id window `87c9ccbc`, S3a live flow `9beb557c` and S2 flow store `67d3644a` (new derived dir `recording.flow_dir`, default `<roller_shadow.dir>-flow`; no RAWL2/HMC2 change). The script refuses a stale or dirty build.
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

- `lt-astra/heatmap-saturation` (worktree `/Volumes/T7/sentinel-worktrees/lt-astra-heatmap-saturation`): tip `ad93f22e` = recording-floor display scale + per-asset slider (auto-fit once to visible cells, origin/fittedAt saved, reset-to-auto, no carry-over) + manual tick falls back to the asset's Auto tick. Fable PASS (4 minors: price-provider symbol guard; warn on refused frames; min..max fit includes dust cells, owner to judge; Agent API patch quirks). Conductor native Metal FAIL (`saturation-native4.log`: UgrGpu TINY maxPriceSpan 0; HeatmapChartUi settle timeouts; ChartUi range-slider persistence and labels). Past two fix rounds: Diagnosed by Claude Opus (native single-case runs, scratchpad `diag/`): product deadlock, prepareFrame refused frames until a book/trade price, while the fit needs labels from frames (real users: blank heatmap when the snapshot seeds first); manual fallback maxPriceSpan 0; tests encoding the old global slider. Fix spec sent 2026-11-09 (packet `astra-saturation-fix3.txt`): never gate frames on the pending range (temporary unsaved window), wider same-symbol price source, the manual fallback HOLDS its first Auto tick (conductor default, owner can flip), test updates. Writer thread `01a1be15-f436-7d21-8eaa-b2a3cfe59f36`. HIGH.

- `lt-astra/flow-history` (worktree `/Volumes/T7/sentinel-worktrees/lt-astra-flow-history`): plan S3b. Tip `5cefc891` (store attached at startup; `source` store|live; final = bucket_end <= sealed_through_ms; partially sealed coarse bars provisional live complete=false). Opus review FAIL (2 major: hourly store/live seam hole; uncancellable, uncapped-cost 1 s journal replay on the shared history pool). Fix round 1 sent 2026-11-09 (packet `astra-s3b-fix1.txt`; also owns S3a retention + the store wake). Writer gpt-6-astra thread `01a1bf84-c80c-7da0-b45c-d5707f89449e`, reviewer Claude Opus. MEDIUM.

Lab branches that never land: `lt-astra/hmc2-rollup-lab`, `lt-claude/hmc2-layout-lab` (results in `/Volumes/T7/sentinel-datalab/hmc2-*`).

## Waiting on the owner

1. **v3.0.0-alpha macOS release:** branch `lt-claude/mac-release-v3` tip `5082065a`, review PASS (gpt-6-astra thread `01a16c06-c945-7260-89c1-cafa71c6f81b`); waits on the owner's second-account test of the zip in `/Users/Shared/Sentinel-alpha-test/` (`TEST-STEPS.md`). Then land, release notes, tag only on the owner's yes.
2. Revoke the old Coinbase key; then the conductor deletes `key.json` at the next restart. Decide the config overlay question (Sentinel-Private security notes).

## Owner constraints in force

- Builds and tests run `-j 6` through the FIFO build queue, one at a time. No automated GUI windows while the owner works at the Mac unless asked.
- The owner hardens `scripts/dev/build-queue.sh` and its tests (FM-195); agents do not change them.
- Docs archive on the T7: `/Volumes/T7/sentinel-backup/docs-archive/`.
