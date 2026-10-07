# Architecture audit: conductor triage (2026-10-07)

Source: the owner's 5-agent architecture audit (session `sentinel-architecture-audit`, report
`sentinel-architecture-audit.md`), plus the owner's ChatGPT read of it. This file gives a verdict on every point,
checked against the code and the live system on 2026-10-07. Status words: **true**, **partly true**,
**already handled** or **wrong**.

## Done today (2026-10-07)

1. **Second copy of the irreplaceable data. True; done.**
   - T7 `sentinel-data/raw-l2` (5.3 GB) and `recording` (147 MB) were copied to the internal SSD at
     `~/Sentinel-backup/t7-sentinel-data/`.
   - The internal SSD's `data/market` (the only copy of the Feb-Mar trade files, 179 MB) was copied to the T7 at
     `/Volumes/T7/sentinel-backup/internal-data/`.
   - Copy only: rsync, no deletes, sources read only. This is a one-time copy. A nightly backup comes with the 5 TB
     drive (backlog).
2. **INV-136 recorded branch behaviour as fact on main. True; fixed.** It is now marked `[BRANCH lt-claude/roller-db1
   ONLY]` until D-b1 lands.
3. **Listener exposure. True.** The server listens on `*:8080` (lsof), and the run log shows 2 connections from
   `192.168.1.1` (most likely the router). An owner decision is below.

## Before R2 (Oct 8 evening)

4. **The R2 runbook requires `fanout_clients == 1`. True; it is wrong.** Steady state is 7: one per product for the
   roller workers (FM-207; Astra noted the same in its D-b1 review). The precondition becomes `fanout_clients == 7`
   with `capacity` disconnects at 0.
5. **The R2 runbook says "same binary". True; wrong.** A redeploy from `main` would also ship the acceptor fix
   (`d10c56a`), and the candle-history fix if it lands first. The options are in owner decision B.
6. **One corrupt journal block stops that product's roller forever. True, and worse than the anchors audit said.**
   - Mechanism: a CRC or zstd failure throws, the day re-anchors, and it fails again in a loop (FM-173 probe). The
     day never completes, and later days never start, because days are processed in order.
   - Today the roller is shadow-only, so this is harmless. After R2 it freezes that product's served heatmap
     history and its live minute.
   - Anchors do not fix this; an explicit corruption policy is needed. See owner decision A.
   - Mitigation that exists today: `fault_cooldown` is a metric with an alert, and rolling R2 back to `primary` is
     a config revert.

## Order of work after R2 (written once; change it only on purpose)

R2 → D-b1 (journal live feed) → anchors deploy → D-b2 (engine deletion) → S8b + slice E (legacy deletion) →
README/ARCHITECTURE pass → architecture freeze and the workflow/storage audit.

Each step deploys separately with its own soak, because one deploy restarts the other's soak clock.

7. **D-b1 cannot pass its own gate: nothing measures main-thread load. True.** Add a server event-loop lag metric
   (p50/p95/p99/max: a QTimer lateness sampler on the main thread, one gauge or histogram) before D-b1's flip.
   Measure the `engine` baseline, then `journal`.
8. **D-b1 and anchors cannot soak together. True.** This is already the plan: anchors phase B is built on D-b1's
   tip, and they deploy sequentially.

## Traps in planned deletions (fix the plans before those slices)

9. **S8b deletes `heatmap.*` server config that candles, footprint retention and the toolbar still read. True as a
   risk.** Before S8b: move each still-used key to a neutral section and list every reader (`rg` for each key).
   Recorded in `docs/research/2026-10-s8-legacy-deletion.md` section 8.
10. **D-b2 removes `TickBinaryLogger`, but `sentinel-backtest` reads its trade files. True; the D-b packet was
    wrong** (it said only a test uses it). D-b2 must keep the logger, or give `sentinel-backtest` the journal's
    trades as its input. Recorded in the D-b packet.

## Docs that are wrong (fix in the README/ARCHITECTURE pass after slice E)

11. **ARCHITECTURE.md describes a dedicated aggregation thread; the main thread does that work, and the capture,
    journal and fan-out stages are missing. True.**
12. **The journal-anchors audit understated the corrupt-block risk. True.** Item 6 corrects it.

## Not now (backlog)

13. **The GUI never reconnects after a server restart. True** (seen in D-b1). The runbooks say: restart the GUI after
    each server deploy. A small client task (auto-reconnect with backoff) is good Codex work after R2.
14. **540 GiB written to the T7 in 24 h, unattributed. Unknown.** Worktrees and builds are on the T7. Moving
    worktrees and builds off the data disk is cheap; do it at a quiet point. Attribute the writes in the storage
    audit.
15. **Legacy machinery still runs** (the TWAP streamer and `.hmcol`, `heatmap_slice` at about 20 Hz, the duplicate
    trade log, the latency broadcast). True; it is planned for deletion in S8b/E and D-b2.
16. **33.9 GB of swap from builds and agent apps on the 16 GB Mac. True as measured.** For the workflow audit; it
    also argues against `-j 6`.
17. **The ntfy topic URL is in plain text in `~/Library/LaunchAgents/com.sentinel.grafana.plist`. True.** It
    belongs in `ops/monitoring/ntfy.env` per AGENTS. The fix is in `ops/monitoring/install.sh` (owner's monitoring
    setup); low risk while that file stays local and uncommitted.

## Agreed with the audit

Not worries: CPU (under 10% of one core), storage runway (about 2.4 years), roller correctness (0 mismatches), queues,
GPU frame time, the core/GUI split, and Coinbase reconnects. One World is about 60-65% done by effort (about 40% by
end-state conditions).

## Owner decisions needed (3)

- **A. Corrupt journal block policy.** Recommended: skip the damaged block, mark that interval invalid (a visible
  gap, never fabricated data), resume at the next exchange snapshot or anchor, and alert loudly (log, metric,
  ntfy). The alternative is to halt loudly. Today's behaviour (looping while it looks alive) is the wrong one either
  way.
  - Implementation: a small Codex slice in `JournalReader`/`Roller`, coordinated with anchors phase B.
  - It need not block R2: the failure is loud (`fault_cooldown` alert), and rolling back to `primary` is a config
    revert.
- **B. What R2 deploys.** Recommended: R2 restarts exactly the soaked binary with the config flip (config-only, as
  the runbook intended). This needs a small `deploy-runtime.sh restart server` mode: the same verify and rollback,
  with no copy. Then main (acceptor fix + candle fix) ships as its own later deploy. The alternative is one redeploy
  from main carrying three changes at once.
- **C. Listener exposure.** Recommended: bind the stream server to `127.0.0.1` by default now. Remote clients (the
  Windows machine, a future Pi) come back with authentication as part of the multi-server work.
