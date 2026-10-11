# Chart-core refactor: dispatch packets (conductor, 2026-10-11)

Conductor: the cloud Opus thread "Chart-core refactor". The Mac Claude session is the operator: it creates worktrees, runs Codex, builds through the queue, pushes writer branches to `origin` for review, and lands. Plan: `docs/research/2026-10-11-chart-core-architecture.md` (revision 2; section 6 owner decisions are final). Rules: `AGENTS.md`.

Every packet below shares these rules:
- Base: current `main` after `lt-sol/dead-code-1` and this docs branch have landed. Worktree via `scripts/dev/agent-worktree.sh create <branch>`.
- Writers never rebase, merge or push. Build and test through `scripts/dev/build-queue.sh ... -j 6`.
- Find code by function name; plan line numbers are at `180d3b7` and have moved.
- Hand-off per AGENTS.md section 10 (`READY:` / `BLOCKED:` + `WORKFLOW:` line).
- After READY the operator commits (if sandboxed), pushes the branch to `origin`, and tells the conductor the tip. The conductor reviews the pushed diff. Landing is `agent-worktree.sh land` (it does not rebase a branch whose merge-base is already `main`); for a branch that merged `main`, merge `main` again and land with `merge --no-ff`, never a rebase.
- Hot files (`UnifiedGridRenderer*.cpp`, `MainWindowGpu.cpp`): one branch at a time, in slice order.

---

## S0a: CPU view-trace golden + chart-health gate (writer: Codex `gpt-6-sol`; risk: low)

Branch `lt-sol/s0a-view-trace`. This merges plan slices S0a and S0f: the gate lands first so it guards every later slice.

Part 1, view trace: exactly plan section 4 "S0a: CPU view-trace golden". Files: `tests/render/test_chart_view_trace.cpp`, `tests/render/golden/view_trace.txt`, `tests/render/CMakeLists.txt` (target `ChartViewTraceTests`, label `render`). Reuse `configureFixture()` and the `UgrZoomCpu` setup pattern from `tests/render/test_ugr_gpu.cpp` (copy the minimum into the new file or a small shared fixture header; do not edit `test_ugr_gpu.cpp` cases). Use `setZoomClockForTest`, `followLiveForTest`, `glideRebasesForTest`, `rasterCameraNow`, `restCameraNow`. Doubles print with `%a`. `--bless` is an argv flag handled in the test's `main()`.
- Accept: 20 consecutive runs identical (`ctest -R ChartViewTrace --repeat until-fail:20`); locally changing `kZoomStepRatio` to 1.26 makes it fail (report, do not commit). If a scenario is not deterministic, fix the scenario (clock injection, settle wait), never loosen the comparison.

Part 2, `scripts/dev/chart-health.sh` (plan section 5.2, with these conductor decisions):
- Compares the branch against `git merge-base HEAD main`; no baseline file. Checks only files changed on the branch.
- Checks: (a) every `libs/gui/UnifiedGridRenderer*` file: line count may not grow vs merge-base; (b) `m_` field count in `UnifiedGridRenderer.h` may not grow (`rg -c '^\s+[^/(]*\bm_[a-zA-Z]'`); (c) every `libs/gui/chart/**` file <= 500 lines (warn at 400); (d) lizard on changed `.cpp/.hpp/.h`: a function new on the branch must be CCN <= 15, NLOC <= 60, params <= 6 (`ChartViewController*` CCN <= 10); an existing function may not rise above max(15, its merge-base CCN), matched by qualified name; (e) jscpd at a pinned exact version, `--min-lines 8 --min-tokens 60`, over `libs/gui/chart` and `libs/gui/UnifiedGridRenderer*.cpp`: fail on a clone that involves a changed file and does not exist at merge-base; (f) boundary greps from plan 5.2.
- Final caps (UGR `.cpp` <= 450, `.h` <= 350, `m_` <= 15) sit behind a `CHART_HEALTH_FINAL=1` switch that S14 turns on.
- Missing `lizard` or `npx`: exit 2 with the install command, never pass silently. Output: one line per breach (`chart-health: FAIL <check> <file>:<func> <old> -> <new>`), last line `chart-health: PASS` or `FAIL (<n>)`.
- `--self-test` mode builds two scratch commits in a temp worktree and proves: a no-op branch passes; adding one `if` to `gpuFitPriceWindow` fails (d).
- Wire into `scripts/dev/agent-worktree.sh land` right before the build step. A `--trailer 'Chart-Health-Waiver: <reason>'` on the land skips the failure (still prints it).
- Do not touch `AGENTS.md`: the conductor adds section 3a at landing after owner sign-off.
- Accept: `chart-health.sh` passes on main; `--self-test` passes; G guard set from plan section 4 passes.

