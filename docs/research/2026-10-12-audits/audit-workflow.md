**Verdict: keep independent review, shorten conductor sessions, and repair the evidence hand-off.** Review repeatedly found consequential defects that passing tests did not cover. The waste is concentrated in long-lived context, repeated status checks, incomplete contracts, and coordination that fails to preserve its own evidence.

**The claim that conductors are the largest token cost is not supported by the available cross-provider records.** Writers account for approximately **55%**, conductors/operators **39%**, and reviewers **6%** of recorded token traffic. Conductors are the largest category within the local Claude records alone. Missing cloud Projects usage prevents a definitive whole-system cost comparison.

This audit made no edits and ran no builds, tests, GUIs, or services. Findings below distinguish historical evidence from independently reproduced results.

**Evidence coverage and limits**

The requested `git log --first-parent main --since=2026-09-12` returned **463 commits: 162 merges and 301 direct commits**. File inspection classified the direct commits as **248 documentation-only and 53 code/config/tooling/other**. Direct changes include deployment tooling and recording-source cutovers; direct does not automatically mean unreviewed or unauthorized.

Across the 162 merges, **54 have Slice/Writer/Reviewer fields, 53 Risk, 48 Review-Rounds, 49 Findings, 33 Deferred, 51 Evidence, and 15 all three token fields**. Historical rules matter: the consolidated 1+1 protocol landed October 6 (`0a86b3b`), and token trailers were added in `30be12c`. **Of 60 subsequent merges, 45 lack token trailers.** Even populated trailers are not consistently comparable: `f5b8acf` records cumulative writer usage, approximately 390k reviewer context “at the last run,” and planner tokens as zero despite conductor packet work.

I inspected review reports and closure reports under `.claude/acting-orchestrator`, relevant failure modes/decisions, STATUS history, session usage, and the panel implementation. **There is no complete finding-to-fix ledger or complete retained landing-attempt history.** An exact month-wide unique-bug count, false-positive rate, or waiting-time total would be invented.

**1. What review actually caught**

The **48 numerically populated Findings trailers total 1 blocker, 53 majors, and 104 minors**; slice E’s additional Findings field explicitly says its counts were not recorded. These are reported findings, including repeat findings and documentation issues—not 54 distinct production defects.

| Slice / merge | Recorded blockers / majors | Assessment |
|---|---:|---|
| Raster camera A, `bdbc293` | 0 / 8 | Concrete cursor jumps, inconsistent mappings, fitting omissions, and rounding failures across three reports. |
| Smooth zoom A2, `302c0ef` | 0 / 13 | Four reports contain 6, 4, 2, then 1 major. Includes recurring root causes and bugs exposed by fixes. |
| Zoom tuning A3, `f5b8acf` | 0 / 3 | Fable found live candle revisions invalidating the supposedly stable zoom solution. |
| Journal backtest D-b2a, `9fc32ac` | 0 / 4 | Actual-data checks found duplicate trades and reversed within-frame ordering; corruption handling also needed correction. |
| Corrupt-block skip, `e8c416a` | 0 / 2 | Fable found damage classes still able to stall the roller and an alert that would not resolve. |
| GUI reconnect / VP rendering | 0 / 2 each | `b087329`, `2f4a718`; integration and visual defects, with conductor correction also needed on VP appearance. |
| Candle history / pixel geometry / wheel input | 0 / 1 each | `533420c`, `902badf`, `b3b2b13`; Fable-reviewed fixes, including real macOS event classification. |
| README, `24e3020` | 1 / 2 | The blocker concerned the executable-name mismatch; this is not equivalent in severity to recorder corruption. |
| Chart S0a+S0f / S2 / S14 / S15 | 0 / 4, 1, 1, 1 | Gate/test/controller/input/startup issues reported in the respective merge trailers. |
| One-world D-b2b / S8b | 0 / 2, 1 | S8b’s major was accepted operationally through a coordinated land/deploy. |
| SentinelBar v1 / v1.1 | 0 / 3, 1 | Additional evidence that ordinary conductor review found substantive issues. |
| Other numerically stamped slices | 0 / 0 | Includes chart S0b–S1, S3–S13, shadow-incremental, hygiene, palette, cancellation, bind-address, and candle-width work; several had minors. |

