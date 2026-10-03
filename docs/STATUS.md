# Sentinel Status (orchestrator hand-off)

Live state for whoever conducts the agents next (Claude or Codex). Update it when something
lands, deploys or changes priority. Rules: `AGENTS.md`; loop: `docs/AGENT_WORKFLOW.md`.

Last updated: 2026-10-03 00:55 EDT.

## Running services (owner's Mac, launchd)

- **Recorder** `com.sentinel.recorder` (`sentinel-server`, deployed 2026-10-02 23:59 from `9bf460d`):
  one Coinbase connection per product, pinned BTC-USD, GUI connection cap 8, `/metrics` on 127.0.0.1:8090.
  Rollback copy: `~/Sentinel-runtime/bin/sentinel-server.rollback`.
- **Capture** `com.sentinel.capture` (deployed 2026-10-02 22:11 from `bddb61a`): 7 products, one
  connection each, 512 MiB shared queue (2 MiB floors), `/metrics` on 127.0.0.1:8091.
  Backup: `~/Sentinel-runtime/bin/sentinel-capture.v2-backup`. First verify: ok, 0 sequence gaps, 0 missing trades.
- **Monitoring**: VictoriaMetrics :8428, Grafana :3000, node_exporter; ntfy phone alerts at high priority
  (topic only in gitignored `ops/monitoring/ntfy.env`). Re-render with `ops/monitoring/install.sh`.
- **GUI host** `scripts/dev/gui-host.py` on 127.0.0.1:17190 (started by hand with nohup; runs only main's
  GUI build; rebuild `sentinel-gui` on main after GUI changes land).
- Deploy ONLY with `scripts/dev/deploy-runtime.sh server|capture|both`; the orchestrator may restart them.

## In flight

| Branch | Agent | What | Next step |
|---|---|---|---|
| `lt-sol/gui-unsubscribe` | Codex sol (thread 01a0ffeb-c2f1-7533-8935-d08d266e4dfa) | GUI releases symbols it no longer watches (server cap 8); review fixes: switch timeout + pending status, agent API completes on activation | review (astra), land, rebuild main GUI |
| `lt-astra/roller-a` | Codex astra (thread 01a10008-1d77-7fb1-9a4d-8fdb9b039b93) | One-world slice A: roller library, `sentinel-roll`, per-product 1 bp 1-2-5 grids, `hmc2_diff`, parity + throughput | review, land, then the orchestrator runs the 7-product backfill into `/Volumes/T7/sentinel-data/hmc2` |

## Next (priority order)

1. One-world slices B-E (`docs/research/2026-10-one-world-pipeline.md`; owner decisions approved).
2. Capture 24 h verify (`sentinel-capture --verify /Volumes/T7/sentinel-data/raw-l2`, ~5-10 min; run detached).
3. Backlog: owner GUI Agent API (17100) has no auth; autoscale follow-ups (queued snapshot generation, re-subscribe snapshot timeout, 1h viewport change 2-3 ms); slice 3 minors (`SentinelServerApp` ignores `MarketDataFeeds::add` result; ConfigLoader `mdc:` block trap; deploy-runtime.sh should keep its own rollback copy); TPO and profiles v2; cosmetics; compression lab; Parquet + DuckDB research layer; Pi / cloud node.

## Owner preferences that shape the work

- Owner decides behaviour, data and looks; the orchestrator decides the rest with stated defaults. One digest,
  at most 2-3 items waiting on the owner.
- Routing (2026-10-03): Claude budget is nearly spent, so Codex writes AND reviews (sol <-> astra); Claude
  only conducts. Fable only for a must-have recorder/GPU-core gate.
- No sounds while the owner is away; loud ping only when the owner is needed.
- Builds and tests go through `scripts/dev/build-queue.sh`.
