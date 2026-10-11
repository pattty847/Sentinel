import Foundation
import SentinelBarCore
import UserNotifications

@MainActor
final class Monitor: ObservableObject {
    @Published var queue: [QueueEntry] = []
    @Published var runningSince: [String: Date] = [:]
    @Published var builds: [BuildProcess] = []
    @Published var worktrees: [Worktree] = []
    @Published var agents: [AgentJob] = []
    @Published var services: [Service] = [
        Service(name: "server :8090", url: URL(string: "http://127.0.0.1:8090/metrics"), critical: true),
        Service(name: "capture :8091", url: URL(string: "http://127.0.0.1:8091/metrics"), critical: true),
        Service(name: "GUI host :17190", url: URL(string: "http://127.0.0.1:17190/status"), critical: false)
    ]
    @Published var budget: [String]? = nil
    @Published var budgetLoaded = false
    @Published var events: [Milestone] = []
    @Published var agentEvents: [Milestone] = []
    @Published var landings: [Landing] = []
    @Published var plan: Plan?
    @Published var lastRefresh: Date?
    @Published var showSettings = false
    @Published var repo: String
    @Published var pinnedPlan: String

    let queueDir = ProcessInfo.processInfo.environment["SENTINEL_BUILD_QUEUE"] ?? "/tmp/sentinel-build-queue"
    let launchAt = Date()
    let defaults: UserDefaults
    private var seenEvents: Set<String>?
    private var seenAgents: Set<String>?
    private var agentScanInProgress = false
    private var lastNotified: [String: Date] = [:]
    private var mainHead: String?
    private var selectedPlanPath: String?

