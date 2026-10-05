# Sentinel Status (orchestrator hand-off)

Live state for whoever conducts the agents next (Claude or Codex). Update it when something
lands, deploys or changes priority. Rules: `AGENTS.md`; loop: `docs/AGENT_WORKFLOW.md`.

Last updated: 2026-10-04 21:52 EDT. Codex acting orchestrator; wave 1 complete, per-product live book tick in progress.

## Running services (owner's Mac, launchd)

- **Recorder** `com.sentinel.recorder` deployed 2026-10-03 22:14 from `958221d`+config: primary recorder unchanged; **shadow roller ON** (BTC-USD, the only nonstop-recorded product) writing `/Volumes/T7/sentinel-data/hmc2`; soak target: `sentinel_roller_shadow_mismatch_total` = 0 until ~2026-10-05 22:15. Rollback: `bash scripts/dev/deploy-runtime.sh rollback server` or set `roller_shadow.enabled: false` + redeploy.
- **Capture** `com.sentinel.capture` deployed 2026-10-03 22:13 (fan-out keeps rings on socket failures): 7 products, fan-out socket `~/Sentinel-runtime/run/capture.sock`. Rollback: `bash scripts/dev/deploy-runtime.sh rollback capture`.
- **Monitoring**: VictoriaMetrics :8428, Grafana :3000, ntfy high priority. **GUI host** :17190 responds (main build); paper-limit-guard own-branch enabled/disabled captures inspected this session; tick slice not live-validated.
- **Backfill** done: `/Volumes/T7/sentinel-data/hmc2` (7 products, ~122 MB). Not served yet (slices C/D).

## In flight

| Branch | State | Next step |
|---|---|---|
| `lt-sol/product-book-tick` | frozen/rebased `9ac52e0` passed 5/5 targeted suites; Fable BLOCK, fix round in same worktree | remove metadata-triggered reconnect/recorder invalidation by continuously maintaining raw state before aggregation; preserve GUI unavailable retry/stale/backoff state; regression tests then Fable re-review; no landing/deploy yet |
| landed `lt-sol/paper-limit-guard` | merge `78f5b63`, reviewed patch rebased unchanged as `52bfcb2`; limit buttons require a valid displayed price; focused 7/7 cases and full landing 91/91 targets passed | Fable round two PASS; enabled/disabled own-branch captures inspected; worktree removed; focus-out guard is a defensive simulated case, not naturally reproduced |
| landed | W1a DOM `3c9b948` (compact standard ladder, verified via own-branch screenshots); W1c dock infra `af09b9f` (per-dock + window shots, `--build` own-branch launches, TradeBlotter/Lab/AICommentary removed); W1b paper ticket `e301c85`; W1d doc `docs/research/2026-10-copenet-boundary.md` | |

## Next (priority order)

1. Per-product live book tick (HANDOFF Next work #1): server owns the grid and publishes the authoritative tick to GUI replicas. Wave 1 complete. Widget wave 2 after the tick slice and owner-applied Codex reset credit (owner correction Oct 4; no Claude-reset date gate), following `docs/research/2026-10-widget-pass-directive.md` and `docs/research/2026-10-widget-pass-plan.md`.
2. Slice C landed `0a2e0bc` (`roller_shadow`, default off). Pre-enable fixes landed `98e5b49` (cooldown needs a 2 min streak; fan-out poll/listener failures keep rings). Checked Oct 4 20:46 EDT: running=1, near/deep mismatches=0, setup/comparison failures=0, lag ~0.36 s. Rechecked during tick review: running=1, near/deep mismatches=0, setup/comparison failures=0, lag ~0.22 s. Soak running since 22:14 Oct 3 (BTC only, by design: the comparison needs the primary's nonstop recording). At ~Oct 5 22:15 after 48 h at 0: propose slice D to owner; no cutover without approval and owner present; D must widen the roller to all 7 journal products (continuous history for every coin), then E deletions.
3. S8: delete the legacy heatmap (Fable verdict GO; pending owner approval).
4. Backlog: owner GUI Agent API (17100) unauthenticated; GUI RSS ~1.5 GB after 10 min; persistent fan-out poll() error loops without sleeping (CaptureFanout.cpp:417, pre-existing, low); safe dock/window grabs and own-branch host launches landed in W1c; GitHub API confirms repository moved to `pattty847/Sentinel`; origin updated to canonical `https://github.com/pattty847/Sentinel.git`; internal disk 8.7 GiB free, main build 3.2 GiB; TPO v2; cosmetics; compression lab; Parquet + DuckDB; Pi / cloud node.

## Owner preferences that shape the work

- Owner decides behaviour, data and looks; the orchestrator decides the rest with stated defaults. One digest,
  at most 2-3 items waiting on the owner.
- Routing (2026-10-03): Codex writes; Claude Fable (allowance separation unproven) reviews Codex work; avoid Claude
  opus/sonnet subagents until the owner says the Claude weekly has reset.
- No sounds while the owner is away; loud ping only when the owner is needed.
- Builds and tests go through `scripts/dev/build-queue.sh`.

## Acting-orchestrator checks

- Claude Fable alias verified with `claude -p --model fable` on Oct 4. One small STATUS review passed; before/after Claude weekly 3% left, session 97% -> 96% left. Paper review used model `claude-fable-5-1`; weekly still 3% left afterward, session 96% -> 94% left. Tick review used `claude-fable-5-1`, BLOCK; weekly remained 3% left, session 94% -> 87% left. Whole-percent precision does not prove a separate Fable allowance. Check around each review; if weekly remaining drops, stop Claude reviews and notify owner before any landing. Codex weekly initially 8%, latest check 4% remaining; use it before owner applies reset credit.
- Wave 2: at most three writing lieutenants; W2b alone owns hubs. Review every branch with Fable only if the budget check permits; no opus/sonnet writers before owner confirmation of the Claude weekly reset.
- Deployments and slice D require asking the owner first and confirmation that the owner is present.
- Unrelated `lt-sol/label-style` locked worktree and untracked `.codex/` left untouched.
- Recorder/capture remain deployed versions above. Server logs checked; shadow journal boundary invalidation at 20:01 recovered with no mismatch/comparison failure.
- Small ticket fix dispatched: disable Buy/Sell Limit with tooltip while no valid limit is set. Backup waits for the owner's new HDD. EATEN/PULLED/ABSORPTION requires a Fable plan across roller/HMC2/wire/renderer after slice D.
