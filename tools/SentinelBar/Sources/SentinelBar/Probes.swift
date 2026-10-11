import Foundation
import SentinelBarCore

// Read-only probes of the owner's dev box. Nothing here writes to the repo, the build queue or a service.

struct QueueEntry: Identifiable {
    let id: String
    let label: String
    let queuedAt: Date
    let running: Bool
    let command: String?
}

struct BuildProcess: Identifiable {
    let id: Int32
    let command: String
    let outsideQueue: Bool
}

struct Landing: Identifiable {
    let id: String
    let slice: String
    let plan: String?
    let at: Date
}

struct Worktree: Identifiable {
    var id: String { path }
    let path: String
    let branch: String
    let head: String
    let ahead: Int?
    let missing: Bool
}

struct Service: Identifiable {
    var id: String { name }
    let name: String
    let url: URL
    let critical: Bool
    var up: Bool?
}

enum Shell {
    /// Runs a command and returns stdout, or nil if it failed to start, timed out or exited non-zero
    /// (`anyExit` keeps the output of a non-zero exit, e.g. lsof when one pid has gone).
    static func run(_ exe: String, _ args: [String], cwd: String? = nil, timeout: TimeInterval = 15,
                    anyExit: Bool = false) -> String? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        if let cwd { p.currentDirectoryURL = URL(fileURLWithPath: cwd) }
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:" + (env["PATH"] ?? "")
        p.environment = env
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return nil }
        DispatchQueue.global().asyncAfter(deadline: .now() + timeout) { if p.isRunning { p.terminate() } }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        guard p.terminationReason == .exit, anyExit || p.terminationStatus == 0 else { return nil }
        return String(data: data, encoding: .utf8)
    }
}

