# Sentinel multi-agent workflow audit — 2026-10-05 snapshot

**Status: frozen evidence (trimmed 2026-10-06).** The adopted policy lives in `AGENTS.md` (sections 0, 4, 4b, 8a, 10), `docs/AGENT_WORKFLOW.md` and `docs/STATUS.md`. Do not maintain this file. The full audit with accounting scripts and per-day/model/role tables was not committed; proposals for hooks, skills, agent definitions, `SESSION_LOG.md`, task-packet ledgers and validation-proof records were rejected (see Outcome).

Recommendations only. The sole repository artifact from this audit is this report; no configuration, hook, source, service, agent definition, or existing workflow document was changed. No agents were dispatched and no builds or tests were run. Analysis helpers and aggregates were kept in temporary local files. Private transcript text is excluded.

Repository evidence is anchored at `7b56b31` (2026-10-05 21:56 EDT), rather than the moving main branch. Usage covers **2026-09-27 00:00 through 2026-10-05 22:00 America/New_York**, excluding this audit's Codex thread. The CodexBar comparison was captured at 21:58 EDT. Other Sentinel work was still in flight at the anchor; this report does not grant permission to land it. Published sources were consulted on October 5; their live documentation is not an immutable historical archive.

The recommended design is one conductor, small independent work packages, an independent reviewer, and one integration queue. The strongest evidence for savings is oversized context replay and poorly labelled accounting. The strongest evidence for quality is concrete review findings plus native/live checks. Removing review indiscriminately would sacrifice a gate that is demonstrably useful.

## 1. How we work today

### Practised loop and evidence

The primary evidence is [AGENTS.md §10](../../AGENTS.md), [AGENT_WORKFLOW.md](../AGENT_WORKFLOW.md), [STATUS.md](../STATUS.md), `docs/HANDOFF.md` (deleted 2026-10-06; see `git show 7b56b31:docs/HANDOFF.md`), and `git log 7b56b31 --since=2026-09-27T00:00:00-04:00 --first-parent`. At that anchor, the interval contains **251 first-parent commits and 96 merge commits**. These are activity counts, not a count of unique completed features.

In practice:

1. The owner defines behaviour, data, and visual intent. A conductor investigates and freezes a cross-cutting plan where necessary.
2. Writers get scoped prompts and isolated T7 worktrees. Sol normally takes bounded implementations; Astra takes harder isolated work. Claude previously took visual work. Recent GUI-host capabilities let Codex verify its own compiled branches too.
3. Writers build and test through a FIFO queue because the Mac has 16 GB. Sandboxed Codex writers generally cannot modify the git index; the conductor checkpoints and rebases their work.
4. A different model/vendor reviews a frozen diff. Findings return to the same writer session. The same reviewer commonly checks closure. Recorder/GPU work has received additional Fable review.
5. Branches enter a serial rebase/build/full-ctest/range-diff/merge/remove loop. Integrated previews and native screenshots supply evidence beyond unit tests. The owner approves visual defaults.
6. A separately authorized conductor deploys services with the signed runtime deployment script and verifies logs/metrics. Writers do not own recorder deployment.
7. STATUS, HANDOFF, research plans, private Claude memory, and `_agent/` preserve portions of the outcome. Their overlap creates contradictory instructions.

This is already more disciplined than a free-form swarm. Worktree isolation, a build queue, hash-specific reviews, different-model checks, native fixtures, owner approval, and signed rollback have all been used. The problems are concentrated in context growth, duplicate state, missing attribution, and integration seams.

### Real incidents: successful gates, escapes, and rework

The entries below summarize technical evidence; they do not reproduce conversations. Saved review artifacts are ignored local files, so the commit IDs and canonical documents provide the portable anchors.

