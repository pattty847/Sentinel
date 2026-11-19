# Recorder compatibility fixtures

Captured on macOS arm64 from production sources at `61ae1b08`, before the bounded
recorder implementation. Only the new `RecorderTest.BaseCompatibilityBytes`
capture harness was added to the base test translation unit. The queued
`mac-clang` build of `test_book_recorder` and queued filtered test produced these
files in scratch; they were copied here without modifying the bytes.

Input: the existing RecorderTest config (BTC-USD, near, scale 100, tick 100,
relative window 0.5..2, default size codec, lateness 0); snapshot at epoch
2000-01-01 with bid 99 size 2 and ask 101 size 4; update bid size to 6 at +20s;
tick +60s; remove ask at +75s; tick +90s; export state and copy the minute file.
The committed test repeats that input and compares every byte, then imports the
base state into a fresh recorder and checks continued state against the original.
There is no fixture-blessing environment variable or automatic rewrite path.

- Config hash: `18191379077652817817`.
- `recorder-61ae1b08.hmc2` SHA-256:
  `581cbadeea6776e388b80ddf93c7462a49f35e3c671ef6b5a7bebdbcf40115f4`.
- `recorder-61ae1b08-arm64.state` SHA-256:
  `cc79ad68d7576c7e3be59d9d91b72dedb448bedf984e71438925660162c1f54b`.

The state fixture uses arm64 long double (53 mantissa bits, exponent limit 1024).
Other representations must reject that fixture under the existing state contract;
its byte pin and successful import are checked on matching representations. HMC2
and config-hash checks run on all platforms. Compression bytes were captured with
the project's linked zstd; a future codec/toolchain change needs explicit review.
