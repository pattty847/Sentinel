import Foundation
import Testing
@testable import SentinelBarCore

struct CoreTests {
    @Test func codexRolloutStructure() {
        let lines = [
            #"{"type":"session_meta","payload":{"cwd":"/synthetic/worktree"}}"#,
            #"{"type":"event_msg","payload":{"type":"task_started"}}"#,
            #"{"type":"response_item","payload":{"type":"function_call","name":"exec_command","arguments":"{\"cmd\":\"synthetic check\"}"}}"#,
            #"{"type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"total_tokens":42}}}}"#,
            #"{"type":"event_msg","payload":{"type":"task_complete"}}"#
        ]
        #expect(TranscriptParser.codexCwd(lines[0]) == "/synthetic/worktree")
        let state = TranscriptParser.codex(lines)
        #expect(state.step == "exec_command: synthetic check")
        #expect(state.tokens == 42)
        #expect(state.started)
        #expect(state.completed)
    }

    @Test func claudeTranscriptAndSubagentShape() {
        let lines = [
            #"{"type":"assistant","cwd":"/synthetic/worktree","message":{"model":"synthetic-model","usage":{"input_tokens":3,"output_tokens":2},"content":[{"type":"tool_use","name":"Read","input":{"file_path":"/synthetic/file"}}]}}"#,
            #"{"type":"user","message":{"content":[{"type":"tool_result","is_error":true}]}}"#
        ]
        let state = TranscriptParser.claude(lines)
        #expect(state.model == "synthetic-model")
        #expect(state.step == "Read: /synthetic/file")
        #expect(state.tokens == 5)
        #expect(!state.failed)
    }

