# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends.

- **Updated:** 2026-10-06 03:50 EDT
- **Conductor:** Claude Opus 5.5. The owner handed conducting back on 2026-10-06; Codex does not dispatch.
- **Review policy:** 1 + 1 cross-provider (AGENTS.md section 10). No fallback active: the 2026-10-04
  GPT-reviews-GPT override ended when Claude usage reset.
- **Budget (02:00):** Claude weekly 100% left; Codex weekly 60% left, resets in ~6 days.

## Resume (fresh conductor)

1. Read `AGENTS.md` and this file. See what landed since: `git log --oneline --first-parent <last STATUS commit>..main`.
2. `scripts/dev/budget.sh`; service checks in `docs/AGENT_WORKFLOW.md` "Data and performance".
3. `git worktree list`, `scripts/dev/build-queue.sh status`, `curl -s 127.0.0.1:17190/status`.
4. Resume the threads listed under In flight; do not restart finished work.

## Services (owner's Mac, launchd)

- **Recorder** deployed 2026-10-04 23:11 from `c5f3dbe` (per-product book tick). Shadow roller ON for BTC-USD,
  writing `/Volumes/T7/sentinel-data/hmc2`. The 48 h soak passed (2026-10-05 22:17). At 2026-10-06 01:55:
  near/deep mismatches 0, setup/comparison failures 0, lag 0.9 s; primary queue drops and disk errors 0.
  Global invalidations are 30 and cumulative; they are not a parity signal.
  Rollback: `scripts/dev/deploy-runtime.sh rollback server`, or `roller_shadow.enabled: false` and redeploy.
- **Known limit:** the legacy cent-scale primary format rejects prices below $0.005 (PEPE, low DOGE levels).
  Slice D must serve the product-aware roller grids, not this format. Live aggregation uses a fixed band: an
  out-of-band best price invalidates it until the next upstream snapshot (recording continues).
- **Capture** deployed 2026-10-03 22:13; 7 products, all feeds up at 01:55. Rollback:
  `scripts/dev/deploy-runtime.sh rollback capture`.
- **GUI host** :17190 idle, no session. **Monitoring:** VictoriaMetrics :8428, Grafana :3000.

## In flight (T7 worktrees)

| Branch | Tip | State | Next |
|---|---|---|---|
| `lt-sol/retained-font-adoption` (W3b) | `e3a6684` on `a489126` (rebased by conductor, clean) | Astra round 2 PASS (source only). Overnight: queued build and native font + watch-rail tests with screenshots | Owner visual approval, then `land` |
| `lt-sol/settings-clarity` (W3c) | `76eb38b` on `a489126` | Reviewed settings work moved onto main; obsolete toolbar/Controls text dropped (overflow redesign replaced it). Owner said settings look good and asked for hover explanations: gpt-6.1-sol adding tooltips, thread `01a1103a-cdfc-7690-884f-f2d41abda7b2` | Claude review, owner reads tooltip table, `land` |
| `lt-sol/label-style` | `d52921b` | Unrelated, locked | Leave alone |

Evidence (logs, screenshots, review reports) is in ignored `.claude/acting-orchestrator/`. A detached
`.claude/worktrees/youthful-ishizaka-b0e889` (`3eca515`) is not part of this work; leave it alone.

## Waiting on the owner

1. Settings appearance (W3c), after its reconciliation.
2. Retained fonts (W3b) visual approval, after its native fixture run.
3. S8: delete the legacy heatmap (Fable verdict GO).

## Owner constraints in force

- The owner is hardening `scripts/dev/build-queue.sh` and its tests (FM-195). Agents do not change them or
  sweep the owner's changes into commits.
- The 2026-10-05 resource hold was released by the owner on 2026-10-06. Builds stay at `-j 2`; no automated GUI windows while the owner is working.

## Next (priority order)

1. Finish wave 3. The overflow redesign landed as `66e39b2` (2026-10-06: owner approved; native chart UI 42/42; full suite 96/96). Next: retained fonts (native fixture, owner visual, `land`); then settings W3c, whose same author must drop its now-obsolete toolbar/Controls explanation changes and rebase through `land`.
2. Slice D: draft packet and cutover runbook in `docs/research/2026-10-one-world-pipeline.md` "Slice D task packet" (Fable, 2026-10-06), awaiting the owner's 3 answers in its section 7. Then dispatch, high-risk review: widen roller serving to all 7 journal products. Cutover needs the owner
   present. Slice E deletions after D.
3. EATEN vs PULLED liquidity and ABSORPTION (owner's strongest direction): Fable plan after slice D. It crosses
   roller, HMC2, wire and renderer.
4. On hold (owner, 2026-10-06): TPO session choices become Session (draws New York, London, Asia and Australia together, like ExoCharts), 24H, 1W, 1M. Changes what the TPO draws; needs its own spec.
5. Backlog: tick-size combo text clips at its fixed 84 px width ("$0.15 (unavailable)", pre-existing); owner GUI Agent API (17100) is unauthenticated; GUI RSS ~1.5 GB after 10 min; capture fan-out
   `poll()` error loop without sleep (`CaptureFanout.cpp:417`, low); W2d live GUI display and Windows runtime
   unverified; TPO history budget refusal in the GUI log; nightly backup of `/Volumes/T7/sentinel-data` when the
   owner's new HDD arrives; internal disk space.
