import Foundation

// Running Claude Code and Codex jobs: found by process scan, placed by cwd, and described by the
// newest entry of their own session transcript (best effort; formats are the CLIs', not ours).

struct AgentJob: Identifiable {
    var id: Int32 { pid }
    let pid: Int32
    let kind: String          // "claude" | "codex"
    let model: String?
    let elapsed: String       // ps etime
    let cwd: String
    var worktree: String?     // branch of the worktree containing cwd
    var step: String?         // last tool call or message
    var lastActive: Date?     // transcript mtime
}

enum AgentProbe {
    static func scan(worktrees: [Worktree]) -> [AgentJob] {
        guard let ps = Shell.run("/bin/ps", ["-axo", "pid=,ppid=,etime=,command="]) else { return [] }
        var found: [(pid: Int32, ppid: Int32, etime: String, kind: String, args: [String])] = []
        for line in ps.split(separator: "\n") {
            let f = line.split(separator: " ", maxSplits: 3, omittingEmptySubsequences: true).map(String.init)
            guard f.count == 4, let pid = Int32(f[0]), let ppid = Int32(f[1]) else { continue }
            let args = f[3].split(separator: " ").map(String.init)
            guard let kind = kindOf(args) else { continue }
            found.append((pid, ppid, f[2], kind, args))
        }
        // Wrappers (node shim -> native binary) show up twice; keep the outermost process of each job.
        let kindByPid = Dictionary(found.map { ($0.pid, $0.kind) }, uniquingKeysWith: { a, _ in a })
        found.removeAll { kindByPid[$0.ppid] == $0.kind }
        guard !found.isEmpty else { return [] }
        let cwds = cwdsOf(found.map(\.pid))
        let codexIndex = CodexSessions.index()
        return found.compactMap { p in
            guard let cwd = cwds[p.pid] else { return nil }
            var job = AgentJob(pid: p.pid, kind: p.kind, model: modelArg(p.args), elapsed: p.etime, cwd: cwd)
            job.worktree = worktrees.filter { cwd == $0.path || cwd.hasPrefix($0.path + "/") }
                .max { $0.path.count < $1.path.count }?.branch
            let t = p.kind == "claude" ? ClaudeSessions.latest(cwd: cwd) : codexIndex.latest(cwd: cwd)
            job.step = t?.step
            job.lastActive = t?.mtime
            return job
        }
        .sorted { ($0.lastActive ?? .distantPast) > ($1.lastActive ?? .distantPast) }
    }

    private static func kindOf(_ args: [String]) -> String? {
        // argv0 may be the binary itself or an interpreter (node) running a script.
        for a in args.prefix(2) {
            let base = (a as NSString).lastPathComponent
            if base == "claude" { return "claude" }
            if base == "codex" || base.hasPrefix("codex-") { return "codex" }
        }
        return nil
    }

    private static func modelArg(_ args: [String]) -> String? {
        for (i, a) in args.enumerated() {
            if (a == "-m" || a == "--model"), i + 1 < args.count { return args[i + 1] }
            if a.hasPrefix("--model=") { return String(a.dropFirst(8)) }
        }
        return nil
    }

    private static func cwdsOf(_ pids: [Int32]) -> [Int32: String] {
        let list = pids.map(String.init).joined(separator: ",")
        guard let out = Shell.run("/usr/sbin/lsof", ["-a", "-d", "cwd", "-Fn", "-p", list], anyExit: true) else { return [:] }
        var result: [Int32: String] = [:]
        var current: Int32?
        for line in out.split(separator: "\n") {
            if line.hasPrefix("p") { current = Int32(line.dropFirst()) }
            if line.hasPrefix("n"), let pid = current { result[pid] = String(line.dropFirst()) }
        }
        return result
    }
}

struct TranscriptStep {
    let step: String?
    let mtime: Date
}

enum Transcript {
    /// Last `bytes` of a file as complete lines, newest first.
    static func tailLines(_ url: URL, bytes: Int = 256 * 1024) -> [Substring] {
        guard let h = try? FileHandle(forReadingFrom: url) else { return [] }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        try? h.seek(toOffset: size > UInt64(bytes) ? size - UInt64(bytes) : 0)
        guard let data = try? h.readToEnd(), let text = String(data: data, encoding: .utf8) else { return [] }
        return Array(text.split(separator: "\n").reversed())
    }

    static func json(_ line: Substring) -> [String: Any]? {
        (try? JSONSerialization.jsonObject(with: Data(line.utf8))) as? [String: Any]
    }

    static func newest(in dir: URL, ext: String = "jsonl") -> (URL, Date)? {
        let keys: [URLResourceKey] = [.contentModificationDateKey]
        guard let files = try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: keys) else { return nil }
        return files.filter { $0.pathExtension == ext }
            .compactMap { u -> (URL, Date)? in
                guard let d = try? u.resourceValues(forKeys: Set(keys)).contentModificationDate else { return nil }
                return (u, d)
            }
            .max { $0.1 < $1.1 }
    }

    static func oneLine(_ s: String, max: Int = 90) -> String {
        let flat = s.split(whereSeparator: \.isNewline).first.map(String.init) ?? s
        return flat.count > max ? String(flat.prefix(max - 1)) + "…" : flat
    }
}

