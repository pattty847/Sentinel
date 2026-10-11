import Foundation

// Read-only probes of the owner's dev box. Nothing here writes to the repo, the build queue or a service.

struct QueueEntry: Identifiable {
    let id: String
    let label: String
    let queuedAt: Date
    let running: Bool
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
        let live: [(String, String, Date)] = names.filter { $0.hasSuffix(".ticket") }.sorted().compactMap { name in
            let path = dir + "/" + name
            guard let mtime = (try? fm.attributesOfItem(atPath: path))?[.modificationDate] as? Date,
                  now.timeIntervalSince(mtime) <= 120 else { return nil }
            let text = (try? String(contentsOfFile: path, encoding: .utf8)) ?? ""
            let label = text.split(separator: "\n").first.map(String.init) ?? name
            let ns = Double(name.split(separator: "-").first ?? "") ?? 0
            return (name, label, Date(timeIntervalSince1970: ns / 1e9))
        }
        return live.enumerated().map { i, t in QueueEntry(id: t.0, label: t.1, queuedAt: t.2, running: i == 0) }
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