| Incident / anchor | What the evidence establishes | Workflow implication |
| --- | --- | --- |
| W2a health, `1009475` → `43a964e`, landed `2ca2a4b` | Independent review blocked false freshness from expired or rejected GPU frames, lost reconnect ages, stale recovery, and paper detachment. Closure review checked accepted-publication identity and lifecycle regressions. Local evidence: `.claude/acting-orchestrator/wave2-health-{review,fix-review}-last.txt`. | Review must trace the accepted data path and reconnect/release lifecycle, rather than inspect only the display model. |
| W2d stock/SEC, `79f8a0d` → `d155d23`, landed `da260b4` | Review caught stock-side crypto requests reaching Python, financial parsing that expected a different provider schema, and hover-date handling. Eight synthetic states supported closure; live backend returned 1,255 candles, but end-to-end live GUI and Windows remained unverified. | Contract samples and live-provider boundaries belong in acceptance evidence. Synthetic success is not live success. |
| W2b toolbar, standalone `274e46f` PASS; integration `d6cbe91` BLOCK → `b10001c` PASS, landed `43b39cd` | Standalone approval explicitly left hub integration open. Integration review caught renderer facts attributed to the wrong snapshot and incoherent publication, plus refusal handling. | A standalone PASS cannot approve a later integration patch. This is a scope gap closed by a second gate, not proof that the first reviewer missed an in-scope defect. |
| W2c screener, `b0c036b` → `8451d91` → `2153842`, later `14abeef` → `4d63a1f`, landed `710d492` | Reviews caught stale sort order, update/activation problems, covered-tab refresh, horizontal context, and a later fix that replaced row-content width with a header-only hint. The sizing-fix review passed with a refresh regression. | Fix rounds can introduce new bugs. Check the changed seam and nearby lifecycle/resize states on each fix. |
| Slice C `0a2e0bc` and slice B `2f45fff`; pre-enable correction `98e5b49` / `b372a33` | Previously reviewed/landed code still entered a ten-minute cooldown after short retry sequences and lost rings on poll/listener errors. The pre-enable correction added a two-minute failure floor and retained rings; targeted mutation evidence is documented in the one-world plan. | A genuine post-review escape. Static review and passing suites did not cover realistic short outages and socket failure behaviour. Add scenario coverage before enabling production changes. |
| Frozen healthy recorder, October 1; correction `02c2ba7`, follow-up `f27a532` | FM-139 records 5h34m of frozen healthy columns (03:52–09:26 EDT): unsubscribe named the remaining set and any product traffic refreshed the watchdog. Reviewed recorder self-heal work had landed at `4e34a45` the previous evening. | The system-level subscription/watchdog scenario escaped existing reviewed code. Evidence does not identify a particular reviewer as responsible or prove that their declared scope included it. |
| Metrics/feed integration `f27a532`; lifecycle integration `2b84256` | The merge subjects explicitly record a metrics conflict resolved per Fable and lifecycle-owned acquire/release after rebase. | Actual semantic integration collisions. A text-clean merge or equal branch patch does not prove integrated behaviour. |
| S6d `735b843`; live-edge correction `38ecf32` | Native A/B verification found the timeframe/span error FM-134 and the missing-live-source issue FM-135 after earlier GPU slices had received reviews. The first span fix itself read an already reclamped span. | Runtime experiments found defects missed by preceding review/test coverage. User-visible sequences belong in the acceptance matrix. |
| Trade bubbles `fa4d021`, fixes `bd90d35`, `e4cc3bf`, `0880b26` | Three Fable rounds were followed by landing fixes for scene-graph backend setup, quiet-frame testing, and empty-mesh dirtiness. | A reviewer cannot substitute for the combined native gate. Test-environment defects and performance behaviour can survive several code reviews. |
| GUI collision / settings side effect, AGENT_WORKFLOW retro Oct 2–3 | Two agents shared API port 17110 and scratch names; one drove the other GUI. A reviewer also ran a GUI without agent-host isolation and could save owner QSettings. | Runtime resources need explicit ownership, not merely separate source worktrees. Preserve the hosted GUI/session boundary. |
| Wave 3 settings `b27f37d`, font `5266aab`, new overflow `f09ab81` | Settings was reviewed and fully tested, then held after owner discussion changed the toolbar direction. Its toolbar/test scope overlaps the replacement overflow slice. | Freeze user-facing seams before expensive implementation, and invalidate superseded task scope explicitly. This is real stranded/rework exposure, not a merged conflict. |

