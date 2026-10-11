import Foundation

public struct TranscriptState: Equatable {
    public var model: String?
    public var step: String?
    public var tokens: Int?
    public var completed = false
    public var failed = false
    public var started = false

    public init() {}
}

public enum SubagentDisposition: Equatable {
    case active
    case finished
    case failed
    case stale

    public var shouldNotify: Bool { self == .finished || self == .failed }
}

public enum SubagentLifecycle {
    public static func decide(parentAlive: Bool, state: TranscriptState,
                              secondsSinceActivity: TimeInterval) -> SubagentDisposition {
        if state.failed { return .failed }
        if state.completed { return .finished }
        if secondsSinceActivity > 30 * 60 { return .stale }
        if !parentAlive { return .finished }
        return .active
    }
}

public struct AgentProcess {
    public let pid: Int32
    public let kind: String
    public let cwd: String
    public let args: [String]
    public let hasWorktree: Bool

    public init(pid: Int32, kind: String, cwd: String, args: [String], hasWorktree: Bool) {
        self.pid = pid; self.kind = kind; self.cwd = cwd; self.args = args; self.hasWorktree = hasWorktree
    }
}

public struct AgentTranscript {
    public let path: String
    public let cwd: String
    public let kind: String
    public let modified: Date

    public init(path: String, cwd: String, kind: String, modified: Date) {
        self.path = path; self.cwd = cwd; self.kind = kind; self.modified = modified
    }
}

public struct AgentMatch {
    public let pid: Int32
    public let transcript: AgentTranscript?
}

public enum AgentMatcher {
    public static func cwd(for process: AgentProcess) -> String {
        guard process.kind == "codex", let path = argument(process.args, names: ["-C", "--cd"]) else {
            return process.cwd
        }
        let full = path.hasPrefix("/") ? path : process.cwd + "/" + path
        return URL(fileURLWithPath: full).standardizedFileURL.path
    }

    public static func modelArgument(_ args: [String]) -> String? {
        guard let model = argument(args, names: ["-m", "--model"]), model != "default" else { return nil }
        return model
    }

    public static func select(processes: [AgentProcess], transcripts: [AgentTranscript], now: Date) -> [AgentMatch] {
        var used: Set<String> = []
        var selected: [Int32: AgentTranscript] = [:]
        let ordered = transcripts.sorted { $0.modified == $1.modified ? $0.path < $1.path : $0.modified > $1.modified }
        // A pinned Claude session takes precedence over cwd-based assignment.
        for process in processes where process.kind == "claude" {
            guard let id = argument(process.args, names: ["--resume", "-r", "--session-id"]),
                  let transcript = ordered.first(where: { $0.kind == "claude" && $0.cwd == cwd(for: process) &&
                      URL(fileURLWithPath: $0.path).lastPathComponent == id + ".jsonl" && !used.contains($0.path) }) else { continue }
            selected[process.pid] = transcript
            used.insert(transcript.path)
        }
        for process in processes where selected[process.pid] == nil {
            if process.kind == "claude" &&
                argument(process.args, names: ["--resume", "-r", "--session-id"]) != nil { continue }
            guard let transcript = ordered.first(where: { $0.kind == process.kind && $0.cwd == cwd(for: process) &&
                !used.contains($0.path) && (process.kind != "claude" ||
                    $0.modified >= now.addingTimeInterval(-24 * 3600)) }) else { continue }
            selected[process.pid] = transcript
            used.insert(transcript.path)
        }
        return processes.compactMap { process in
            let transcript = selected[process.pid]
            return transcript != nil || process.hasWorktree ? AgentMatch(pid: process.pid, transcript: transcript) : nil
        }
    }

    private static func argument(_ args: [String], names: [String]) -> String? {
        for (index, arg) in args.enumerated() {
            if names.contains(arg), index + 1 < args.count { return args[index + 1] }
            for name in names where name.hasPrefix("--") && arg.hasPrefix(name + "=") {
                return String(arg.dropFirst(name.count + 1))
            }
        }
        return nil
    }
}

