import Foundation
import SentinelBarCore

struct AgentJob: Identifiable {
    let id: String
    let pid: Int32
    let kind: String
    var model: String?
    let cwd: String
    var branch: String?
    var transcript: URL?
    var step: String?
    var tokens: Int?
    var lastActive: Date?
    var failed = false
    var subagent = false
    var transcriptState = TranscriptState()
    var idle: Bool { lastActive.map { Date().timeIntervalSince($0) > 120 } ?? false }
}

struct AgentScan {
    let jobs: [AgentJob]
    let terminalSubagents: [String: SubagentDisposition]
}

enum TranscriptFiles {
    private static let cacheLock = NSLock()
    private static var states: [String: (Date, TranscriptState)] = [:]
    private static var cwds: [String: String] = [:]

    static func tail(_ url: URL) -> [String] {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return [] }
        defer { try? handle.close() }
        let size = (try? handle.seekToEnd()) ?? 0
        let offset = size > 256 * 1024 ? size - 256 * 1024 : 0
        try? handle.seek(toOffset: offset)
        guard let data = try? handle.read(upToCount: 256 * 1024) else { return [] }
        let text = String(decoding: data, as: UTF8.self)
        var lines = text.components(separatedBy: "\n")
        if offset > 0 && !lines.isEmpty { lines.removeFirst() }
        if !text.hasSuffix("\n") && !lines.isEmpty { lines.removeLast() }
        return lines
    }

    static func state(_ url: URL, _ date: Date, kind: String) -> TranscriptState {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        if let cached = states[url.path], cached.0 == date { return cached.1 }
        let state = kind == "codex" ? TranscriptParser.codex(tail(url)) : TranscriptParser.claude(tail(url))
        states[url.path] = (date, state)
        return state
    }

    static func codexCwd(_ url: URL) -> String? {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        if let cached = cwds[url.path] { return cached }
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        guard let data = try? handle.read(upToCount: 65536),
              let first = String(data: data, encoding: .utf8)?.components(separatedBy: "\n").first,
              let cwd = TranscriptParser.codexCwd(first) else { return nil }
        cwds[url.path] = cwd
        return cwd
    }

    static func codexFiles() -> [AgentTranscript] {
        var result: [AgentTranscript] = []
        let home = ProcessInfo.processInfo.environment["CODEX_HOME"] ?? NSHomeDirectory() + "/.codex"
        let calendar = Calendar.current
        for back in 0...1 {
            guard let day = calendar.date(byAdding: .day, value: -back, to: Date()) else { continue }
            let parts = calendar.dateComponents([.year, .month, .day], from: day)
            guard let year = parts.year, let month = parts.month, let day = parts.day else { continue }
            let path = String(format: "%@/sessions/%04d/%02d/%02d", home, year, month, day)
            let files = (try? FileManager.default.contentsOfDirectory(at: URL(fileURLWithPath: path),
                includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
            for url in files where url.lastPathComponent.hasPrefix("rollout-") && url.pathExtension == "jsonl" {
                guard let cwd = codexCwd(url), let date = modified(url) else { continue }
                result.append(AgentTranscript(path: url.path, cwd: cwd, kind: "codex", modified: date))
            }
        }
        return result
    }

    static func claudeFiles(cwd: String) -> [AgentTranscript] {
        let slug = cwd.map { $0 == "/" || $0 == "." ? "-" : $0 }.map(String.init).joined()
        let root = URL(fileURLWithPath: NSHomeDirectory() + "/.claude/projects/" + slug)
        let mains = ((try? FileManager.default.contentsOfDirectory(at: root,
            includingPropertiesForKeys: [.contentModificationDateKey])) ?? []).filter { $0.pathExtension == "jsonl" }
        return mains.compactMap { url in
            modified(url).map { AgentTranscript(path: url.path, cwd: cwd, kind: "claude", modified: $0) }
        }
    }

    static func claudeChildren(main: URL) -> [(URL, Date)] {
        let dir = main.deletingLastPathComponent().appendingPathComponent(main.deletingPathExtension().lastPathComponent)
            .appendingPathComponent("subagents")
        let files = (try? FileManager.default.contentsOfDirectory(at: dir,
            includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        return files.filter { $0.pathExtension == "jsonl" }
            .compactMap { url in modified(url).map { (url, $0) } }
    }

    private static func modified(_ url: URL) -> Date? {
        try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate
    }
}

enum AgentProbe {
    static func scan(worktrees: [Worktree]) -> AgentScan? {
        guard let output = Shell.run("/bin/ps", ["-axo", "pid=,ppid=,command="]) else { return nil }
        var processes: [(pid: Int32, parent: Int32, kind: String, args: [String])] = []
        for line in output.split(separator: "\n") {
            let f = line.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: true).map(String.init)
            guard f.count == 3, let pid = Int32(f[0]), let parent = Int32(f[1]) else { continue }
            let args = f[2].split(separator: " ").map(String.init)
            guard let kind = kindOf(args) else { continue }
            processes.append((pid, parent, kind, args))
        }
        let kinds = Dictionary(processes.map { ($0.pid, $0.kind) }, uniquingKeysWith: { a, _ in a })
        processes.removeAll { kinds[$0.parent] == $0.kind }
        guard let cwds = cwdsOf(processes.map(\.pid)) else { return nil }
        let descriptors: [AgentProcess] = processes.compactMap { process in
            let fallback = cwds[process.pid] ?? "/"
            let provisional = AgentProcess(pid: process.pid, kind: process.kind, cwd: fallback,
                                           args: process.args, hasWorktree: false)
            let cwd = AgentMatcher.cwd(for: provisional)
            guard cwds[process.pid] != nil || (process.kind == "codex" && cwd != "/") else { return nil }
            let managed = worktrees.contains { cwd == $0.path || cwd.hasPrefix($0.path + "/") }
            return AgentProcess(pid: process.pid, kind: process.kind, cwd: fallback,
                                args: process.args, hasWorktree: managed)
        }
        let claudeCwds = Set(descriptors.filter { $0.kind == "claude" }.map { AgentMatcher.cwd(for: $0) })
        let transcripts = TranscriptFiles.codexFiles() + claudeCwds.flatMap { TranscriptFiles.claudeFiles(cwd: $0) }
        let selected = AgentMatcher.select(processes: descriptors, transcripts: transcripts, now: Date())
        let byPID = Dictionary(processes.map { ($0.pid, $0) }, uniquingKeysWith: { a, _ in a })
        let descriptorByPID = Dictionary(descriptors.map { ($0.pid, $0) }, uniquingKeysWith: { a, _ in a })
        var jobs: [AgentJob] = []
        var terminalSubagents: [String: SubagentDisposition] = [:]
        for match in selected {
            guard let p = byPID[match.pid], let descriptor = descriptorByPID[match.pid] else { continue }
            let cwd = AgentMatcher.cwd(for: descriptor)
            let branch = worktrees.filter { cwd == $0.path || cwd.hasPrefix($0.path + "/") }
                .max { $0.path.count < $1.path.count }?.branch
            let id = p.kind == "claude" ? (match.transcript?.path ?? String(p.pid)) : String(p.pid)
            var job = AgentJob(id: id, pid: p.pid, kind: p.kind,
                               model: AgentMatcher.modelArgument(p.args), cwd: cwd, branch: branch)
            if let transcript = match.transcript {
                let url = URL(fileURLWithPath: transcript.path)
                apply(url, transcript.modified, to: &job)
                if p.kind == "claude" {
                    for (url, date) in TranscriptFiles.claudeChildren(main: url) {
                        var child = AgentJob(id: url.path, pid: p.pid, kind: "claude",
                                             model: nil, cwd: cwd, branch: branch, subagent: true)
                        apply(url, date, to: &child)
                        let disposition = SubagentLifecycle.decide(parentAlive: true, state: child.transcriptState,
                            secondsSinceActivity: Date().timeIntervalSince(date))
                        if disposition == .active { jobs.append(child) }
                        else { terminalSubagents[child.id] = disposition }
                    }
                }
            }
            jobs.append(job)
        }
        return AgentScan(jobs: jobs.sorted { ($0.lastActive ?? .distantPast) > ($1.lastActive ?? .distantPast) },
                         terminalSubagents: terminalSubagents)
    }

    private static func apply(_ url: URL, _ date: Date, to job: inout AgentJob) {
        let state = TranscriptFiles.state(url, date, kind: job.kind)
        job.transcript = url
        job.model = job.model ?? state.model
        job.step = state.step
        job.tokens = state.tokens
        job.lastActive = date
        job.failed = state.failed
        job.transcriptState = state
    }

    private static func kindOf(_ args: [String]) -> String? {
        for arg in args.prefix(2) {
            let name = (arg as NSString).lastPathComponent
            if name == "claude" { return "claude" }
            if name == "codex" || name.hasPrefix("codex-") { return "codex" }
        }
        return nil
    }

    private static func cwdsOf(_ pids: [Int32]) -> [Int32: String]? {
        guard !pids.isEmpty else { return [:] }
        guard let output = Shell.run("/usr/sbin/lsof",
            ["-a", "-d", "cwd", "-Fn", "-p", pids.map(String.init).joined(separator: ",")], anyExit: true) else { return nil }
        var result: [Int32: String] = [:]
        var current: Int32?
        for line in output.split(separator: "\n") {
            if line.hasPrefix("p") { current = Int32(line.dropFirst()) }
            if line.hasPrefix("n"), let current { result[current] = String(line.dropFirst()) }
        }
        return result
    }
}