The slice C acceptance and pre-enable evidence is in [the one-world pipeline plan](2026-10-one-world-pipeline.md), especially its review and pre-enable subsections. FM-139 and other `_agent/` entries are ignored local evidence, accessible here but not portable to a fresh clone. Git anchors above let a future auditor inspect the corresponding landed changes.

### Stale and contradictory state

- AGENT_WORKFLOW initially says durable state lives in Claude memory and that only Claude conducts; STATUS and later amendments say Codex conducts and repository files are the handoff source. The newer override is the practised policy.
- HANDOFF initially requires cross-vendor review and forbids GPT-only high-stakes review, then appends an owner override permitting different GPT models while Claude is unavailable. A new reader must resolve precedence across a long document.
- AGENT_WORKFLOW retains the old `pgrep ninja`/“owner declined a build lock” retro despite the FIFO queue installed at `4bc14d3`. The observation is historical but reads like an active operational exception.
- AGENTS routes visual writing to Claude because Codex cannot see the app, while the host now supports own-branch runs and safe window/dock captures. STATUS documents Codex-authored visual work. Capability is harness-dependent; provider identity alone is an outdated routing rule.
- HANDOFF's historical in-flight W3 list says no W3 code landed; later appendices correctly record font landing. STATUS at the anchor is newer. This duplicated state forces every session to reconcile history.
- Local FM-135 still says “open (S6d blocker)” although the follow-up landed at `38ecf32`. Durable scratchpad memory also needs retirement of resolved status text.
- AGENTS was **25,360 bytes** and STATUS **26,727 bytes** at the snapshot. A session-start design that prints both in full would recreate the overhead it is meant to remove.

The fix is a clear authority order, a compact current-state file, and an append-only historical wave log. Vendor handoff should change a current field, not create a second roadmap.


## 2. Token spend (summary)

Processed-token counters from local Claude and Codex logs, 2026-09-27 to 2026-10-05 22:00 EDT, Sentinel directories only. Cached replay is counted as processed work; these are not money paid or subscription percentages, and role labels are inferred. CodexBar is the chosen tracker going forward.

### Provider totals (exact recorded counters)

| Provider | Fresh input | Cache read | Cache write | Output | Total | Usage records |
| --- | --- | --- | --- | --- | --- | --- |
| claude | 35,168 | 2,812,565,015 | 56,456,368 | 8,991,958 | 2,878,048,509 | 8,252 |
| codex | 35,441,909 | 1,268,927,232 | 0 | 5,377,025 | 1,309,746,166 | 11,681 |

Codex additionally records **2,168,849 reasoning output tokens**, already included in output. The Claude table does not separately extract thinking-token detail. Combined recorded total: **4,187,794,675 tokens**.

### Redundant-spend candidates: what is measured, what is not

| Candidate | Measured evidence | Interpretation / limit |
| --- | --- | --- |
| Long-thread replay | Claude: 7,354 calls with ≥100k input; 2,801,132,126 input tokens in those calls; largest 946,045. Codex: 6,679 calls; 969,656,732 input tokens in that group; largest 249,962. | Direct measurement of large repeated contexts. The transcript does not identify which tokens were unnecessary; fresh-context A/B waves must measure avoidable replay. |
| Orchestration replay | Claude orchestrator/unallocated: 1,504,101,932 total; Codex: 258,921,113. | A large candidate bucket. It includes useful investigation, integration and owner interaction, not just dispatch overhead. |
| Repeated documentation reads | Deduplicated shell-call mentions: AGENTS 216; invariants 147; failure modes 137; STATUS 25; workflow 22; HANDOFF 5. | Reads can be targeted or necessary. These are command mentions, not counts of complete-file loads, unique bytes, or attributable token costs. Native Read tools and commands nested in other orchestration tools are not fully covered. |
| Review rounds with no new finding | Saved PASS examples: health layout, health fix, stock/SEC fix, C exposure fix, C sizing fix, and final toolbar fixture fix. Other reviews did find defects in fixes. | No finding is an outcome, not proof of redundant work. Most listed rounds close a real earlier finding or inspect a changed patch. No defensible “all no-finding review tokens wasted” total can be extracted. |
| Fix rounds | Explicitly classified Claude fix turns: 171,957,635 tokens; Codex: 85,201,246. | Measured counters, heuristic attribution. Preventable specification rework, necessary bug closure, and errors introduced by fixes are not reliably separable without a ledger. |
| Repeated builds/tests | 826 build-command mentions; 389 ctest-command mentions. W2 has individual serial landing suites plus a combined 95-target preview suite. W3 font had a pre-land 96-target suite and a 96-target landing suite. | Command mentions are not completed runs. Rebased/integrated trees can legitimately require retesting; transcript summaries cannot prove identical effective builds. Machine time is clearer than token savings here. |
| Review gate repeated after successful tests | agent-worktree.sh currently builds/tests before checking whether range-diff needs review, then a rerun builds/tests again. | A concrete script-level opportunity: inspect rebase changes before expensive validation, and reuse proof only for an identical validated tree/environment. No historical avoidable-run count is claimed. |

