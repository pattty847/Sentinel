# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends.

- **Updated:** 2026-10-06 18:45 EDT. R1 deployed; 48 h soak running until 2026-10-08 ~18:40 EDT.
- **Conductor:** Claude Opus 5.5. The owner handed conducting back on 2026-10-06; Codex does not dispatch.
- **Review policy:** 1 + 1 cross-provider (AGENTS.md section 10). No fallback active: the 2026-10-04
  GPT-reviews-GPT override ended when Claude usage reset.
- **Budget (D-a review):** Claude weekly 94% left; Codex weekly 58% left but +18% DEFICIT (runs out in ~2.3 d at this pace); owner holds 2 Codex reset credits.

## Resume (fresh conductor)

1. Read `AGENTS.md` and this file. See what landed since: `git log --oneline --first-parent <last STATUS commit>..main`.
2. `scripts/dev/budget.sh`; service checks in `docs/AGENT_WORKFLOW.md` "Data and performance".
3. `git worktree list`, `scripts/dev/build-queue.sh status`, `curl -s 127.0.0.1:17190/status`.
4. Resume the threads listed under In flight; do not restart finished work.

## Services (owner's Mac, launchd)

- **Recorder** deployed 2026-10-06 18:36 EDT from `08d3bb8` (R1: D-a code, roller shadow on all 7 capture products from 2026-10-06, `recording.source: primary`). All 7 lags < 5 s within 54 s of the deploy; server 4% CPU, 243 MB. BTC `comparison.json` moved aside to `comparison.json.pre-r1` (owner-approved; the `from` bump broke its identity, FM-203), so BTC parity restarts from 2026-10-06 00:00Z. R1 soak gates (plan slice D section 5): all 14 mismatch series 0 for 48 h incl. one UTC midnight; rollback triggers: any mismatch, fault_cooldown 1, primary column_overdue > 120, disk_errors > 0; rollback = `deploy-runtime.sh rollback server` + revert `08d3bb8`. Previous deploy notes: Shadow roller ON for BTC-USD,
  writing `/Volumes/T7/sentinel-data/hmc2`. The 48 h soak passed (2026-10-05 22:17). At 2026-10-06 01:55:
  near/deep mismatches 0, setup/comparison failures 0, lag 0.9 s; primary queue drops and disk errors 0.
  Global invalidations are 30 and cumulative; they are not a parity signal.
  Rollback: `scripts/dev/deploy-runtime.sh rollback server`, or `roller_shadow.enabled: false` and redeploy.
- **Known limit:** the legacy cent-scale primary format rejects prices below $0.005 (PEPE, low DOGE levels).
  Slice D must serve the product-aware roller grids, not this format. Live aggregation uses a fixed band: an
  out-of-band best price invalidates it until the next upstream snapshot (recording continues).
- **Capture** deployed 2026-10-03 22:13; 7 products, all feeds up at 01:55. Rollback:
  `scripts/dev/deploy-runtime.sh rollback capture`.
- **GUI host** :17190 idle, no session. **Monitoring:** VictoriaMetrics :8428, Grafana :3000.

## In flight (T7 worktrees)

| Branch | Tip | State | Next |
|---|---|---|---|
| `lt-claude/s8a-legacy-gui` | `e258947` (base) | S8a writer (Claude `opus`): A/B capture, then delete legacy GUI/client per `docs/research/2026-10-s8-legacy-deletion.md` | Review `gpt-6-astra` high with native evidence, land; R2 needs it |
| `lt-sol/label-style` | `d52921b` | Locked; edits `HeatmapLabelRenderer`/`HeatmapOverlayRenderer`, which S8a deletes | Owner: retire or port to GPU labels |
| `lt-claude/heatmap-ab-isolation` | `88e7ef4` | Unrelated, locked | Leave alone |

Evidence (logs, screenshots, review reports) is in ignored `.claude/acting-orchestrator/`. The
`.claude/worktrees/youthful-ishizaka-b0e889` (`60caa04`, branch `claude/exciting-hypatia-1c7a34`) is not part of this work; leave it alone.

## Waiting on the owner

None. (S8 legacy heatmap deletion approved by the owner 2026-10-06; see Next.)

## Owner constraints in force

- The owner is hardening `scripts/dev/build-queue.sh` and its tests (FM-195). Agents do not change them or
  sweep the owner's changes into commits.
- The 2026-10-05 resource hold was released by the owner on 2026-10-06. Builds stay at `-j 2` while the owner is at the Mac; the owner allows more (`-j 6` used) while away (2026-10-06); no automated GUI windows while the owner is working, except: the owner approved (2026-10-06) the D-a phase 2 isolated GUI run and conductor use of the Mac for it, and said isolated agent GUIs running in the background do not bother them (they will not close them).

## Next (priority order)