/// Claude Code: ~/.claude/projects/<cwd with / and . as ->/<session>.jsonl
enum ClaudeSessions {
    static func latest(cwd: String) -> TranscriptStep? {
        let slug = cwd.map { $0 == "/" || $0 == "." ? "-" : $0 }.map(String.init).joined()
        let dir = URL(fileURLWithPath: NSHomeDirectory() + "/.claude/projects/" + slug)
        guard let (file, mtime) = Transcript.newest(in: dir) else { return nil }
        for line in Transcript.tailLines(file) {
            guard let obj = Transcript.json(line), obj["type"] as? String == "assistant",
                  let msg = obj["message"] as? [String: Any],
                  let content = msg["content"] as? [[String: Any]] else { continue }
            for item in content.reversed() {
                if item["type"] as? String == "tool_use", let name = item["name"] as? String {
                    let input = item["input"] as? [String: Any] ?? [:]
                    let detail = ["description", "command", "file_path", "pattern", "prompt"]
                        .lazy.compactMap { input[$0] as? String }.first
                    return TranscriptStep(step: Transcript.oneLine(detail.map { "\(name): \($0)" } ?? name), mtime: mtime)
                }
                if item["type"] as? String == "text", let text = item["text"] as? String, !text.isEmpty {
                    return TranscriptStep(step: Transcript.oneLine(text), mtime: mtime)
                }
            }
        }
        return TranscriptStep(step: nil, mtime: mtime)
    }
}

/// Codex: $CODEX_HOME/sessions/YYYY/MM/DD/rollout-*.jsonl; the first line's session_meta carries the cwd.
struct CodexSessions {
    private var byCwd: [String: (URL, Date)] = [:]

    static func index() -> CodexSessions {
        var idx = CodexSessions()
        let home = ProcessInfo.processInfo.environment["CODEX_HOME"] ?? NSHomeDirectory() + "/.codex"
        let cal = Calendar.current
        for back in 0...1 {   // today and yesterday cover a job that started before midnight
            guard let day = cal.date(byAdding: .day, value: -back, to: Date()) else { continue }
            let c = cal.dateComponents([.year, .month, .day], from: day)
            let dir = URL(fileURLWithPath: String(format: "%@/sessions/%04d/%02d/%02d", home, c.year!, c.month!, c.day!))
            let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
            for f in files where f.pathExtension == "jsonl" {
                guard let mtime = try? f.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate,
                      let cwd = firstLineCwd(f) else { continue }
                if let have = idx.byCwd[cwd], have.1 >= mtime { continue }
                idx.byCwd[cwd] = (f, mtime)
            }
        }
        return idx
    }

    private static func firstLineCwd(_ url: URL) -> String? {
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        guard let data = try? h.read(upToCount: 64 * 1024), let text = String(data: data, encoding: .utf8),
              let first = text.split(separator: "\n").first, let obj = Transcript.json(first) else { return nil }
        let payload = obj["payload"] as? [String: Any] ?? obj
        return payload["cwd"] as? String
    }

    func latest(cwd: String) -> TranscriptStep? {
        guard let (file, mtime) = byCwd[cwd] else { return nil }
        for line in Transcript.tailLines(file) {
            guard let obj = Transcript.json(line), let p = obj["payload"] as? [String: Any],
                  let type = p["type"] as? String else { continue }
            switch type {
            case "function_call", "custom_tool_call", "local_shell_call":
                let name = p["name"] as? String ?? "shell"
                var detail = p["arguments"] as? String ?? p["input"] as? String ?? ""
                if let args = (try? JSONSerialization.jsonObject(with: Data(detail.utf8))) as? [String: Any] {
                    if let cmd = args["command"] as? [String] { detail = cmd.joined(separator: " ") }
                    else if let cmd = args["command"] as? String { detail = cmd }
                }
                return TranscriptStep(step: Transcript.oneLine("\(name): \(detail)"), mtime: mtime)
            case "message" where p["role"] as? String == "assistant":
                let parts = p["content"] as? [[String: Any]] ?? []
                if let text = parts.compactMap({ $0["text"] as? String }).last, !text.isEmpty {
                    return TranscriptStep(step: Transcript.oneLine(text), mtime: mtime)
                }
            case "agent_message", "agent_reasoning":
                if let text = p["message"] as? String ?? p["text"] as? String, !text.isEmpty {
                    return TranscriptStep(step: Transcript.oneLine(text), mtime: mtime)
                }
            default:
                continue
            }
        }
        return TranscriptStep(step: nil, mtime: mtime)
    }
}