At the anchor, recorded full-suite wall times in STATUS include W2 A/C/B/D gates and the combined preview: C 471.55 s, B 479.30 s, D 433.76 s, combined 426.26 s. Those four alone occupy **30.18 minutes**, excluding A, builds, queue waits, and earlier rounds. Their different integration points make them useful evidence, not automatically duplicate work.

Budget guardrail: the first command was `scripts/dev/budget.sh`: Codex weekly **64% remaining**, Claude weekly **2%**, Claude session **100%**. A mid-audit check showed Codex **63%**, Claude weekly **2%**. Week percentages are coarse and account-wide, and the other Sentinel chat was active. A final observation is appended below; it cannot isolate this audit's depletion exactly.


## 3. Outside research: published practice and applicability

### Official documentation first

- **Claude Code subagents:** custom agents have narrow tools, separate context, model selection, and worktree options. Use a read-only tool set for reviewers; `permissionMode` alone can be overridden by the parent's permission mode. Native worktree isolation is available, but adopting it alongside Sentinel's T7 script would create two worktree lifecycles unless deliberately integrated. [Anthropic subagent documentation](https://code.claude.com/docs/en/sub-agents).
- **Claude cost control:** Anthropic recommends small teams, focused spawn prompts, clearing unrelated context, model selection by task, and preprocessing with hooks/skills. Caching reduces replay price but does not eliminate repeated-context usage. Its recommendation to reserve expensive models for complex work supports testing a lighter conductor, not declaring a cheaper model equivalent without evaluation. [Anthropic cost documentation](https://code.claude.com/docs/en/costs).
- **Claude context and handoff:** maintain useful tests and bounded tasks, separate exploration from execution, and preserve progress in files/git so a fresh session can recover. Skills should load task-specific instructions when invoked; lifecycle hooks can inject context. These support a wave-end checkpoint and a deterministic start script. [Best practices](https://code.claude.com/docs/en/best-practices), [skills](https://code.claude.com/docs/en/skills), [hooks](https://code.claude.com/docs/en/hooks).
- **Codex subagents:** current documentation describes custom TOML agents with explicit instructions and model/sandbox settings, plus an `[agents]` concurrency cap. Child permission inheritance and parent overrides matter. Independent reviews should receive a self-contained task rather than the entire conductor transcript. The app's built-in agents and headless CLI have different operational limits; use a known CLI fallback when the app cannot open another reviewer. [OpenAI subagent documentation](https://learn.chatgpt.com/docs/agent-configuration/subagents).
- **Codex skills and context:** repository skills live under `.agents/skills`, with progressive disclosure. OpenAI explicitly cautions against broadly triggered skills and mandatory stacks of pre-reads. Keep invariant enforcement in AGENTS, executable routine work in scripts, and optional workflows in narrowly described skills. [OpenAI skill documentation](https://learn.chatgpt.com/docs/build-skills), [OpenAI's September 2026 prompting/skills guidance](https://developers.openai.com/blog/rethinking-skills-and-prompts-for-gpt-6-astra).
- **Codex hooks:** hooks can live in user/project `hooks.json` or TOML. SessionStart can return bounded additional context. Non-managed hooks require review/trust; project hooks need a trusted project layer. Multiple sources merge. Commands are supported; prompt/agent hook handlers are parsed but skipped in the current documented runtime. A shared deterministic Python hook is a better portable choice here than a model-powered hook. [OpenAI hook documentation](https://learn.chatgpt.com/docs/hooks).

### Published engineering experience

Anthropic's research-system write-up uses an orchestrator with independent workers and emphasizes precise objectives, source/tool boundaries, output formats, and explicit resource budgets. It reports about 15× chat token usage for its multi-agent research system and warns that coding has fewer independent tasks. That number is a research-system observation, **not a multiplier measured for Sentinel**. Its lesson here is to delegate independent breadth, not multiply agents around a tightly coupled state machine. [Anthropic engineering](https://www.anthropic.com/engineering/multi-agent-research-system).

Anthropic's long-running-agent harness uses a progress file, git history, incremental features, and explicit verification so a new session can resume without reconstructing everything. Sentinel already has pieces of this; a single current STATUS and historical wave log would make them coherent. A file cannot prove an unperformed end-to-end test, so the evidence manifest must preserve limitations. [Anthropic long-running harnesses](https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents).

Cursor's engineering account describes failures of flat worker coordination and lock-heavy shared state, then a separation of planning and implementation with periodic fresh cycles. It also reports bottlenecks from excessive process and the value of removing complexity. This is vendor engineering experience at a much larger scale, not a controlled comparison of Claude versus Codex. Adopt narrow ownership and fresh cycles; do not copy hundreds of workers pushing one branch onto a solo-owner recorder repository. [Cursor scaling agents](https://cursor.com/blog/scaling-agents), [Cursor practical agent guidance](https://cursor.com/blog/agent-best-practices).

None of these sources establishes that “different vendor” is a measured quality guarantee, that Fable has an independent quota, or that larger RAM reduces token usage. Preserve independence through fresh evidence, actual failure scenarios, and scope-specific validation, then measure which reviewer finds useful defects.


## 4. Outcome (owner decisions, 2026-10-06)

Adopted:
- One conductor per repo, chosen by the owner: Claude Opus 5.5 by default, Codex when handed over.
- Per feature 1 + 1: one writer and one reviewer from the other provider; the same threads handle fix and closure rounds. High-risk work (recorder, capture, roller, recording format, GPU core) gets the other provider's strongest reviewer plus scenario/native evidence instead of an extra reviewer. Same-provider different-model review is a fallback only while one provider is out of usage.
- The conductor may redeploy reviewed, landed changes through `deploy-runtime.sh`; cutovers, format changes and data deletions stay owner-present.
- Builds at `-j 2` until the owner raises it.
- Writers run focused checks and do not rebase; the conductor's landing gate runs the full suite and does all Git work for sandboxed writers.
- Read-only tasks write nothing, including `_agent/` and docs.
- `docs/STATUS.md` holds current facts only (about 4 KB) and is replaced, not appended; `docs/HANDOFF.md` was folded into AGENTS and STATUS and deleted; history is `git log --first-parent` and `_agent/`.
- AGENTS.md keeps durable rules; the GUI-host manual, Codex CLI notes and lessons moved to `docs/AGENT_WORKFLOW.md`. Code tasks search `_agent/INVARIANTS.md` by area instead of reading it whole.
- `docs/research/*` plans are frozen after their work lands.

Rejected: SessionStart hooks, `session-context.py`, `.claude/agents` and `.codex/agents` reviewer definitions, wave skills, `SESSION_LOG.md`, `.agent-work/` packet ledger, custom usage tracking, tree-hash validation proof records, and routing routine conducting to cheaper models against the owner's preference.

Missed by the original audit and fixed in the same cleanup: Claude's private memory held a 65 KB roadmap and a stale start checklist that Codex could not see; section 0 made every code task read all of `_agent/INVARIANTS.md` (40 KB); several hard rules (no data deletion, no secrets or `.codex/`, owner presence, runtime-load review) existed only in HANDOFF or retro notes; `-j 4` examples contradicted the post-freeze `-j 2`.

Optional later: move the `agent-worktree.sh land` range-diff check before its build/test step.