1. Wave 3 complete (2026-10-06): overflow redesign `66e39b2`, retained fonts `fac62a0`, settings with hover tooltips `542be4d`; each owner-approved, full suite 96/96.
2. Slice D-a LANDED 2026-10-06 as `e8958cd` (option C live path, roller serving switch off by default). Next: R1 widen (owner present; runbook in the plan's slice D section 5 plus owner decisions 5-6), then the 48 h soak, during which S8 and the acceptor fix (FM-154/202) are dispatched. Acceptor restart fix (FM-154/202) LANDED `d10c56a` (97/97; Sol review PASS); not deployed: it ships with the R2 redeploy, so the soak is not restarted. R1 checkpoints: first hourly comparison 2026-10-06 19:37 EDT, all 7 products compared through 23:00Z, 14 mismatch series 0. First UTC midnight (20:01 EDT): all 7 rolled to 2026-10-07, lags < 1.5 s, no cooldown. Original packet: packet in `docs/research/2026-10-one-world-pipeline.md` "Slice D task packet" plus "Owner decisions on the slice D packet (2026-10-06)" (live path option C: no live-age regression; split D-a/D-b; `from` bump). Writer Claude `opus` (needs native evidence), reviewer `gpt-6-astra` high. Runbook R1 (widen to all 7 products) and R2 (flip) are conductor steps with the owner present. Then D-b (engine replaced by the journal feed: one Coinbase connection) and slice E deletions.
2a. S8 (delete the legacy heatmap, owner-approved 2026-10-06): dispatch after D-a lands, during the R1 48 h soak (GUI files only; no overlap with roller/server files). First step of the packet: one final legacy-vs-gpu A/B capture with the same steps as `docs/research/2026-10-s6-plan.md` "Sequence and timings", kept as the before/after record. Writer Claude (needs Metal), reviewer `gpt-6-astra` high (GPU heatmap core). Removes the D-a legacy-renderer limit (the legacy page path cannot drop a withdrawn live column). Packet must cite FM-079 (removing legacy heatmap production once stopped live footprint, TPO and volume profile).
3. After D: change-driven live publishing with a per-client max update rate (see the owner decisions block). Then EATEN vs PULLED liquidity and ABSORPTION (owner's strongest direction): Fable plan after slice D. It crosses
   roller, HMC2, wire and renderer.
4. On hold (owner, 2026-10-06): TPO session choices become Session (draws New York, London, Asia and Australia together, like ExoCharts), 24H, 1W, 1M. Changes what the TPO draws; needs its own spec.
5. After one-world (owner, 2026-10-06; GUI polish, not before slice E): order book cumulative-size mode; one tick size shared by the heatmap and the order book (today they look separate, and where to change the order book tick is unclear); heatmap and candles drift apart slightly while zooming out with the wheel (suspect two scaling paths; check FM-001 / INV-004 first); local-time axis labels (owner 2026-10-06): `TimeAxisModel` already has a `timezone` property (UTC default; America/New_York, Europe/London, Asia/Tokyo) but no control sets it, and `HeatmapGpuLayer.cpp:16-19` hard-codes UTC in its labels; add a setting and route every time label through one timezone; zoom-to-footprint (owner 2026-10-06, from an X demo): candles widen into bid/ask footprint cells and then show numbers as you zoom in; extends existing F4 footprint and TODO "text LOD system (numbers -> hints -> none)", not a new feature; an order book imbalance strip from the Coinbase book (multi-venue, perps and open interest would be a separate data-scope decision); BTC-USD order book at 0.1 tick showed no bids in the 20 rows under the best bid (thin book or display bug, unverified).
6. Backlog: internal disk has ~5 GB free (builds and logs at risk); `config/client_config.yaml` top-level `server:` is ignored (only `client.server` is read; nearly pointed an agent GUI at :8080); PEPE at its native 1e-8 tick draws ~200 px rows and logs "native rows exceed int32" in the GUI (the tick is ~0.25-1% of PEPE's price, so few levels exist: the blocks are partly real. Fix direction from a 2026-10-06 look at other platforms: readable price-axis units for tiny prices, as exchanges list 1000PEPE; a minimum visible price span or row-height cap in auto-scale; no fake sub-tick grid); Codex writers could commit in the sandbox via a separate-gitdir clone (parked, `docs/AGENT_WORKFLOW.md`); tick-size combo text clips at its fixed 84 px width ("$0.15 (unavailable)", pre-existing); owner GUI Agent API (17100) is unauthenticated; GUI RSS ~1.5 GB after 10 min; capture fan-out
   `poll()` error loop without sleep (`CaptureFanout.cpp:417`, low); W2d live GUI display and Windows runtime
   unverified; TPO history budget refusal in the GUI log; nightly backup of `/Volumes/T7/sentinel-data` when the
   owner's new HDD arrives; internal disk space.
