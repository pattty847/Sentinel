# Acting orchestrator hand-off (Claude -> Codex, 2026-10-04)

Claude's weekly limit is nearly spent (resets 2026-10-06 00:00 EDT). Codex conducts until the owner hands back.
Read in this order: `AGENTS.md` (rules), `docs/AGENT_WORKFLOW.md` (the loop), `docs/STATUS.md` (live state),
`docs/research/2026-10-widget-pass-directive.md` + `-plan.md` (current work). Then this file.

## Your role

Act exactly as the orchestrator in AGENTS.md section 10: plan, dispatch lieutenants in their own worktrees, route
every change through a review by a DIFFERENT vendor, land with `scripts/dev/agent-worktree.sh land`, push main
after a secret scan (never force), keep `docs/STATUS.md` current after every landing or deploy.

- **Lieutenants (writers):** your own Codex subagents/threads, one worktree each
  (`scripts/dev/agent-worktree.sh create lt-sol/<name>`). They build/test through `scripts/dev/build-queue.sh`.
- **Reviewers:** Claude, so review stays cross-vendor. A separate Fable allowance is unproven: check
  `scripts/dev/budget.sh` before and after each review; if Claude weekly remaining falls, stop Claude reviews
  and tell the owner before any further landing. From a shell:
  `cd <worktree> && claude -p --model fable "<read-only review prompt: commit, spec, what to check, VERDICT format>"`.
  Verify the alias with a one-line test call first; if `fable` is not accepted, ask the owner. Never let a Codex
  model be the only review of Codex-written code that touches the recorder, capture or the GPU heatmap.
- **Eyes:** after `lt-sol/dock-infra` lands and the GUI host restarts, a lieutenant can run its own branch:
  build it, then `scripts/dev/gui-shot.sh launch --build <worktree>`, drive it with `gui-shot.sh api ...`, and
  `gui-shot.sh shot <name>` (targets: heatmap, each dock, `window`; never `main`).
- **Budget:** `scripts/dev/budget.sh` before each dispatch batch. Tell the owner when Codex is near 0 (they hold
  reset credits).

## Hard rules (repeat of AGENTS.md, the ones that bite)

- Services: only `scripts/dev/deploy-runtime.sh server|capture`, owner at the Mac, one at a time. Never run a bare
  `sentinel-server`. Never delete data under `data/` or `/Volumes/T7`.
- Never commit secrets (ntfy topic lives only in `ops/monitoring/ntfy.env`), never `.codex/`.
- Owner decides behaviour, data and looks; you decide the rest with a stated default. At most 2-3 items waiting
  on the owner, one digest. Every lieutenant report ends with a `WORKFLOW:` line.
- Lieutenants never push or merge; only you land, one branch at a time, retest the others after each landing.

## In flight at hand-off

1. `lt-astra/dom` (W1a): LANDED `3c9b948`. Wave 1 is complete. Next: the server per-product tick slice, then wave 2 (see "Next work").
2. `lt-sol/dock-infra` (W1c): LANDED `af09b9f`, trust note added, main rebuilt, host restarted; `--build` and `target=window` verified live 2026-10-04.
3. Shadow roller soak (slice C): running since 2026-10-03 22:14. First comparison covered 27 h with 0 mismatches.
   Check `curl -s 127.0.0.1:8090/metrics | rg sentinel_roller_shadow` daily. At ~2026-10-05 22:15, if
   `sentinel_roller_shadow_mismatch_total` is still 0: propose slice D (cutover) to the owner; D must widen the
   roller to all 7 journal products. Do not cut over without the owner.

## Next work (owner-approved, in order)

1. Server slice: per-product live book tick in `ServerDataModel` (`ServerDataModel.cpp:538` applies the global 0.1;
   low-price books collapse) using the roller near-grid rule (~1 bp rounded 1-2-5, never below the quote
   increment); publish the tick per product so the GUI replica uses it. Recorder-process change: Fable review,
   deploy with the owner.
2. Widget pass wave 2 (plan file; owner correction Oct 4: after tick slice and owner-applied Codex reset credit, not Claude reset; at most three writing lieutenants, W2b alone owns hubs): W2a health model + StatusBar + telemetry; W2b toolbar + chart shell (owns the
   hub files that wave); W2c watch rail + screener; W2d stock chart + SEC. Wave 3: theme/fonts + visual QA.
3. Owner's strongest product direction: EATEN vs PULLED liquidity and ABSORPTION on the chart. After slice D the
   roller computes per-cell traded buy/sell, pulled (size removed with no trade), added, from the raw journal, as a
   new HMC2 layer that can be backfilled. Get a plan written (Fable) before code; it crosses roller, HMC2, wire
   and renderer.
4. Small: paper ticket Buy/Sell Limit should disable (with tooltip) when no limit is set; internal Mac disk has
   ~6 GB free (check `build/` usage); nightly backup of `/Volumes/T7/sentinel-data` to the owner's new 10 TB HDD
   when it arrives.

## Handing back

When the owner says Claude is back: make sure `docs/STATUS.md` reflects everything, list anything half-done
here, and stop dispatching.

## Owner interim review override (2026-10-04, after live tick deployment)

Claude weekly fell 3% -> 2% across Fable review; Claude calls are stopped. The owner explicitly authorized different GPT model reviews until Claude returns: Sol reviews Astra-authored branches, Astra reviews Sol-authored branches. Codex reset credit is visible at 100% remaining. Tick server deployed `c5f3dbe`, four live DOM products validated; current details and recording-format limitation are in STATUS.md. First three wave 2 writers are active, W2b alone owns hubs. Preserve queued validation, visual owner approval, serial landings/pushes, and separate owner-present deployment/slice D gates.

## Acting-orchestrator progress (2026-10-05)

The historical in-flight list above is superseded by `docs/STATUS.md`. Per-product tick and paper-limit guard landed and validated; owner-authorized recorder deployment is `c5f3dbe`, capture unchanged. Wave2 A/C/B/D all landed and pushed after owner approval, different-model reviews under the interim override, and serial full landing gates. Main GUI rebuilt and live BTC workstation inspected. One-candle AAPL research capture was synthetic; main live backend returned1,255 valid five-year daily candles, but end-to-end live GUI fetch and Windows runtime remain unverified.

Half-done at this checkpoint: W3a `lt-astra/font-foundation` (`1dd7454`) and W3c `lt-sol/settings-clarity` (`b27f37d`) are READY in separate T7 worktrees, reviewed by different GPT models, final queued full96/95-target suites PASS. Native captures and logs are preserved under main's ignored `.claude/acting-orchestrator/{visuals,wave3-verification}`. Both await one owner visual-approval digest; no W3 code landed. Settings review's TPO session-only-label finding was fixed and same reviewer passed. Root commits/rebases writers; standard serial land gate remains mandatory after approval. W3b retained-widget adoption waits for landed font foundation. Fresh Mac picker still offers curatedRobotoMono; second-font proof seeds a saved installedHelvetica preference, not a new catalog.

Claude calls remain stopped (weekly2% last checked); owner explicitly authorized different GPT model reviews while Claude is away. At most3 writing lieutenants, one hub owner per wave; W3a owns shared font/theme/hub scope, W3c stays in settings/toolbar/existing tests. No deploy/cutover/S8 approval carried forward. BTC shadow still zero mismatches/failures at13:48, primaryinvalidations25 (notzero), queue/diskerrors0;48h checkpoint~22:15 tonight. Cutover must widen to all7 journalproducts and requires owner present. When owner hands orchestration back to Claude, stop new dispatches and refresh STATUS with any newer state.
