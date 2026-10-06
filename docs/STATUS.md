# Sentinel Status

Current facts only: replace old facts, do not append a journal. History is `git log --first-parent` and
`_agent/`. Rules: `AGENTS.md`. Update at landings, deploys, owner decisions and wave ends.

- **Updated:** 2026-10-06 02:10 EDT
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
| `lt-astra/complete-overflow` | `665c0ee` on `3aac4d0` | Keeps the inline toolbar and makes the right-edge `>>` a complete menu. Sol review PASS; full 96/96 and native PASS (both fonts, actual Metal) | Owner visual approval, then land |
| `lt-sol/retained-font-adoption` (W3b) | `e297743` on `3aac4d0` | Astra round 2 PASS (source only); native and full gates not run (held during the Oct 5 freeze) | After the owner releases the resource hold: native + full gate at `-j 2`, owner visual approval, land. Astra thread `01a10eeb-486c-7812-8c1e-db7f3f1754ce` |
| `lt-sol/settings-clarity` (W3c) | `b27f37d` on `5129d1c` | Review and full 95/95 PASS; held | After overflow lands: same author removes its toolbar/Controls explanation changes, then owner approves settings appearance. Do not land as-is |
| `lt-sol/label-style` | `d52921b` | Unrelated, locked | Leave alone |

Evidence (logs, screenshots, review reports) is in ignored `.claude/acting-orchestrator/`. A detached
`.claude/worktrees/youthful-ishizaka-b0e889` (`3eca515`) is not part of this work; leave it alone.

## Waiting on the owner

1. Overflow visual approval (`665c0ee`).
2. Settings appearance (W3c), after its reconciliation.
3. Release the resource hold (heavy builds, full tests, GUI runs).
4. S8: delete the legacy heatmap (Fable verdict GO).

## Owner constraints in force

- The owner is hardening `scripts/dev/build-queue.sh` and its tests (FM-195). Agents do not change them or
  sweep the owner's changes into commits.
- Resource hold from 2026-10-05 22:15 (desktop freeze) is still in force: no heavy builds, full tests or GUI runs until the owner releases it. The owner set `-j 2` for builds on 2026-10-06; that alone is not a release.

## Next (priority order)

1. Finish wave 3: land overflow, then retained fonts, then reconcile settings.
2. Slice D plan and high-risk review: widen roller serving to all 7 journal products. Cutover needs the owner
   present. Slice E deletions after D.
3. EATEN vs PULLED liquidity and ABSORPTION (owner's strongest direction): Fable plan after slice D. It crosses
   roller, HMC2, wire and renderer.
4. Backlog: owner GUI Agent API (17100) is unauthenticated; GUI RSS ~1.5 GB after 10 min; capture fan-out
   `poll()` error loop without sleep (`CaptureFanout.cpp:417`, low); W2d live GUI display and Windows runtime
   unverified; TPO history budget refusal in the GUI log; nightly backup of `/Volumes/T7/sentinel-data` when the
   owner's new HDD arrives; internal disk space.
