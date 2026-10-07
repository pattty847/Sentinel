# Journal replay anchors: audit (2026-10-07)

Read-only audit by Claude Fable at the owner's request, against `main` @ b1cd28f. No decision has been taken yet;
see STATUS.

## Findings

1. **No doc, invariant or decision specifies 15-minute raw-book snapshots for the journal.** The "15m" memory most
   likely comes from two other things:
   - HMC2 schema 3's 15-minute keyframes (INV-058, `docs/MARKETDATA.md:317-324`). These are keyframes of the output
     column deltas, not order-book snapshots.
   - The cost table in `docs/research/2026-09-storage-pyramid.md:675-682` ("keyframes ... book snapshots we insert
     ourselves" at 1/5/15/60 min).

   The plan's hourly roller state checkpoint (`2026-10-one-world-pipeline.md:181-185`) was rejected by owner decision
   round 2, item 6 (`:404`): "No additional snapshot/state-checkpoint machinery".
2. **The journal holds only exchange-originated snapshots,** meaning Coinbase's connect and reconnect `l2_data`.
   Capture never parses JSON (INV-082). No Sentinel-synthesized snapshot exists on `main`. `roller.json` holds
   `{pos, committedThroughMs, configHash, fromMs}`, with no book state.
3. **Replay start points:**
   - **Restart:** `JournalReader::anchor(day)` picks the newest exchange snapshot at or before 00:00Z, searching
     earlier files without limit. The whole day is then replayed (`Roller.cpp:91-92`, `JournalReader.cpp:86-99`).
     Measured at the 2026-10-06 18:36 EDT restart: all 7 products caught up in 41-56 s, with anchors in the previous
     UTC day.
   - **Midnight:** the same path. On 2026-10-07 at 00:01Z this took about 2 s, but the cost is bounded only by the
     age of the connection.
   - **Mid-connection start:** updates before the first snapshot are dropped, and those minutes are marked invalid.
   - **Torn tail or gap:** the roller stays invalid until the next exchange snapshot.
   - **Interior CRC corruption:** the read throws, the day re-anchors, and a persistent corruption loops forever,
     because no code path skips a corrupt block.
   - **Ring miss:** a file catch-up from the applied cursor; no day replay.
4. **UTC days are NOT independently replayable.** Day N's anchor usually lies in day N-1's files. Deleting or losing
   day N-1 changes day N's anchor and grid, and `configHash` then refuses the checkpoint.
5. **Format:** RAWL2 v1 has no slot for non-exchange records. The writer rejects kind > 8 (`RawCapture.cpp:314`)
   and the reader rejects it (`:509`). Anchors belong in a separate sidecar store, which needs no RAWL2 change.
6. **Storage (real data):** about 95.7k levels across the 7 products, at about 110 B per level of JSON. That is
   about 10.5 MB per round. At a 15-minute cadence it comes to about 96 MB/day compressed (BTC about 42 MB), which is
   **+11-15 % per day, about 35 GB/year**. A compact integer sidecar would likely be smaller (unverified).
7. **Benefit:**
   - Restart drops from 41-56 s to about 1-3 s, and midnight is bounded the same way.
   - The hourly oracle scans at most 15 minutes instead of whole days.
   - Each day becomes replayable from its own files plus its 00:00Z anchor.

## Recommended invariant

RAWL2 is pristine wire evidence only: every record is an observed exchange frame or a capture lifecycle event, never
derived data. Sentinel-generated replay anchors live in a separate sidecar store. Each anchor is the full raw book per
product at a UTC 15-minute boundary. Anchors:

- are built only from durable journal records;
- are tagged with the JournalPos they reflect;
- are rebuildable from the journal;
- are seeded into the book as state, never as an exchange snapshot event and never as `kResynced`.

A replay from an anchor must be byte-identical to a replay from the day's exchange snapshot. Any UTC day is
replayable from its own files plus its 00:00Z anchor.

## Summary

- **CURRENT:** exchange snapshots only. Restart and midnight replay from the last exchange snapshot, which is often
  in the previous day.
- **INTENDED:** not specified anywhere. The hourly checkpoint was rejected on 2026-10-03.
- **GAP:**
  - no anchors;
  - days depend on the previous day's files;
  - restart and midnight costs are bounded only by connection age;
  - a corrupt mid-day block makes that day unrecoverable.
- **RISK:**
  - about 1 min of live dark per restart (the live book too, after D-b);
  - growing midnight windows;
  - 87.5 product-day oracle scans per day;
  - losing day N-1 breaks day N.
- **SLICE:** a separate post-one-world slice after R2 and D-b, as a sidecar with no RAWL2 change. Acceptance: strict
  `hmc2_diff` = 0 between a full-day replay and a resume at every 15-minute anchor on a real day, plus one midnight
  with no previous-day files.

Evidence (file:line): `JournalReader.cpp:18-25, 86-99`; `Roller.cpp:91-92, 107-126, 165-168`;
`JournalFeed.cpp:14-17, 67-71`; `ShadowRoller.cpp:411-434, 743-761, 832-838, 939-941`; `RawCapture.hpp:21-24,
168-170`; `RawCapture.cpp:314, 509`; `CaptureApp.cpp:178, 205-232`; `docs/ROLLER.md:78-86, 126-136`;
`docs/RAW_CAPTURE.md:569-575`; tests `test_roller.cpp:157, 236, 441`, `test_shadow.cpp:473, 586, 761, 789, 1593,
1658, 1676`; server log `sentinel-server-20261006-183604-1287.log`; VictoriaMetrics `sentinel_roller_shadow_lag_seconds`.