enum Probes {
    /// Mirrors `build-queue.sh status`: tickets sorted by name (enqueue time in ns), first live one runs.
    /// Tickets without a heartbeat for 120 s are dead; the script prunes them, we only skip them.
    static func buildQueue(dir: String) -> [QueueEntry] {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: dir) else { return [] }
        let now = Date()
        let tickets: [Ticket] = names.filter { $0.hasSuffix(".ticket") }.compactMap { name in
            let path = dir + "/" + name
            guard let mtime = (try? fm.attributesOfItem(atPath: path))?[.modificationDate] as? Date,
                  now.timeIntervalSince(mtime) <= 120 else { return nil }
            let text = (try? String(contentsOfFile: path, encoding: .utf8)) ?? ""
            let label = text.split(separator: "\n").first.map(String.init) ?? name
            let ns = Double(name.split(separator: "-").first ?? "") ?? 0
            return Ticket(id: name, label: label, queuedAt: Date(timeIntervalSince1970: ns / 1e9), modifiedAt: mtime)
        }
        let processes = buildProcesses()
        return TicketParser.live(tickets, now: now).map { ticket in
            QueueEntry(id: ticket.id, label: ticket.label, queuedAt: ticket.queuedAt,
                       running: ticket.running, command: ticket.running ? processes.first { !$0.outsideQueue }?.command : nil)
        }
    }

    static func buildProcesses() -> [BuildProcess] {
        guard let output = Shell.run("/bin/ps", ["-axo", "pid=,ppid=,command="]) else { return [] }
        var commands: [Int32: (Int32, String)] = [:]
        for line in output.split(separator: "\n") {
            let fields = line.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: true)
            guard fields.count == 3, let pid = Int32(fields[0]), let parent = Int32(fields[1]) else { continue }
            commands[pid] = (parent, String(fields[2]))
        }
        return commands.compactMap { pid, value in
            let command = value.1
            let isBuild = command.contains("cmake --build") || command.contains("ninja ") ||
                command.contains("ctest ") || command.contains("swift build")
            guard isBuild, !command.contains("build-queue.sh") else { return nil }
            var parent = value.0
            var queued = false
            for _ in 0..<8 {
                guard let ancestor = commands[parent] else { break }
                if ancestor.1.contains("build-queue.sh") { queued = true; break }
                parent = ancestor.0
            }
            return BuildProcess(id: pid, command: String(command.prefix(100)), outsideQueue: !queued)
        }.sorted { $0.id < $1.id }
    }

    static func landings(repo: String) -> [Landing] {
        guard let output = Shell.run("/usr/bin/git", ["-C", repo, "log", "--first-parent", "main",
            "--format=%h%x1f%ct%x1f%(trailers:only,unfold)%x1e"]) else { return [] }
        return output.components(separatedBy: "\u{1e}").compactMap { record in
            let parts = record.trimmingCharacters(in: .whitespacesAndNewlines).components(separatedBy: "\u{1f}")
            guard parts.count == 3, let seconds = TimeInterval(parts[1]) else { return nil }
            let trailers = parts[2].components(separatedBy: "\n")
            guard let slice = trailers.first(where: { $0.hasPrefix("Slice: ") }).map({ String($0.dropFirst(7)) }) else { return nil }
            let plan = trailers.first(where: { $0.hasPrefix("Plan: ") }).map { String($0.dropFirst(6)) }
            return Landing(id: parts[0], slice: slice, plan: plan, at: Date(timeIntervalSince1970: seconds))
        }
    }

    static func plan(repo: String, pinned: String?, landings: [Landing]) -> Plan? {
        let path = pinned.flatMap { $0.isEmpty ? nil : $0 } ?? landings.compactMap(\.plan).first
        guard let path else { return nil }
        let url = path.hasPrefix("/") ? URL(fileURLWithPath: path) : URL(fileURLWithPath: repo).appendingPathComponent(path)
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        return PlanParser.parse(text)
    }

    static func events() -> [Milestone] {
        let files = [NSHomeDirectory() + "/Library/Application Support/Sentinel/events.jsonl", "/tmp/sentinel-events.jsonl"]
        return files.flatMap { EventParser.parse(TranscriptFiles.tail(URL(fileURLWithPath: $0))) }
            .sorted { $0.at > $1.at }
    }

    static func worktrees(repo: String) -> [Worktree] {
        let git = ["--no-optional-locks", "-C", repo]
        guard let out = Shell.run("/usr/bin/git", git + ["worktree", "list", "--porcelain"]) else { return [] }
        var result: [Worktree] = []
        for block in out.components(separatedBy: "\n\n") {
            var path = "", head = "", branch = "(detached)"
            for line in block.split(separator: "\n") {
                if line.hasPrefix("worktree ") { path = String(line.dropFirst(9)) }
                if line.hasPrefix("HEAD ") { head = String(line.dropFirst(5).prefix(7)) }
                if line.hasPrefix("branch ") { branch = String(line.dropFirst(7)).replacingOccurrences(of: "refs/heads/", with: "") }
            }
            guard !path.isEmpty else { continue }
            var ahead: Int? = nil
            if branch != "main", branch != "(detached)",
               let n = Shell.run("/usr/bin/git", git + ["rev-list", "--count", "main..\(branch)"]) {
                ahead = Int(n.trimmingCharacters(in: .whitespacesAndNewlines))
            }
            result.append(Worktree(path: path, branch: branch, head: head, ahead: ahead,
                                   missing: !FileManager.default.fileExists(atPath: path)))
        }
        return result
    }

    static func isUp(_ url: URL) async -> Bool {
        var req = URLRequest(url: url)
        req.timeoutInterval = 2
        guard let (_, resp) = try? await URLSession.shared.data(for: req),
              let http = resp as? HTTPURLResponse else { return false }
        return (200..<300).contains(http.statusCode)
    }

    /// `budget.sh` text lines (it shells out to the CodexBar CLI), or nil when it fails.
    static func budget(repo: String) -> [String]? {
        guard let out = Shell.run("/bin/bash", [repo + "/scripts/dev/budget.sh"], cwd: repo, timeout: 60) else { return nil }
        return out.split(separator: "\n").map(String.init).filter { !$0.isEmpty }
    }
}
