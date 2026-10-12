# BTC journal replay fixture

`btc-prefix.rawl2` is an exact, read-only prefix (file header + 180 complete blocks)
of the public Coinbase journal:

`BTC-USD/2026/10/01/04.49ffaa57-d40e-4df1-bb61-7bfd31a6d36e.0.rawl2`

It covers roughly the first 185 seconds from receive time 2026-10-01 04:53:00 UTC.
The index and later blocks are deliberately excluded; the prefix is unsealed.
No credentials, private trades or owner GUI state are in the journal. File size:
1,029,019 bytes. SHA-256:
`dc9d9f8e34c774c2c64a7aab3eae419daa67bb90e59f60fb8e9d886aee17e115`.

The prefix is retained for journal replay, grid derivation and anchor fallback
regressions. The retired primary-recorder timer parity harness and its output
hash are no longer acceptance gates; live behaviour is pinned by the history/lead
and capture reconnect scenarios in `test_shadow.cpp`.

## Real 16:00 hour-boundary fixture

`btc-hour-boundary.rawl2` preserves every original serialized record (kind,
receive/steady stamps, connection and JSON payload) from:

- All 44,045 records in
  `BTC-USD/2026/10/01/15.630f00d1-6671-45b4-974a-de7dcda206fb.0.rawl2`.
- The first complete blocks through receive time 16:00:10 from the same run's
  `BTC-USD/2026/10/01/16.rawl2` (221 records).

The first file's original header is retained. The records are grouped into
blocks of at most 8 MiB and compressed at Zstd level 19, with fresh sequential
block ordinals and CRCs, to reduce fixture size. No records are omitted between
the first and last included records. No timestamps or payloads are modified.
There is no closing index, so the fixture is an unsealed journal prefix.

Receive span: 2026-10-01 15:24:26.563331 through 16:00:10.258089 UTC.
The SHA-256 of the concatenated original serialized records is
`6fe300b023ed3e7f6b98b6a39fe5106cced98be6b92b1f135713860c0142ec1b`.
File size: 7,593,562 bytes. File SHA-256:
`42cee9ab39a9c8a7d65e9bdb3b0bb566a412a25ed5566f6de70ead93f20ee9b5`.

The regression rolls 15:00-16:00 UTC. The snapshot starts at 15:24; earlier
minutes remain unknown. It requires both 15:59 minute records, the partial
15:00 deep-hour record and a checkpoint through 16:00, then verifies that a
rerun writes zero columns and changes no HMC2/checkpoint bytes. Restoring the
receive-time stop condition must fail this regression because the recorder's
integration clock has not yet crossed the final lateness threshold.