    @Test func staleSubagentDisappearsWithoutNotification() {
        let state = TranscriptState()
        let idle = SubagentLifecycle.decide(parentAlive: true, state: state, secondsSinceActivity: 300)
        #expect(idle == .active)
        #expect(!idle.shouldNotify)
        let stale = SubagentLifecycle.decide(parentAlive: true, state: state, secondsSinceActivity: 1801)
        #expect(stale == .stale)
        #expect(!stale.shouldNotify)
        #expect(SubagentLifecycle.decide(parentAlive: false, state: state,
                                         secondsSinceActivity: 1801) == .stale)

        var completed = state
        completed.completed = true
        #expect(SubagentLifecycle.decide(parentAlive: true, state: completed,
                                         secondsSinceActivity: 300) == .finished)
        var failed = state
        failed.failed = true
        #expect(SubagentLifecycle.decide(parentAlive: true, state: failed,
                                         secondsSinceActivity: 300) == .failed)
        #expect(SubagentLifecycle.decide(parentAlive: false, state: state,
                                         secondsSinceActivity: 300).shouldNotify)

        let ended = TranscriptParser.claude([
            #"{"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"synthetic done"}]}}"#
        ])
        #expect(ended.completed)
    }

    @Test func claudeSessionsAreDistinctAndDefaultModelFallsBack() {
        let cwd = "/synthetic/main"
        let processes = (1...6).map { index in
            AgentProcess(pid: Int32(index), kind: "claude", cwd: cwd,
                         args: index == 1 ? ["claude", "--resume", "session-3", "--model", "default"] : ["claude"],
                         hasWorktree: true)
        }
        let transcripts = (1...7).map { index in
            AgentTranscript(path: "/synthetic/claude/session-\(index).jsonl", cwd: cwd,
                            kind: "claude", modified: Date(timeIntervalSince1970: Double(index)))
        }
        let matches = AgentMatcher.select(processes: processes, transcripts: transcripts,
                                          now: Date(timeIntervalSince1970: 100))
        #expect(matches.count == 6)
        #expect(matches.first?.transcript?.path.hasSuffix("session-3.jsonl") == true)
        #expect(Set(matches.compactMap { $0.transcript?.path }).count == 6)
        #expect(!matches.contains { $0.transcript?.path.hasSuffix("session-1.jsonl") == true })
        #expect(AgentMatcher.modelArgument(processes[0].args) == nil)
    }

    @Test func codexUsesCommandCwdAndHidesUnmatchedHelpers() {
        let main = AgentProcess(pid: 1, kind: "codex", cwd: "/synthetic/launcher",
                                args: ["codex", "-C", "/synthetic/worktree"], hasWorktree: true)
        let helper = AgentProcess(pid: 2, kind: "codex", cwd: "/synthetic/helper",
                                  args: ["codex"], hasWorktree: false)
        let other = AgentProcess(pid: 3, kind: "codex", cwd: "/synthetic/worktree",
                                 args: ["codex", "--cd=/synthetic/worktree"], hasWorktree: true)
        let rollouts = (1...2).map { index in
            AgentTranscript(path: "/synthetic/rollout-\(index).jsonl", cwd: "/synthetic/worktree",
                            kind: "codex", modified: Date(timeIntervalSince1970: Double(index)))
        }
        #expect(AgentMatcher.cwd(for: main) == "/synthetic/worktree")
        let matches = AgentMatcher.select(processes: [main, helper, other], transcripts: rollouts,
                                          now: Date(timeIntervalSince1970: 100))
        #expect(matches.map(\.pid) == [1, 3])
        #expect(Set(matches.compactMap { $0.transcript?.path }).count == 2)
        #expect(matches[0].transcript?.path.hasSuffix("rollout-2.jsonl") == true)
    }

    @Test func unpinnedClaudeIgnoresOldTranscriptButPinnedSessionCanUseIt() {
        let now = Date(timeIntervalSince1970: 200_000)
        let cwd = "/synthetic/main"
        let old = AgentTranscript(path: "/synthetic/old.jsonl", cwd: cwd, kind: "claude",
                                  modified: now.addingTimeInterval(-25 * 3600))
        let fresh = AgentTranscript(path: "/synthetic/fresh.jsonl", cwd: cwd, kind: "claude",
                                    modified: now.addingTimeInterval(-3600))
        let pinned = AgentProcess(pid: 1, kind: "claude", cwd: cwd,
                                  args: ["claude", "--resume", "old"], hasWorktree: true)
        let current = AgentProcess(pid: 2, kind: "claude", cwd: cwd, args: ["claude"], hasWorktree: true)
        let extra = AgentProcess(pid: 3, kind: "claude", cwd: cwd, args: ["claude"], hasWorktree: true)
        let matches = AgentMatcher.select(processes: [pinned, current, extra],
                                          transcripts: [old, fresh], now: now)
        #expect(matches.map { $0.transcript?.path } == [old.path, fresh.path, nil])
        let unpinned = AgentMatcher.select(processes: [current, extra], transcripts: [old], now: now)
        #expect(unpinned.allSatisfy { $0.transcript == nil })
    }

    @Test func runningAgentCountExcludesIdleSessionsAndIdleSubagents() {
        #expect(AgentCounter.runningCount([.running, .idle, .subagentIdle, .running]) == 2)
        #expect(AgentCounter.runningCount([.idle, .subagentIdle]) == 0)
    }

    @Test func planSkipsNewestZeroSliceDocumentUnlessPinned() {
        let empty = "# SentinelBar\n## Goal\nNo slices here."
        let slices = "# Chart plan\n### Phase One\n**S0a: Camera**"
        let candidates = [(path: "sentinelbar.md", document: empty),
                          (path: "chart.md", document: slices)]
        let selected = PlanParser.select(candidates)
        #expect(selected?.path == "chart.md")
        #expect(selected?.plan.phases.flatMap(\.slices).map(\.id) == ["S0a"])
        #expect(PlanParser.select(candidates, pinned: true)?.path == "sentinelbar.md")
    }

    @Test func budgetRowsParseWindowsCreditsAndRawFallback() {
        let lines = [
            "codex  weekly      60% left resets in 6.3d | +30% DEFICIT, runs out in 1.0d",
            "codex  resets     3 credit(s) held by the owner",
            "claude session     90% left resets in 2.5h | -40% reserve",
            "claude weekly      14% left resets in 1.3d | +4% DEFICIT, runs out in 0.9d",
            "codex  extra       80% left",
            "unrecognized budget line"
        ]
        let expected: [BudgetEntry] = [
            .window(BudgetWindow(provider: "codex", window: "weekly", percentLeft: 60, reset: "6.3d",
                                 pace: BudgetPace(kind: .deficit, percent: 30, runsOut: "1.0d"))),
            .credits(provider: "codex", count: 3),
            .window(BudgetWindow(provider: "claude", window: "session", percentLeft: 90, reset: "2.5h",
                                 pace: BudgetPace(kind: .reserve, percent: -40, runsOut: nil))),
            .window(BudgetWindow(provider: "claude", window: "weekly", percentLeft: 14, reset: "1.3d",
                                 pace: BudgetPace(kind: .deficit, percent: 4, runsOut: "0.9d"))),
            .window(BudgetWindow(provider: "codex", window: "extra", percentLeft: 80,
                                 reset: nil, pace: nil)),
            .raw("unrecognized budget line")
        ]
        #expect(BudgetParser.parse(lines) == expected)
    }

    @Test func budgetEvenPaceTickAndDurationFormatting() {
        #expect(abs((BudgetGuide.evenPaceLeft(window: "weekly", reset: "6.3d") ?? 0) - 0.9) < 0.0001)
        #expect(BudgetGuide.evenPaceLeft(window: "session", reset: "2.5h") == 0.5)
        #expect(BudgetGuide.evenPaceLeft(window: "session", reset: "8h") == 1)
        #expect(BudgetGuide.evenPaceLeft(window: "extra", reset: "2.5h") == nil)
        #expect(BudgetGuide.evenPaceLeft(window: "weekly", reset: nil) == nil)
        #expect(BudgetGuide.duration("6.3d") == "6d 7h")
        #expect(BudgetGuide.duration("2.5h") == "2h 30m")
        #expect(BudgetGuide.duration("0.95d") == "22h 48m")
        #expect(BudgetGuide.duration("unknown") == nil)
    }

    @Test func privatePlanDirectoryAndTrailerResolution() {
        let directory = PlanLocation.researchDirectory(repository: "/workspace/Sentinel")
        #expect(directory.path == "/workspace/Sentinel-Private/research")
        #expect(PlanLocation.researchDirectory(repository: "/workspace/Sentinel",
            configured: "../Other-Private/research").path == "/workspace/Other-Private/research")
        let custom = PlanLocation.researchDirectory(repository: "/workspace/Sentinel",
            configured: "/custom/plans")
        #expect(custom.path == "/custom/plans")
        #expect(PlanLocation.resolve("docs/research/example.md", directory: custom).path == "/custom/plans/example.md")
        #expect(PlanLocation.resolve("../Sentinel-Private/research/audit/example.md", directory: custom).path
            == "/custom/plans/audit/example.md")
        #expect(PlanLocation.resolve("example.md", directory: directory, pinned: true).path
            == "/workspace/Sentinel-Private/research/example.md")
        #expect(PlanLocation.resolve("/explicit/example.md", directory: custom, pinned: true).path
            == "/explicit/example.md")
    }

    @Test func planSliceTableAndBugRows() {
        let plan = PlanParser.parse("""
        # Example feature plan
        ### Phase 1: Foundation
        **S0a: Establish baseline**
        **S0f (review): Validate baseline**
        ### Phase 2: Implementation
        **S1: Implement policy**
        **S15: Complete integration**
        ### Phase 3: Follow-up bugs
        | ID | Issue | Acceptance |
        | --- | --- | --- |
        | B1 | Boundary condition | Reproduction passes |
        | B7 | Teardown | Cleanup completes |
        """)
        let ids = Set(plan.phases.flatMap(\.slices).map(\.id))
        #expect(ids == Set(["S0a", "S0f", "S1", "S15", "B1", "B7"]))
        #expect(plan.title == "Example feature plan")
        #expect(plan.phases.count == 3)
        #expect(plan.phases.first?.slices.first?.title == "Establish baseline")
        #expect(plan.phases.last?.slices.first?.title == "Boundary condition")
    }

    @Test func trailersAndBranchBoundaries() {
        #expect(PlanParser.sliceIDs(in: "S0a+S0f") == ["s0a", "s0f"])
        #expect(PlanParser.branchSlice("lt-sol/s0b-camera", ids: ["s0b", "s0"]) == "s0b")
        #expect(PlanParser.branchSlice("lt-sol/s0bar", ids: ["s0b"]) == nil)
    }

    @Test func queueOrderAndStaleness() {
        let now = Date(timeIntervalSince1970: 1000)
        let tickets = [
            Ticket(id: "002.ticket", label: "second", queuedAt: now, modifiedAt: now),
            Ticket(id: "001.ticket", label: "first", queuedAt: now, modifiedAt: now),
            Ticket(id: "000.ticket", label: "stale", queuedAt: now, modifiedAt: now.addingTimeInterval(-121))
        ]
        let live = TicketParser.live(tickets, now: now)
        #expect(live.map(\.label) == ["first", "second"])
        #expect(live.map(\.running) == [true, false])
    }

    @Test func eventTailAndResolution() {
        let events = EventParser.parse([
            #"{"at":1000,"kind":"blocker","text":"synthetic issue","tone":"bad","slice":"S1"}"#,
            #"{"at":2000,"kind":"resolved","text":"fixed","tone":"ok","slice":"S1"}"#,
            #"{"at":3000,"kind":"review","text":"round 1","tone":"info","branch":"lt-sol/example"}"#,
            #"not json"#
        ])
        #expect(events.count == 3)
        #expect(EventParser.unresolved(events).isEmpty)
        #expect(events.last?.branch == "lt-sol/example")
    }

    @Test func activityFoldsOnlyConsecutiveMatchingEvents() {
        func event(_ second: TimeInterval, kind: String = "build_failed", text: String = "s10 exited 1",
                   branch: String = "lt-astra/s10") -> Milestone {
            Milestone(at: Date(timeIntervalSince1970: second), kind: kind, text: text,
                      tone: "bad", slice: nil, branch: branch)
        }
        let events = [event(10), event(9), event(8, text: "s10 exited 139"), event(7),
                      event(6, branch: "other"), event(5, kind: "review")]
        let groups = ActivityFolder.fold(events)
        #expect(groups.map(\.count) == [2, 1, 1, 1, 1])
        #expect(groups.map(\.text) == ["s10 exited 1 ×2", "s10 exited 139", "s10 exited 1",
                                       "s10 exited 1", "s10 exited 1"])
        #expect(groups[0].latest.at == events[0].at)
        #expect(groups[0].earliestAt == events[1].at)
        #expect(groups[2].latest.at == events[3].at)
    }
}
