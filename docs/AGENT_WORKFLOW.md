# Agent Workflow (how Sentinel is built)

The working loop the owner and the orchestrator use, as practiced since 2026-09-27.
The rules agents must follow live in `AGENTS.md` section 10; this file explains the loop
around them. Durable session state (current sprint, open decisions) is in the
orchestrator's Claude Code memory (`sentinel-roadmap`, `codex-delegation`,
`role-ceo-orchestrator`), which every Claude session in this repo loads.

## Roles

- **Owner** (solo developer): product direction, visual judgement, every decision that
  changes behaviour or data, push to GitHub, permission prompts on the Mac.
- **Orchestrator** (one Claude Code session, "Main Sentinel Work"): plans, dispatches,
  reviews, commits for sandboxed agents, lands, deploys the always-on services, keeps
  `_agent/` and the roadmap current. Exactly one orchestrator per repo; forked
  sessions answer questions only.
- **Lieutenants** (write tasks, each in its own git worktree and branch):
  - Codex `gpt-6-astra` / `gpt-6-sol` (ChatGPT subscription): non-visual work: core,
    recorder, capture, protocol, data paths. Sandboxed: no GPU, no window server, cannot
    write the git index (the orchestrator commits for it).
  - Claude subagents `opus` / `sonnet` (Claude subscription): UI and visual work (they can
    run the GUI and read their own screenshots), plus anything Codex cannot verify.
- **Reviewers** (read-only): always a different vendor from the author (Claude reviews
  Codex, Codex reviews Claude).
- **Claude Fable**: where a miss is expensive: final review for the always-on recorder and
  the GPU heatmap core, large plans (S6, per-symbol connections, observability), data
  forensics (FM-139), visual A/B QA.

## The loop

1. **Discuss.** The owner raises a goal or a problem. The orchestrator investigates (or
   dispatches a read-only investigation) and comes back with the cause and options.
2. **Plan.** For anything cross-cutting, a plan doc in `docs/research/` (often written by
   Fable) with the real decisions listed. The owner answers; decisions are appended to the
   plan as "Owner decisions (approved)" and committed. No dispatch before agreement on
   anything that changes behaviour, data or UX.
3. **Dispatch.** A self-contained prompt: read AGENTS.md + the plan, worktree and branch,
   scope and non-scope, files another agent is touching, acceptance tests (each must fail
   without its fix), the build rule, the hand-off format (`READY:` / `BLOCKED:`).
4. **Review.** Cross-vendor review of `git diff main...HEAD`, findings ranked
   blocker/major/minor with a failure scenario. Fix rounds go back to the SAME agent
   thread (Codex `exec resume`, Claude `SendMessage`) so context is kept. Recorder and GPU
   core changes also get Fable's final review.
5. **Land.** `scripts/dev/agent-worktree.sh land <branch>`: rebase onto main, build, full
   ctest, range-diff gate, `--no-ff` merge, worktree removal. One branch at a time.
6. **Deploy** (always-on services only): `scripts/dev/deploy-runtime.sh server|capture|both`
   (signed copy, restart, verify writes within 60 s, auto-rollback), then watch the run
   logs against the reviewer's watch list.
7. **Record.** New failure modes and invariants go to `_agent/`; state goes to the
   orchestrator's roadmap memory; the owner gets a short report.

## Constraints that shape it

- 16 GB Mac: builds, tests and benchmarks go through `scripts/dev/build-queue.sh`;
  read-only reviews and planning overlap freely.
- Usage is spread across both subscriptions. Before dispatching, the orchestrator runs
  `scripts/dev/budget.sh` (CodexBar CLI: % left and reset per window) and routes work to the
  subscription with room; when one is near its limit the other takes more work. Fable has its
  own weekly limit (shown in the CodexBar app, not the CLI).
- Hot files (`MainWindowGpu.cpp`, `DataProcessor.cpp`, `MarketDataCoreEngine.cpp`,
  `UnifiedGridRenderer.cpp`) are never edited by two branches at once.
- Visual changes the owner has not seen do not become the default.
- Agents never touch the launchd recorder/capture, recordings, signing keys, or push.

## Known agent quirks

- Codex: always `< /dev/null`; pass `--add-dir <repo>/_agent` (it can then append to `_agent/` itself; verified 2026-10-02) but know that `.git` stays read-only in the sandbox even with `--add-dir <git-common-dir>`, so the orchestrator commits and rebases for it; resume needs
  `-m <model>` and `-c sandbox_workspace_write.writable_roots=[...]`; never put backticks
  in a double-quoted prompt (the shell eats them). Its sandbox cannot start the GUI, but it
  can take screenshots of landed work through the GUI host
  (`scripts/dev/gui-shot.sh`, AGENTS.md section 4b): the orchestrator keeps
  `scripts/dev/gui-host.py` running outside the sandbox and rebuilds main after a GUI change lands.
  The host runs only main's build, never a worktree build, because it executes with the owner's
  privileges; a Codex branch's own visuals come from a Claude subagent or after landing.
- Claude subagents: can launch the GUI on a separate `--api-port` with `--no-screener`;
  screenshots `target=heatmap` only; never run the GUI binary with `--help` (it starts a
  full GUI).

## Retro notes (folded from WORKFLOW: lines)

- 2026-10-02: two GUI-running agents were given the same Agent API port (17110) and the same scratchpad file names; one agent's calls drove the other's GUI. Give every GUI-running agent its own `--api-port` (17110 + n) and its own `scratchpad/<branch>/` directory in the dispatch prompt.
- 2026-10-02: `pgrep -x ninja` before a build is check-then-act; agents sometimes start builds together. Acceptable for now (owner declined a build lock).
- 2026-10-02: for anything that executes outside a sandbox, review prompts must ask about every runtime load of agent-writable code or config (QML from source dirs, plugin paths, config files, caches), not only the diff.
- 2026-10-02: freeze UX specs before dispatching UI work; zoom-autofit got two review rounds and was then replaced by the auto-scale spec.

## Owner decisions on the loop (2026-10-02)

- Decision split: the owner decides behaviour, data and how things look; everything else the orchestrator decides with a stated default the owner can override.
- One batched digest instead of a ping per slice; at most 2-3 slices wait on the owner at once, and the orchestrator stops dispatching owner-gated work at the cap.
- Every lieutenant report ends with a `WORKFLOW:` line; the orchestrator folds them into the retro notes above and runs a short retro every few days.
- The orchestrator pushes `main` itself after clean landings (secret scan, never force).
- 2026-10-03: a review agent ran a branch GUI without `--agent-host`, so it used the owner's QSettings (window/layout state may be saved on exit). Reviewers running their own GUI must pass `--agent-host <scratch dir>` (isolated settings) or use the GUI host.
