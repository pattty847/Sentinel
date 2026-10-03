# Sentinel Status (orchestrator hand-off)

Live state for whoever conducts the agents next (Claude or Codex). Update it when something
lands, deploys or changes priority. Rules: `AGENTS.md`; loop: `docs/AGENT_WORKFLOW.md`.

Last updated: 2026-10-03 08:40 EDT. Trade bubbles landed; main GUI rebuilt.

## Running services (owner's Mac, launchd)

- **Recorder** `com.sentinel.recorder` deployed 2026-10-03 06:09 from `2b84256`: one connection per product, cap 8, recorder stops recording released GUI symbols (verified live), trade side = aggressor (footprint colours look swapped). Rollback: `bash scripts/dev/deploy-runtime.sh rollback server`.
- **Capture** `com.sentinel.capture` deployed 2026-10-02 22:11 (`bddb61a`). Slice B fan-out LANDED (`2f45fff`) but NOT deployed: deploy with the owner present (`deploy-runtime.sh capture`; watch `~/Sentinel-runtime/run` 0700, `sentinel_fanout_running 1`, journals keep writing, reload Grafana for the new rule).
- **Monitoring**: VictoriaMetrics :8428, Grafana :3000, ntfy high priority. **GUI host** :17190 (main build).
- **Backfill** done: `/Volumes/T7/sentinel-data/hmc2` (7 products, ~122 MB). Not served yet (slices C/D).

## In flight

| Branch | State | Next step |
|---|---|---|
| (none) | | |

## Next (priority order)

1. Morning with owner: capture deploy (slice B); look at candles (landed `db7aa70`), bubbles (landed; gear menu Trades toggle), S7c screenshots (`screenshots/s7c/`, S8 = GO); footprint colour check.
2. One-world slices C (shadow live roller), D (cutover), E (delete old path).
3. S8: delete the legacy heatmap (Fable verdict GO).
4. Backlog: owner GUI Agent API (17100) unauthenticated; GUI RSS ~1.5 GB after 10 min; stale-book status not cleared on disconnect (RemoteGridDataSource.cpp:146); fan-out retry clears rings each attempt; AGENTS.md 4b: `--agent-host` needs `--agent-host-symbols` explicitly; TPO v2; cosmetics; compression lab; Parquet + DuckDB; Pi / cloud node.

## Owner preferences that shape the work

- Owner decides behaviour, data and looks; the orchestrator decides the rest with stated defaults. One digest,
  at most 2-3 items waiting on the owner.
- Routing (2026-10-03): Codex writes; Claude Fable (separate usage limit) reviews Codex work; avoid Claude
  opus/sonnet subagents until the owner says the Claude weekly has reset.
- No sounds while the owner is away; loud ping only when the owner is needed.
- Builds and tests go through `scripts/dev/build-queue.sh`.