Older reports add important catches outside that trailer total:

- **D-a:** three initial majors, three follow-up majors, then one landing blocker. Review found withdrawn provisional data still reaching subscribers, deployment “readiness” before successful recording, and split-config exclusion bypasses. Later reports exposed incomplete fixes. The landing blocker was an existing deployment test omitted from targeted validation. [D-a reports](/Users/copeharder/Programming/Sentinel/.claude/acting-orchestrator/d-a/review-astra-r1.md)
- **D-b1:** two initial majors; the candle issue survived the first fix, and two later reports rejected inaccurate lag measurements. The decisive correction was changing candle finalization to the delivered journal clock. [D-b1 reports](/Users/copeharder/Programming/Sentinel/.claude/acting-orchestrator/db1/review-r1.md)
- **Acceptor restart:** two majors—an unconditional POSIX include broke Windows compilation, and session registration could race shutdown. [Acceptor review](/Users/copeharder/Programming/Sentinel/.claude/acting-orchestrator/acceptor/review-r1.md)

A conservative inspected subset contains **at least 19 substantive bug/coverage gaps**: D-a’s first 3, D-b1’s first 2, raster A’s first 4, A2’s first 6, A3’s first 2, and backtest duplicates/order. The reports give executable scenarios, arithmetic counterexamples, or actual-data evidence and explain why the supplied fixtures missed them. **I did not rerun those historical reproductions.** Follow-up reports substantiate closure for many; some fixes changed the contract or removed the affected path.

Noise is less well measured. The clearest quantified example is the separate outside heatmap review: **of nine checked claim groups, one held fully, five partly, and three were false**—GPU buffer race, metatype delivery loss, and purported dead helpers. That is **not** a false-positive rate for 1+1 review. [Verification report](/Users/copeharder/Programming/Sentinel/.claude/acting-orchestrator/glm-packet/verify.md)

**Fable earned its place on high-risk work, but “only Fable would catch this” is unproven.** Its corruption and zoom findings were concrete. However, Astra caught comparably difficult lifecycle, deployment, and animation defects; Sonnet’s backtest review measured actual behavioral differences. There was no controlled same-tip comparison. Recent chart high-risk reviews S3/S10/S11/S12/S15 report **1 major and 18 minors combined**; S12 also fixed the potentially dangerous teardown callback described in FM-270. Severity labels alone therefore understate some value.

Review also missed things: FM-139 records **5h34m of frozen healthy recorder output**, and FM-134/135 document defects found through native A/B runs after review. Preserve scenario and Metal evidence. [Failure modes](/Users/copeharder/Programming/Sentinel/_agent/FAILURE_MODES.md)

**2. Token cost and wasted work**

These are **token traffic totals, including cached input**, not dollars, subscription percentages, or unique generated tokens.

| Estimated role | Claude local records | Codex rollouts | Combined | Share |
|---|---:|---:|---:|---:|
| Conductor/operator/interactive | 2,505.9M | 136.8M | **2,642.7M** | **39.3%** |
| Writers/other implementation | 1,887.9M | 1,790.9M | **3,678.8M** | **54.7%** |
| Reviewers/audits | 276.8M | 123.4M | **400.2M** | **6.0%** |

Source: September 12–October 12 events in `~/.codex/sessions` and `~/.claude/projects/-Users-copeharder-Programming-Sentinel`, including its subagent transcripts. Codex uses increments of cumulative `token_count` counters, not their sum. Claude usage is deduplicated by message/request identity, using iteration usage where provided instead of adding it again to outer usage. Role estimates use prompts, session origin, subagent placement, and available review-file thread IDs; mixed-role sessions remain approximate.

Excluding cache-read tokens, the same categories are approximately **41.6M conductor, 104.6M writer, and 37.1M reviewer tokens**. This still does not establish dollar costs across models. Cloud Projects usage and Claude sessions stored outside this project directory are missing; roughly **1.04B additional conductor token traffic** would reverse the observed conductor-versus-writer ordering.

The largest inspected conductor illustrates the problem:

