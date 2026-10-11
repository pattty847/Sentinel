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
        let matches = AgentMatcher.select(processes: processes, transcripts: transcripts)
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
        let matches = AgentMatcher.select(processes: [main, helper, other], transcripts: rollouts)
        #expect(matches.map(\.pid) == [1, 3])
        #expect(Set(matches.compactMap { $0.transcript?.path }).count == 2)
        #expect(matches[0].transcript?.path.hasSuffix("rollout-2.jsonl") == true)
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

    @Test func chartPlanSlicesAndBugRows() throws {
        let path = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent()
            .appendingPathComponent("docs/research/2026-10-11-chart-core-architecture.md")
        let plan = PlanParser.parse(try String(contentsOf: path, encoding: .utf8))
        let ids = Set(plan.phases.flatMap(\.slices).map(\.id))
        for id in ["S0a", "S0b", "S0c", "S0d", "S0e", "S0f"] + (1...15).map({ "S\($0)" })
            + (1...7).map({ "B\($0)" }) {
            #expect(ids.contains(id), "missing \(id)")
        }
        #expect(plan.title.hasPrefix("Chart core architecture"))
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
}