public enum TranscriptParser {
    public static func codex(_ lines: [String]) -> TranscriptState {
        var state = TranscriptState()
        for line in lines {
            guard let object = json(line), let outer = object["type"] as? String,
                  let payload = object["payload"] as? [String: Any] else { continue }
            let kind = payload["type"] as? String
            if outer == "event_msg" {
                switch kind {
                case "task_started": state.started = true; state.completed = false
                case "task_complete": state.completed = true
                case "item_completed":
                    if let item = payload["item"] as? [String: Any],
                       item["status"] as? String == "failed" { state.failed = true }
                case "token_count":
                    if let info = payload["info"] as? [String: Any],
                       let usage = info["total_token_usage"] as? [String: Any] {
                        state.tokens = usage["total_tokens"] as? Int
                    }
                default: break
                }
            } else if outer == "response_item" {
                switch kind {
                case "function_call", "custom_tool_call", "local_shell_call":
                    let name = payload["name"] as? String ?? "tool"
                    let raw = payload["arguments"] as? String ?? payload["input"] as? String ?? ""
                    let input = json(raw) ?? [:]
                    let detail = input["description"] as? String ?? input["cmd"] as? String
                        ?? input["command"] as? String ?? input["title"] as? String ?? ""
                    state.step = short(detail.isEmpty ? name : "\(name): \(detail)")
                case "message":
                    if payload["role"] as? String == "assistant",
                       let parts = payload["content"] as? [[String: Any]],
                       let message = parts.compactMap({ $0["text"] as? String }).last, !message.isEmpty {
                        state.step = short(message)
                        state.failed = false
                    }
                default: break
                }
            } else if outer == "turn_context", let model = payload["model"] as? String {
                state.model = model
            }
        }
        return state
    }

    public static func codexCwd(_ line: String) -> String? {
        guard let object = json(line), object["type"] as? String == "session_meta",
              let payload = object["payload"] as? [String: Any] else { return nil }
        return payload["cwd"] as? String
    }

    public static func claude(_ lines: [String]) -> TranscriptState {
        var state = TranscriptState()
        for line in lines {
            guard let object = json(line), let type = object["type"] as? String else { continue }
            if type == "assistant", let message = object["message"] as? [String: Any] {
                state.started = true
                state.completed = message["stop_reason"] as? String == "end_turn"
                state.failed = false
                state.model = message["model"] as? String ?? state.model
                if let usage = message["usage"] as? [String: Any] {
                    let input = usage["input_tokens"] as? Int ?? 0
                    let output = usage["output_tokens"] as? Int ?? 0
                    if input + output > 0 { state.tokens = (state.tokens ?? 0) + input + output }
                }
                if let content = message["content"] as? [[String: Any]] {
                    for item in content {
                        if item["type"] as? String == "tool_use", let name = item["name"] as? String {
                            let input = item["input"] as? [String: Any] ?? [:]
                            let detail = ["description", "command", "file_path", "pattern", "prompt"]
                                .compactMap { input[$0] as? String }.first
                            state.step = short(detail.map { "\(name): \($0)" } ?? name)
                        } else if item["type"] as? String == "text", let text = item["text"] as? String,
                                  !text.isEmpty { state.step = short(text) }
                    }
                }
            } else if type == "system", object["subtype"] as? String == "turn_duration" {
                state.completed = true
            }
        }
        return state
    }

    private static func short(_ text: String) -> String {
        let first = text.split(whereSeparator: \.isNewline).first.map(String.init) ?? text
        return first.count > 120 ? String(first.prefix(119)) + "…" : first
    }
}

public struct PlanSlice: Equatable, Identifiable {
    public let id: String
    public let title: String
    public init(id: String, title: String) { self.id = id; self.title = title }
}

public struct PlanPhase: Equatable, Identifiable {
    public let title: String
    public var slices: [PlanSlice]
    public var id: String { title }
    public init(title: String, slices: [PlanSlice]) { self.title = title; self.slices = slices }
}

public struct Plan: Equatable {
    public let title: String
    public let phases: [PlanPhase]
    public init(title: String, phases: [PlanPhase]) { self.title = title; self.phases = phases }
}

public enum PlanParser {
    public static func select(_ candidates: [(path: String, document: String)], pinned: Bool = false)
        -> (path: String, plan: Plan)? {
        for candidate in candidates {
            let plan = parse(candidate.document)
            if pinned || plan.phases.contains(where: { !$0.slices.isEmpty }) {
                return (candidate.path, plan)
            }
        }
        return nil
    }