- [`fcbde983…jsonl`](/Users/copeharder/.claude/projects/-Users-copeharder-Programming-Sentinel/fcbde983-01d0-44b5-ae36-e96e5549ad5b.jsonl), October 6–11: **570.8M tokens**, including **561.1M cached input—98.3%**.
- Across **1,281 recorded usage iterations**, input context was **414k median, 780k p90, 940k maximum**.
- Its **853 Bash calls** included **389 poll/wait-like commands** and **400 read/search-like commands**. These lexical categories include useful log reads; they are not 789 proven wasted calls.
- Recorded tool-result payloads included about **1.27M serialized characters from Read**, **0.92M from Bash**, and **23k from Agent**. Read included images, so those character counts are not text-token measurements.

**Repeated long context is the dominant measured cost.** Polling and broad outputs repeatedly carry that context forward. Re-reading files adds to it, but shell-based reads and image payloads prevent a trustworthy causal percentage split. Direct subagent-result payloads were much smaller in this session; attributing the bulk of cost to them is unsupported.

Other avoidable costs have concrete evidence:

- **Fix loops:** A2 produced 13 major reports across four rounds; A3 required four rounds and three BLOCKED responses. D-b1 separately redesigned candle timing and lag measurement after point fixes failed. Diagnose the governing contract earlier.
- **Landing churn:** D-a reached an omitted existing test, then a **96/97** gate with an acceptor hang. FM-202 records ctest processing output at **860 MB RSS**. These were validation-scope and infrastructure defects, not reasons to weaken review.
- **Load-sensitive tests:** FM-262’s 15-second deadline sat close to normal runtime. Later evidence reports **25.9 seconds alone versus 80–120 seconds under parallel load**, followed by a timeout increase.
- **Dispatch friction:** sandbox ccache and vcpkg failures recur in FM-253/255; conductor restarts kill background runs, and some subagent types cannot resume through another conductor. [Workflow manual](/Users/copeharder/Programming/Sentinel/docs/AGENT_WORKFLOW.md:162)
- **Changed requirements:** toolbar/settings and heatmap-coverage work were superseded after implementation. Freeze the owner-visible contract before dispatch.
- **Missing history:** landing removes the worktree’s `.land-ctest.log`, as STATUS explicitly notes. Exact re-land counts and accumulated waiting time cannot be recovered from merge history alone.

**3. Conductor lifetime and cheap resumption**

Use the next safe boundary, with a **120k input-context warning threshold**. This is a proposed operating threshold, not an empirically optimized cutoff. Finish the active review/landing rather than destroying its agents, then hand over before another slice starts. A whole multi-day plan is too large a conductor lifetime.

Proposed AGENTS.md text—**eight lines**, not applied:

```text
- A conductor serves one bounded wave; it does not own the whole plan.
- At each completed wave, checkpoint STATUS and end before dispatching the next wave.
- If the latest input context exceeds 120k tokens, stop new dispatches and checkpoint after the active slice.
- Do not restart during a build, landing, deployment, or non-resumable agent task; finish or safely checkpoint it first.
- Keep STATUS under 120 lines: current facts and evidence pointers, never accumulated session history.
- Record exact tips, agent IDs, review closure, usage sources, approvals, and the next executable step.
- Preserve writer/reviewer threads and logs; the owner names the successor, who reads only the referenced sections.
- A PASS remains valid only for its reviewed tip and covered integration delta; handover does not trigger a fresh full review.
```

STATUS hand-off template:

```text
Updated / outgoing conductor / successor designation:
Main SHA / cleanliness / owner-owned changes:
Plan path + exact next section / completed frontier / next slice:
Owner approvals, constraints, exceptions and unresolved decisions:
Active work: branch, worktree, base, tip, owned files, phase:
Writer/reviewer: provider, model, thread ID, resume method, liveness:
Review: reviewed SHA, verdict, open findings, closure links, integration delta:
Validation: commands, binary SHA, results, native evidence, retained log paths:
Usage: writer/reviewer/planner totals, units, source, missing data:
Runtime: deployed SHAs/PIDs, health evidence, rollback restrictions:
Next executable action / stop condition / owner decision if required:
```

This specifically avoids the historical `05ae99d:docs/STATUS.md` problem: it simultaneously listed already-landed work as in flight and later said “None,” with duplicate waiting sections.

