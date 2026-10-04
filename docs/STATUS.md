# Sentinel Status (orchestrator hand-off)

Live state for whoever conducts the agents next (Claude or Codex). Update it when something
lands, deploys or changes priority. Rules: `AGENTS.md`; loop: `docs/AGENT_WORKFLOW.md`.

Last updated: 2026-10-03 22:20 EDT. Shadow roller soak started (BTC-USD).

## Running services (owner's Mac, launchd)

- **Recorder** `com.sentinel.recorder` deployed 2026-10-03 22:14 from `958221d`+config: primary recorder unchanged; **shadow roller ON** (BTC-USD, the only nonstop-recorded product) writing `/Volumes/T7/sentinel-data/hmc2`; soak target: `sentinel_roller_shadow_mismatch_total` = 0 until ~2026-10-05 22:15. Rollback: `bash scripts/dev/deploy-runtime.sh rollback server` or set `roller_shadow.enabled: false` + redeploy.
- **Capture** `com.sentinel.capture` deployed 2026-10-03 22:13 (fan-out keeps rings on socket failures): 7 products, fan-out socket `~/Sentinel-runtime/run/capture.sock`. Rollback: `bash scripts/dev/deploy-runtime.sh rollback capture`.
- **Monitoring**: VictoriaMetrics :8428, Grafana :3000, ntfy high priority. **GUI host** :17190 (main build).
- **Backfill** done: `/Volumes/T7/sentinel-data/hmc2` (7 products, ~122 MB). Not served yet (slices C/D).

## In flight

| Branch | State | Next step |
|---|---|---|
| landed | W1a DOM `3c9b948` (compact standard ladder, verified via own-branch screenshots); W1c dock infra `af09b9f` (per-dock + window shots, `--build` own-branch launches, TradeBlotter/Lab/AICommentary removed); W1b paper ticket `e301c85`; W1d doc `docs/research/2026-10-copenet-boundary.md` | |

## Next (priority order)

1. Widget pass: owner directive `docs/research/2026-10-widget-pass-directive.md`, plan `docs/research/2026-10-widget-pass-plan.md`. Wave 1 in flight; wave 2 after the Claude weekly reset (2026-10-06). S8 = GO pending owner.
2. Slice C landed `0a2e0bc` (`roller_shadow`, default off). Pre-enable fixes landed `98e5b49` (cooldown needs a 2 min streak; fan-out poll/listener failures keep rings). Soak running since 22:14 Oct 3 (BTC only, by design: the comparison needs the primary's nonstop recording). After 48 h at 0: slice D cutover; D must widen the roller to all 7 journal products (continuous history for every coin), then E deletions.
3. S8: delete the legacy heatmap (Fable verdict GO).
4. Backlog: owner GUI Agent API (17100) unauthenticated; GUI RSS ~1.5 GB after 10 min; persistent fan-out poll() error loops without sleeping (CaptureFanout.cpp:417, pre-existing, low); add safe dock screenshot targets to the GUI host before widget visual work; git remote moved to github.com/pattty847/Sentinel.git; TPO v2; cosmetics; compression lab; Parquet + DuckDB; Pi / cloud node.

## Owner preferences that shape the work

- Owner decides behaviour, data and looks; the orchestrator decides the rest with stated defaults. One digest,
  at most 2-3 items waiting on the owner.
- Routing (2026-10-03): Codex writes; Claude Fable (separate usage limit) reviews Codex work; avoid Claude
  opus/sonnet subagents until the owner says the Claude weekly has reset.
- No sounds while the owner is away; loud ping only when the owner is needed.
- Builds and tests go through `scripts/dev/build-queue.sh`.