    public static func parse(_ document: String) -> Plan {
        let lines = document.components(separatedBy: .newlines)
        let title = lines.first { $0.hasPrefix("# ") }.map { String($0.dropFirst(2)) } ?? "Plan"
        let usePhases = lines.contains { $0.hasPrefix("### Phase ") }
        var phases: [PlanPhase] = []
        var current: PlanPhase?
        let pattern = try! NSRegularExpression(pattern: #"^\*\*((S[0-9]+[a-z]?|B[0-9]+))(?:\s+\([^)]*\))?:?\s*(.*?)\*\*"#, options: .caseInsensitive)
        let table = try! NSRegularExpression(pattern: #"^\|\s*(B[0-9]+)\s*\|\s*(.*?)\s*\|"#, options: .caseInsensitive)
        for line in lines {
            if (usePhases && line.hasPrefix("### Phase ")) || (!usePhases && line.hasPrefix("## ")) {
                if let current { phases.append(current) }
                current = PlanPhase(title: String(line.dropFirst(usePhases ? 4 : 3)), slices: [])
                continue
            }
            guard var phase = current else { continue }
            let range = NSRange(line.startIndex..<line.endIndex, in: line)
            guard let match = pattern.firstMatch(in: line, range: range) ?? table.firstMatch(in: line, range: range),
                  let idRange = Range(match.range(at: 1), in: line),
                  let titleRange = Range(match.range(at: match.numberOfRanges - 1), in: line) else { continue }
            phase.slices.append(PlanSlice(id: String(line[idRange]), title: String(line[titleRange])))
            current = phase
        }
        if let current { phases.append(current) }
        return Plan(title: title, phases: phases.filter { !$0.slices.isEmpty })
    }

    public static func sliceIDs(in trailer: String) -> [String] {
        let expression = try! NSRegularExpression(pattern: #"(?i)\b(?:S[0-9]+[a-z]?|B[0-9]+)\b"#)
        let range = NSRange(trailer.startIndex..<trailer.endIndex, in: trailer)
        return expression.matches(in: trailer, range: range).compactMap { match in
            Range(match.range, in: trailer).map { String(trailer[$0]).lowercased() }
        }
    }

    public static func branchSlice(_ branch: String, ids: [String]) -> String? {
        let lower = branch.lowercased()
        return ids.first { id in
            let escaped = NSRegularExpression.escapedPattern(for: id.lowercased())
            let regex = try! NSRegularExpression(pattern: "(?<![a-z0-9])\(escaped)(?![a-z0-9])")
            return regex.firstMatch(in: lower, range: NSRange(lower.startIndex..<lower.endIndex, in: lower)) != nil
        }
    }
}

public struct BudgetWindow: Equatable {
    public let provider: String
    public let window: String
    public let percentLeft: Int
    public let reset: String?
    public let pace: BudgetPace?
}

public struct BudgetPace: Equatable {
    public enum Kind: Equatable { case deficit, reserve }
    public let kind: Kind
    public let percent: Int
    public let runsOut: String?
}

public enum BudgetEntry: Equatable {
    case window(BudgetWindow)
    case credits(provider: String, count: Int)
    case raw(String)
}

public enum BudgetParser {
    private static let windowPattern = try! NSRegularExpression(
        pattern: #"^\s*(claude|codex)\s+(.+?)\s+(-?\d+)% left(?:\s+resets in (\S+))?(?:\s+\|\s+([+-]\d+)% (DEFICIT|reserve)(?:, runs out in (\S+))?)?\s*$"#)
    private static let creditsPattern = try! NSRegularExpression(
        pattern: #"^\s*(claude|codex)\s+resets\s+(\d+) credit\(s\) held by the owner\s*$"#)

    public static func parse(_ lines: [String]) -> [BudgetEntry] {
        lines.map(parseLine)
    }

    private static func parseLine(_ line: String) -> BudgetEntry {
        let range = NSRange(line.startIndex..<line.endIndex, in: line)
        if let match = creditsPattern.firstMatch(in: line, range: range),
           let provider = field(1, in: line, match: match),
           let count = field(2, in: line, match: match).flatMap(Int.init) {
            return .credits(provider: provider, count: count)
        }
        guard let match = windowPattern.firstMatch(in: line, range: range),
              let provider = field(1, in: line, match: match),
              let window = field(2, in: line, match: match),
              let percentLeft = field(3, in: line, match: match).flatMap(Int.init) else { return .raw(line) }
        let pace: BudgetPace?
        if let percent = field(5, in: line, match: match).flatMap(Int.init),
           let label = field(6, in: line, match: match) {
            pace = BudgetPace(kind: label == "DEFICIT" ? .deficit : .reserve,
                              percent: percent, runsOut: field(7, in: line, match: match))
        } else {
            pace = nil
        }
        return .window(BudgetWindow(provider: provider, window: window, percentLeft: percentLeft,
                                    reset: field(4, in: line, match: match), pace: pace))
    }

    private static func field(_ index: Int, in line: String, match: NSTextCheckingResult) -> String? {
        guard let range = Range(match.range(at: index), in: line) else { return nil }
        return String(line[range])
    }
}

public enum BudgetGuide {
    public static func evenPaceLeft(window: String, reset: String?) -> Double? {
        guard let reset, let hours = hoursUntilReset(reset) else { return nil }
        let length: Double
        switch window.lowercased() {
        case "weekly": length = 7 * 24
        case "session": length = 5
        default: return nil
        }
        return min(1, max(0, hours / length))
    }

    public static func duration(_ raw: String) -> String? {
        guard let hours = hoursUntilReset(raw) else { return nil }
        if hours >= 24 {
            let roundedHours = Int(hours.rounded())
            let days = roundedHours / 24
            let remainingHours = roundedHours % 24
            return remainingHours == 0 ? "\(days)d" : "\(days)d \(remainingHours)h"
        }
        let minutes = Int((hours * 60).rounded())
        let wholeHours = minutes / 60
        let remainingMinutes = minutes % 60
        if wholeHours == 0 { return "\(remainingMinutes)m" }
        return remainingMinutes == 0 ? "\(wholeHours)h" : "\(wholeHours)h \(remainingMinutes)m"
    }

    private static func hoursUntilReset(_ raw: String) -> Double? {
        guard let unit = raw.last, let value = Double(raw.dropLast()), value.isFinite, value >= 0 else { return nil }
        switch unit {
        case "d": return value * 24
        case "h": return value
        default: return nil
        }
    }
}

public struct Ticket: Equatable, Identifiable {
    public let id: String
    public let label: String
    public let queuedAt: Date
    public let modifiedAt: Date
    public var running: Bool
    public init(id: String, label: String, queuedAt: Date, modifiedAt: Date, running: Bool = false) {
        self.id = id; self.label = label; self.queuedAt = queuedAt; self.modifiedAt = modifiedAt; self.running = running
    }
}

public enum TicketParser {
    public static func live(_ tickets: [Ticket], now: Date) -> [Ticket] {
        let ordered = tickets.filter { now.timeIntervalSince($0.modifiedAt) <= 120 }.sorted { $0.id < $1.id }
        return ordered.enumerated().map { index, ticket in
            Ticket(id: ticket.id, label: ticket.label, queuedAt: ticket.queuedAt,
                   modifiedAt: ticket.modifiedAt, running: index == 0)
        }
    }
}

public struct Milestone: Equatable, Identifiable {
    public let at: Date
    public let kind: String
    public let text: String
    public let tone: String
    public let slice: String?
    public let branch: String?
    public var id: String { "\(at.timeIntervalSince1970):\(kind):\(text)" }
    public init(at: Date, kind: String, text: String, tone: String, slice: String?, branch: String?) {
        self.at = at; self.kind = kind; self.text = text; self.tone = tone; self.slice = slice; self.branch = branch
    }
}

public enum EventParser {
    public static func parse(_ lines: [String]) -> [Milestone] {
        lines.compactMap { line in
            guard let object = json(line), let milliseconds = object["at"] as? Double,
                  let kind = object["kind"] as? String, let text = object["text"] as? String,
                  let tone = object["tone"] as? String, ["info", "ok", "warn", "bad"].contains(tone) else { return nil }
            return Milestone(at: Date(timeIntervalSince1970: milliseconds / 1000), kind: kind,
                             text: text, tone: tone, slice: object["slice"] as? String,
                             branch: object["branch"] as? String)
        }
    }

    public static func unresolved(_ events: [Milestone]) -> [Milestone] {
        var blockers: [Milestone] = []
        for event in events.sorted(by: { $0.at < $1.at }) {
            if event.kind == "blocker" { blockers.append(event) }
            if event.kind == "resolved", let index = blockers.lastIndex(where: {
                event.slice != nil ? $0.slice?.lowercased() == event.slice?.lowercased() : $0.text == event.text
            }) { blockers.remove(at: index) }
        }
        return blockers
    }
}

private func json(_ line: String) -> [String: Any]? {
    (try? JSONSerialization.jsonObject(with: Data(line.utf8))) as? [String: Any]
}