**4. Cloud Projects conductor + Mac operator trial**

The trial preserved native execution, but **did not demonstrate a token saving**.

One identified Mac operator session, [`8a50fe90…jsonl`](/Users/copeharder/.claude/projects/-Users-copeharder-Programming-Sentinel/8a50fe90-e720-4bef-b444-c42d25407449.jsonl), consumed **271.8M tokens locally** over approximately 24 hours, before counting its cloud conductor: **671 usage iterations**, **388k median context**, **788k maximum**, **112 remote send-message calls**, and **206 poll/wait-like Bash commands**.

What broke or became harder:

- **All 42 October 11–12 merges lack token trailers.** Historical STATUS expressly says the operator could not read ma-panel totals and omitted them. This is a recorded interface failure, not evidence that no tokens were spent.
- **Approval relay friction:** `05ae99d:docs/STATUS.md` says a relayed card tap failed the operator’s permission check while typed owner approval passed; conductor-written audit docs needed another explicit approval.
- **Review-role ambiguity:** numerous merges name the cloud conductor as reviewer although section 10 says the conductor is not counted. The chart plan authorizes cheaper reviews for trivial extractions, but does not clearly reconcile that exclusion. Same-provider examples also exist: S0b, serial-stopgap, and roller-timeout.
- **State became contradictory**, as above, and slice E’s finding counts were lost.
- **Post-land usability still failed:** FM-274 records new source QML loading against main’s old binary. That is a landing/main-build gap; the trial does not establish that the split caused it.

The current, already-modified STATUS says the owner ended the trial. **Cloud usage is absent locally, so its total overhead cannot be priced.**

**5. SentinelBar’s 21/29 and B8**

**Primarily a parser/data-contract limitation, with a small plan-document presentation trap.**

The parser recognizes **21 S-slices plus eight B-rows = 29**, regardless of whether a row says “moved.” Completion requires a matching **Plan + Slice git trailer**; it does not interpret dependency or supersession prose. Thus S11’s landing cannot complete B8. [Parser](/Users/copeharder/Programming/Sentinel/tools/SentinelBar/Sources/SentinelBarCore/Core.swift:247), [completion lookup](/Users/copeharder/Programming/Sentinel/tools/SentinelBar/Sources/SentinelBar/Monitor.swift:233)

The plan clearly moved B8 into S11’s acceptance gate, and `d231a11` records the late-enable node-order evidence. **B8 is not unfinished implementation.** [Plan row](/Users/copeharder/Programming/Sentinel/docs/research/2026-10-11-chart-core-architecture.md:778)

For independent remaining work, the count should be **21/28**, leaving B1–B7, including blocked B2. Alternatively, a panel retaining historical obligations could show **22/29**, with B8 “satisfied by S11.” Prefer an explicit `superseded-by`/`satisfied-by` representation; do not make the parser guess arbitrary prose or fabricate a B8 landing.

**6. Top five changes, ranked by expected tokens saved per unit of risk**

1. **Replace repeated status polling with completion notifications or bounded waits.** Lowest-risk saving: remove turns that repeatedly carry 400k–900k context without producing decisions.
2. **Enforce bounded conductor lifetimes and the compact hand-off above.** Largest structural opportunity; preserve live agents and reviewed tips to avoid paying for rediscovery.
3. **Return concise evidence summaries with durable paths; read deltas on fix rounds.** Keep full logs available, but avoid repeatedly injecting entire reports, source files, and transcripts.
4. **Diagnose contracts after two substantive rounds and preflight targeted validation coverage.** The D-a/D-b1/A2 records show why another narrow patch can cost more than a short redesign. Include existing tests for every owned script/file.
5. **Make landing evidence collection automatic and locally accessible.** Preserve review counts, reviewed SHA, log paths, and consistently defined token totals through the existing panel/landing tooling. This reduces operator relay work and prevents future audits from reconstructing missing records.

**Do not save tokens by removing high-risk independent review or native/scenario checks.** The evidence supports reducing coordination overhead and repeated context while keeping those gates.

WORKFLOW: Read-only audit completed; token roles are estimates, cloud Projects usage and complete landing-attempt history are missing, and no files or runtime state were changed.