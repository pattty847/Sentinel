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
                state.completed = false
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
            } else if type == "user", let message = object["message"] as? [String: Any],
                      let content = message["content"] as? [[String: Any]],
                      content.contains(where: { $0["is_error"] as? Bool == true }) {
                state.failed = true
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
