# BTC live-path compatibility fixture

`btc-prefix.rawl2` is an exact, read-only prefix (file header + 180 complete blocks)
of the public Coinbase journal:

`BTC-USD/2026/10/01/04.49ffaa57-d40e-4df1-bb61-7bfd31a6d36e.0.rawl2`

It covers roughly the first 185 seconds from receive time 2026-10-01 04:53:00 UTC.
The index and later blocks are deliberately excluded; the prefix is unsealed.
No credentials, private trades or owner GUI state are in the journal. File size:
1,029,019 bytes. SHA-256:
`dc9d9f8e34c774c2c64a7aab3eae419daa67bb90e59f60fb8e9d886aee17e115`.

`test_live_fixture.cpp` feeds shared parsed L2 into the legacy recorder API with an
injected local receive clock and deterministic 250 ms ticks. It drains after each
L2 frame to avoid testing artificial producer congestion. Output uses the deployed
BTC grids and size floor. Concatenated deep/near HMC2 files in lexical path order
have SHA-256:
`676013266163d8f4266c92df7e9126231d2d318264d0120a0e60a7a588b8e027`.

This golden was checked by separately compiling the unmodified pre-slice
BookRecorder and Hmc2Store source from pre-slice commit `d5161c7` under renamed classes,
then running this fixture against both baseline and changed implementations.
The temporary baseline target was removed after the comparison. This proves
compatibility for this input; the existing recorder/engine suites cover other
live-path behaviors. The roller's record-time ticks have a separate contract:
they change some TWAP codes even on this same input (see docs/ROLLER.md).