## S0b: Metal frame-hash golden (writer: native Claude on the Mac; risk: low)

Branch `lt-claude/s0b-metal-golden`. Exactly plan section 4 "S0b", including the owner rules: Mac M4 + Metal key required, a missing baseline or scene fails, explicit field serialization (never raw struct bytes), settled frames only, PNG dump on mismatch, `--bless`. Starts after S0a lands (it reuses the fixture). Accept as in the plan (10 identical runs; candle-gap change fails scene 1; deleting a scene entry fails). Footprint scene 8: verify the fixture can feed it; if not, skip with the reason and say so in the hand-off.

## Core hygiene (writer: Codex `gpt-6-sol`; risk: low; parallel lane)

Branch `lt-sol/core-hygiene`. Files: `libs/core/trading/TradingEngine.cpp`, `libs/core/marketdata/model/LiveOrderBook.{hpp,cpp}`, `tests/trading/test_trading_core.cpp`, a new `tests/marketdata/test_live_order_book.cpp` (or the existing LOB test file if one exists), their CMake entries.
1. TradingEngine NaN/inf guard: the fill path (`applyFill`-style function near `cumulativeFilledQty < 0.0 || fillPrice <= 0.0`) lets NaN/inf through because comparisons with NaN are false. Reject non-finite `cumulativeFilledQty`, `fillPrice`, mark/last-trade price and limit price at every public entry that takes a price or quantity, with the same rejection path the existing negative checks use. Tests: each entry with NaN, +inf, -inf leaves orders, positions and PnL unchanged; each test fails without its guard.
2. Missing core tests: cover the public TradingEngine and LiveOrderBook entry points that have no test today (list them in the hand-off).
3. LiveOrderBook bid/ask dedupe (jscpd: 47-line self-clone): the bid and ask loops in the range/snapshot functions become one side-parameterised helper. Behaviour-neutral: write the pinning tests first on the old code (snapshot, range query, clamp at both ends, empty side), then refactor; same outputs, same iteration order. Remove the unused `SentinelLogging.hpp` include.
- Accept: `ctest -R 'Trading|LiveOrderBook|MarketData'` passes; the hand-off shows each new guard test failing without its guard.

## D-b2b: the engine leaves the server (writer: Codex `gpt-6-astra`; risk: high; parallel lane)

Branch `lt-astra/db2b`. Packet: `docs/research/2026-10-07-db2-packet.md` sections 0-3 and the D-b2b parts of the rest. Changes since it was written:
- D-b1 and anchors landed; the cutover is live (`recording.live_feed: journal`, `f15264f`). Line refs tagged `(db1)` are now on `main`, moved; find by name.
- Owner: no soaks. Keep `recording.live_feed: journal` in `config/server_config.yaml` (the loader stops reading it); slice E removes it (rollback trap, packet finding 5).
- No hot file is touched (packet section 3). Owns `ServerDataModel.*`, `SentinelStreamServer.*`, `SentinelServerApp.*`, `ConfigLoader.cpp`, `ConfigTypes.hpp`, the monitoring files and docs listed. S8b and slice E follow on the same files, strictly after it.
- Not in scope: deleting anything under `data/` (owner present, at the deploy), the deploy itself.
- Evidence for high risk: tests for startup refusal of `recording.source: primary`, journal-only feed up/down metric (`sentinel_live_feed_up`), subscribe refusal JSON. Never run a bare `sentinel-server` (AGENTS.md 4b).
- Review: Opus (conductor) with the evidence above.
