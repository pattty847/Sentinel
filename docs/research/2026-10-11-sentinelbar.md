# SentinelBar: native menu bar view of agents, plans and builds (2026-10-11)

Owner approved on 2026-10-11 in the Projects thread "Sentinel menu bar app". Owner decisions:
- One extra line per milestone (review round, blocker) from the conductor is fine.
- Use the owner's glyph (`resources/brand/sentinel-glyph.png`).
- Ship it.

## Goal

A SwiftUI `MenuBarExtra` app in `tools/SentinelBar/`. It shows, live, what every agent is doing, where each plan stands, and what is building, and it posts a native notification when something happens. It is read-only: it never writes to the repo, the queue, services or recorded data. The only file it writes is its own settings. Agents spend no tokens to keep it current: it reads files that already exist. The one exception is the milestone line in "Milestone events" below.

## Data sources (all local, polled)

| What | Source | Poll |
|---|---|---|
| Codex threads: model, cwd, tokens, current step, done or failed | `$CODEX_HOME/sessions/YYYY/MM/DD/rollout-*.jsonl` (today and yesterday). Line 1 is `session_meta` with the cwd. Later lines carry `function_call`, `message`, `token_count` and `task_started`/`task_complete`. Verify the real field names on this Mac before parsing. | 4 s |
| Claude sessions and subagents: current tool call, last active, model | `~/.claude/projects/<cwd with / and . as ->/*.jsonl` (main sessions). Subagents are in sibling files or in subdirectories; verify the real layout. | 4 s |
| Which agents are alive | `ps` for `claude` and `codex` processes, plus `lsof -d cwd` for each one's cwd. Collapse wrappers (a parent and child of the same kind). | 4 s |
| Running build and queue | `/tmp/sentinel-build-queue/*.ticket`, using the same order and staleness rules as `build-queue.sh status`. Also scan `ps` for `cmake --build`, `ninja`, `ctest` and `swift build` processes that have no ticket, and show those as "outside queue". | 2 s |
| Worktrees | `git worktree list --porcelain`, plus `rev-list --count main..<branch>` | 10 s |
| Plan structure | the plan doc named by the newest `Plan:` trailer on `git log --first-parent main`, or one pinned in settings. Sections are `### Phase …` headings; slices are lines matching `^\*\*(S[0-9]+[a-z]?\|B[0-9]+)[:*]` (id plus title). Fall back to `##` headings. | on change of main |
| Slice done | merge trailers `Slice:` (which can list several ids, e.g. `S0a+S0f`) + `Plan:` on `git log --first-parent main`; show the merge's short sha | on change of main |
| Slice in progress | a worktree whose branch contains the slice id as a word (`lt-sol/s0b-...` -> S0b, case-insensitive), plus the agents whose cwd is inside it | 4 s |
| Milestones | `~/Library/Application Support/Sentinel/events.jsonl`, one JSON object per line: `{"at": <unix ms>, "kind": "...", "text": "...", "tone": "info\|ok\|warn\|bad", "slice": "S0b"?, "branch": "..."?}` | 2 s (tail) |
| Services | GET `127.0.0.1:8090/metrics`, `:8091/metrics` (critical) and `:17190/status` (informational), each with a 2 s timeout | 5 s |
| Budget | `scripts/dev/budget.sh` text lines | 5 min |

Polling must stay cheap. Read only the tails of transcripts (256 KB at most), and skip files whose mtime hasn't changed since the last read. Never re-read a whole rollout.

## Milestone events

New `scripts/dev/sentinel-event.sh <kind> <tone> <text> [--slice S0b] [--branch b]` appends one line to `events.jsonl`. It creates the directory, writes with an atomic append (a single `printf >>`), and needs only POSIX tools, so it works from Codex sandboxes. If the sandbox blocks that path, it falls back to `/tmp/sentinel-events.jsonl`, and the app reads both files.

Callers:
- **`build-queue.sh`:** `build_failed` (with label and exit code) when the command exits non-zero. Nothing on start or success, because those are too noisy.
- **`agent-worktree.sh land`:** `landed` (branch, slice from the `Slice:` trailer, sha) on success, and `land_failed` (which gate) on failure.
- **The conductor, by hand:** `review` (round N, findings blocker/major/minor), `blocker`, `plan` (new plan or phase change). One line per milestone. `docs/AGENT_WORKFLOW.md` gets a short note that says so.