    var blockers: [Milestone] { EventParser.unresolved(events) }
    var activity: [Milestone] {
        let gitEvents = landings.filter { landing in
            !events.contains { $0.kind == "landed" && $0.text.contains(landing.id) }
        }.map { landing in
            Milestone(at: landing.at, kind: "landed", text: "\(landing.slice) landed · \(landing.id)",
                      tone: "ok", slice: landing.slice, branch: nil)
        }
        return Array((events + gitEvents + agentEvents).sorted { $0.at > $1.at }.prefix(15))
    }
    var alert: Bool {
        services.contains { $0.critical && $0.up == false } ||
        blockers.contains { Date().timeIntervalSince($0.at) < 24 * 3600 }
    }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        repo = defaults.string(forKey: "repoPath") ?? NSHomeDirectory() + "/Programming/Sentinel"
        pinnedPlan = defaults.string(forKey: "pinnedPlan") ?? ""
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
        Task {
            while !Task.isCancelled {
                await refreshAgents()
                try? await Task.sleep(nanoseconds: 4_000_000_000)
            }
        }
        Task {
            while !Task.isCancelled {
                await refreshBuilds()
                await refreshEvents()
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
        Task {
            while !Task.isCancelled {
                await refreshServices()
                try? await Task.sleep(nanoseconds: 5_000_000_000)
            }
        }
        Task {
            while !Task.isCancelled {
                await refreshWorktreesAndPlan()
                try? await Task.sleep(nanoseconds: 10_000_000_000)
            }
        }
        Task {
            while !Task.isCancelled {
                await refreshBudget()
                try? await Task.sleep(nanoseconds: 300_000_000_000)
            }
        }
    }

    func setRepo(_ path: String) {
        repo = (path as NSString).expandingTildeInPath
        defaults.set(repo, forKey: "repoPath")
        mainHead = nil
        Task { await refresh() }
    }

    func setPlan(_ path: String) {
        pinnedPlan = path
        defaults.set(path, forKey: "pinnedPlan")
        mainHead = nil
        Task { await refreshWorktreesAndPlan() }
    }

    func refresh() async {
        await refreshWorktreesAndPlan()
        await refreshAgents()
        await refreshBuilds()
        await refreshEvents()
        await refreshServices()
        await refreshBudget()
    }

    private func refreshAgents() async {
        guard !agentScanInProgress else { return }
        agentScanInProgress = true
        defer { agentScanInProgress = false }
        let previous = seenAgents
        let trees = worktrees
        guard let scan = await Task.detached(operation: { AgentProbe.scan(worktrees: trees) }).value else { return }
        var fresh = scan.jobs
        let parentPIDs = Set(fresh.filter { !$0.subagent }.map(\.pid))
        let scannedIDs = Set(fresh.map(\.id))
        for child in agents where child.subagent && !scannedIDs.contains(child.id) &&
            parentPIDs.contains(child.pid) && scan.terminalSubagents[child.id] == nil {
            fresh.append(child)
        }
        let ids = Set(fresh.map(\.id))
        if let previous {
            for job in fresh where !previous.contains(job.id) {
                notify(kind: "agent started", agent: job.id, text: "\(job.kind) \(job.model ?? "") started")
                agentEvents.append(Milestone(at: Date(), kind: "agent_started", text: "\(job.kind) started · \(job.branch ?? "unmanaged")",
                    tone: "info", slice: nil, branch: job.branch))
            }
            for job in agents where !ids.contains(job.id) {
                let failed: Bool
                if job.subagent {
                    let disposition = scan.terminalSubagents[job.id] ??
                        SubagentLifecycle.decide(parentAlive: parentPIDs.contains(job.pid),
                            state: job.transcriptState,
                            secondsSinceActivity: job.lastActive.map { Date().timeIntervalSince($0) } ?? 0)
                    guard disposition.shouldNotify else { continue }
                    failed = disposition == .failed
                } else {
                    failed = job.failed
                }
                notify(kind: failed ? "agent failed" : "agent finished", agent: job.id,
                       text: "\(job.kind) \(job.model ?? "") finished")
                agentEvents.append(Milestone(at: Date(), kind: failed ? "agent_failed" : "agent_finished",
                    text: "\(job.kind) \(failed ? "failed" : "finished") · \(job.branch ?? "unmanaged")",
                    tone: failed ? "bad" : "ok", slice: nil, branch: job.branch))
            }
        }
        agentEvents = Array(agentEvents.suffix(30))
        agents = fresh
        seenAgents = ids
        lastRefresh = Date()
    }

    private func refreshBuilds() async {
        let dir = queueDir
        let fresh = await Task.detached {
            let processes = Probes.buildProcesses()
            return (Probes.buildQueue(dir: dir, processes: processes), processes)
        }.value
        let runningIDs = Set(fresh.0.filter(\.running).map(\.id))
        runningSince = runningSince.filter { runningIDs.contains($0.key) }
        for id in runningIDs where runningSince[id] == nil { runningSince[id] = Date() }
        queue = fresh.0
        builds = fresh.1
    }

    private func refreshEvents() async {
        let fresh = await Task.detached { Probes.events() }.value
        let ids = Set(fresh.map(\.id))
        let previous = seenEvents ?? []
        for event in fresh where !previous.contains(event.id) && event.at > launchAt {
            let kind = notificationKind(event.kind)
            if let kind { notify(kind: kind, agent: event.branch ?? event.slice ?? event.text, text: event.text) }
        }
        events = fresh
        seenEvents = ids
    }

    private func refreshServices() async {
        var fresh = services
        for index in fresh.indices {
            let old = fresh[index].up
            fresh[index].up = await Probes.isUp(fresh[index].url)
            if fresh[index].critical, let old, old != fresh[index].up {
                notify(kind: fresh[index].up == true ? "service back up" : "service down",
                       agent: fresh[index].name, text: fresh[index].name)
            }
        }
        services = fresh
    }

    private func refreshWorktreesAndPlan() async {
        let path = repo
        let trees = await Task.detached { Probes.worktrees(repo: path) }.value
        worktrees = trees
        let head = await Task.detached {
            Shell.run("/usr/bin/git", ["-C", path, "rev-parse", "main"])?
                .trimmingCharacters(in: .whitespacesAndNewlines)
        }.value
        if head != mainHead {
            let pinned = pinnedPlan
            let data = await Task.detached { () -> ([Landing], Plan?) in
                let landed = Probes.landings(repo: path)
                return (landed, Probes.plan(repo: path, pinned: pinned, landings: landed))
            }.value
            landings = data.0
            plan = data.1
            selectedPlanPath = pinned.isEmpty ? data.0.compactMap(\.plan).first : pinned
            mainHead = head
        }
    }

    private func refreshBudget() async {
        let path = repo
        budget = await Task.detached { Probes.budget(repo: path) }.value
        budgetLoaded = true
    }

    func landing(_ slice: String) -> Landing? {
        guard let selectedPlanPath else { return nil }
        return landings.first {
            guard let source = $0.plan else { return false }
            return normalizedPlan(source) == normalizedPlan(selectedPlanPath) &&
                PlanParser.sliceIDs(in: $0.slice).contains(slice.lowercased())
        }
    }

    private func normalizedPlan(_ path: String) -> String {
        let full = path.hasPrefix("/") ? path : repo + "/" + path
        return URL(fileURLWithPath: full).standardizedFileURL.path
    }

    func agentForSlice(_ slice: String) -> AgentJob? {
        agents.first { job in
            guard let branch = job.branch else { return false }
            return PlanParser.branchSlice(branch, ids: [slice.lowercased()]) != nil
        }
    }

    func milestoneForSlice(_ slice: String) -> Milestone? {
        events.first { PlanParser.sliceIDs(in: $0.slice ?? "").contains(slice.lowercased()) }
    }

    private func notificationKind(_ kind: String) -> String? {
        switch kind {
        case "landed": return "slice landed"
        case "land_failed": return "land failed"
        case "land_review": return "review"
        case "review": return "review"
        case "blocker": return "blocker raised"
        case "resolved": return "blocker resolved"
        case "plan": return "plan or phase change"
        case "build_failed": return "build failed"
        default: return nil
        }
    }

    private func notify(kind: String, agent: String, text: String) {
        let key = "notify." + kind
        let enabled = (defaults.object(forKey: key) as? Bool) ?? (kind != "agent started")
        guard enabled else { return }
        let throttleKey = kind + ":" + agent
        if let last = lastNotified[throttleKey], Date().timeIntervalSince(last) < 30 { return }
        lastNotified[throttleKey] = Date()
        let content = UNMutableNotificationContent()
        content.title = "SentinelBar · " + kind
        content.body = text
        content.sound = .default
        UNUserNotificationCenter.current().add(UNNotificationRequest(
            identifier: UUID().uuidString, content: content, trigger: nil))
    }
}
