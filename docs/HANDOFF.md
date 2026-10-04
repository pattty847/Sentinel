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
- **Reviewers:** Claude, so review stays cross-vendor. Claude Fable has its own weekly limit. From a shell:
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

1. `lt-astra/dom` (W1a), worktree `/Volumes/T7/sentinel-worktrees/lt-astra-dom`, Codex thread
   `01a104bb-9a13-74c3-bb45-e24e7968e396`. Round 4 running: use the server tick for every product (round 3's
   `max(server, derived)` coarsened ETH for the book-top line and Agent API), keep the 1% "too coarse" message,
   revert `libs/core/roller/Grid.*` if unused. Next: commit its changes, Claude Fable confirms round 4, land.
   Done markers: `/private/tmp/claude-501/-Users-copeharder-Programming-Sentinel/2aedf6e1-a9df-4ef2-9ff7-eee9dcaf6d58/scratchpad/w1a-fix3.{done,last}`.
2. `lt-sol/dock-infra` (W1c): Fable LAND; `land` was running at hand-off. If it is not in `git log`, rerun
   `scripts/dev/agent-worktree.sh land lt-sol/dock-infra`. After it lands: (a) add one sentence to AGENTS.md 4b:
   for `--build` runs the in-GUI guardrails (AgentHostMode, RemoteGridDataSource send checks) live in the
   agent-compiled binary and are not host guarantees; the host-enforced boundary is path containment, fixed argv,
   allowlisted env, one session, idle timeout; (b) rebuild main through the build queue; (c) restart the host
   (`pkill -TERM -f gui-host.py; nohup scripts/dev/gui-host.py >/dev/null 2>&1 & disown`); (d) live-check
   `launch --build` on a worktree and a `target=window` shot.
3. Shadow roller soak (slice C): running since 2026-10-03 22:14. First comparison covered 27 h with 0 mismatches.
   Check `curl -s 127.0.0.1:8090/metrics | rg sentinel_roller_shadow` daily. At ~2026-10-05 22:15, if
   `sentinel_roller_shadow_mismatch_total` is still 0: propose slice D (cutover) to the owner; D must widen the
   roller to all 7 journal products. Do not cut over without the owner.

## Next work (owner-approved, in order)

1. Server slice: per-product live book tick in `ServerDataModel` (`ServerDataModel.cpp:538` applies the global 0.1;
   low-price books collapse) using the roller near-grid rule (~1 bp rounded 1-2-5, never below the quote
   increment); publish the tick per product so the GUI replica uses it. Recorder-process change: Fable review,
   deploy with the owner.
2. Widget pass wave 2 (plan file): W2a health model + StatusBar + telemetry; W2b toolbar + chart shell (owns the
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