Do not touch `deploy-runtime.sh` in this slice.

## UX spec (frozen)

**Menu bar label:** the template image `MenuGlyph` (18 pt, @1x/@2x in `Sources/SentinelBar/Resources`, `isTemplate = true`). The glyph is followed by the number of running agents when that is above 0. It switches to `MenuGlyphAlert` when an unresolved `blocker` event exists from the last 24 h, or when a critical service is down. No colour in the label.

**Panel** (`.menuBarExtraStyle(.window)`, 420 pt wide, scrolls if taller than 640 pt), top to bottom:
1. **Running now:** one row per active build. Each row has a hammer, the label, a short command, an elapsed timer (`Text(date, style: .timer)`), and "next: <label>" if others are waiting. Builds outside the queue get a warning row. The section is hidden when nothing is running.
2. **Plan:** the title from the doc's first `#` line, then "phase · done/total", then phase headings with slices underneath:
   - ✓ for landed (with its sha), ◐ for in progress, ○ for not started;
   - an in-progress slice shows its agent (provider icon, model, status, current step) and its latest milestone (e.g. "review r1: 0/2/1");
   - phases that are fully done collapse to one line.
3. **Blockers:** shown only when there are any. These are unresolved `blocker` events, and a later event with `"kind":"resolved"` and the same slice or text clears one.
4. **Agents:** one card per live agent:
   - provider icon (Claude or Codex), model, role if known (from the event's branch prefix or the slice), and branch or worktree;
   - status (running, or idle after 2 min with no transcript change), current step (2 lines), tokens if known, and last active (relative time).

   Clicking a card opens its worktree in Terminal. Option-clicking it opens its transcript in the default app. Agents with no worktree are labelled "unmanaged".
5. **Activity:** the last 15 events, merged from `events.jsonl`, landings and agent start/finish, newest first, with timestamps.
6. **Infra:** worktrees (collapsible), services (one dot each), and budget lines.
7. **Footer:** Grafana, Logs (`~/Library/Logs/Sentinel`), Terminal (at the repo), Settings, Refresh and Quit, plus "updated hh:mm:ss".

**Settings** (a small sheet, stored in UserDefaults `com.sentinel.bar`):
- the repo path, defaulting to `~/Programming/Sentinel`;
- the pinned plan path, which is optional;
- open at login (`SMAppService`);
- one toggle per notification kind.

**Notifications** (`UNUserNotificationCenter`; request authorization on first launch):

| Kind | Default |
|---|---|
| agent started | off |
| agent finished | on |
| agent failed | on |
| slice landed | on |
| land failed | on |
| review | on |
| blocker raised | on |
| blocker resolved | on |
| plan or phase change | on |
| build failed | on |
| service down | on |
| service back up | on |

Coalesce repeats: send at most 1 notification per kind per agent per 30 s. Send nothing for history already present at launch; only events newer than launch count.

## Non-goals (v1)
- Phone push through ntfy. This is a later toggle; the topic lives in `ops/monitoring/ntfy.env`.
- Cloud Projects threads. They are not on the Mac; their Mac-side work shows up through Remote Control sessions, Codex and the queue.
- Exporting ma-panel's `$.store`.

## Build and ship
- `tools/SentinelBar/build-app.sh [--install]` builds release, then puts the resource PNGs in `Contents/Resources` and loads them through `Bundle.main` (not `Bundle.module`, whose resource bundle sits in the `.app` root and breaks codesign). It ad-hoc signs with `codesign -s -`, never with the Sentinel identity.
- `swift build` runs through `scripts/dev/build-queue.sh`.
- Pure logic goes in a separate library target, `SentinelBarCore`, with XCTest tests in `Tests/SentinelBarCoreTests`. That covers the rollout and transcript parsers (with fixture lines), plan and slice parsing against `docs/research/2026-10-11-chart-core-architecture.md` (S0a…S15, B1…B7), trailer-to-slice matching, branch-to-slice matching, ticket ordering and staleness, and event tail parsing. Each test must fail when its parser is broken.
