import AppKit
import ServiceManagement
import SwiftUI

@MainActor
final class Monitor: ObservableObject {
    @Published var queue: [QueueEntry] = []
    @Published var worktrees: [Worktree] = []
    @Published var agents: [AgentJob] = []
    @Published var services: [Service]
    @Published var budget: [String]? = nil
    @Published var budgetLoaded = false
    @Published var lastRefresh: Date? = nil

    let repo: String
    let queueDir: String

    var alert: Bool { services.contains { $0.critical && $0.up == false } }

    init() {
        let env = ProcessInfo.processInfo.environment
        repo = UserDefaults.standard.string(forKey: "repoPath") ?? env["SENTINEL_REPO"]
            ?? NSHomeDirectory() + "/Programming/Sentinel"
        queueDir = env["SENTINEL_BUILD_QUEUE"] ?? "/tmp/sentinel-build-queue"
        services = [
            Service(name: "sentinel-server :8090", url: URL(string: "http://127.0.0.1:8090/metrics")!, critical: true),
            Service(name: "capture :8091", url: URL(string: "http://127.0.0.1:8091/metrics")!, critical: true),
            Service(name: "GUI host :17190", url: URL(string: "http://127.0.0.1:17190/status")!, critical: false),
        ]
        Task { while !Task.isCancelled { await refresh(); try? await Task.sleep(nanoseconds: 5_000_000_000) } }
        Task { while !Task.isCancelled { await refreshBudget(); try? await Task.sleep(nanoseconds: 300_000_000_000) } }
    }

    func refresh() async {
        let repo = repo, dir = queueDir
        async let q = Task.detached { Probes.buildQueue(dir: dir) }.value
        let w = await Task.detached { Probes.worktrees(repo: repo) }.value
        async let a = Task.detached { AgentProbe.scan(worktrees: w) }.value
        var updated = services
        for i in updated.indices { updated[i].up = await Probes.isUp(updated[i].url) }
        queue = await q
        worktrees = w
        agents = await a
        services = updated
        lastRefresh = Date()
    }

    func refreshBudget() async {
        let repo = repo
        budget = await Task.detached { Probes.budget(repo: repo) }.value
        budgetLoaded = true
    }
}

@main
struct SentinelBarApp: App {
    @StateObject private var monitor = Monitor()

    init() { NSApplication.shared.setActivationPolicy(.accessory) }

    var body: some Scene {
        MenuBarExtra {
            StatusPanel().environmentObject(monitor)
        } label: {
            BarLabel(monitor: monitor)
        }
        .menuBarExtraStyle(.window)
    }
}

struct BarLabel: View {
    @ObservedObject var monitor: Monitor

    private var labelText: String {
        var parts: [String] = []
        if !monitor.agents.isEmpty { parts.append("\(monitor.agents.count)") }
        if !monitor.queue.isEmpty { parts.append("q\(monitor.queue.count)") }
        return parts.joined(separator: " ")
    }

    var body: some View {
        if monitor.alert {
            Image(systemName: "exclamationmark.triangle.fill")
        } else if monitor.agents.isEmpty && monitor.queue.isEmpty {
            Image(systemName: "waveform.path.ecg")
        } else {
            // One string: menu bar labels render a single image plus text reliably, not nested stacks.
            Label(labelText, systemImage: monitor.queue.isEmpty ? "cpu" : "hammer.fill")
                .labelStyle(.titleAndIcon)
        }
    }
}

struct StatusPanel: View {
    @EnvironmentObject var m: Monitor

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            section("Agents") {
                if m.agents.isEmpty { dim("none running") }
                ForEach(m.agents) { a in AgentRow(job: a) }
            }
            section("Build queue") {
                if m.queue.isEmpty { dim("empty") }
                ForEach(m.queue) { e in
                    HStack {
                        Image(systemName: e.running ? "play.circle.fill" : "clock")
                            .foregroundStyle(e.running ? Color.green : Color.secondary)
                        Text(e.label).lineLimit(1).truncationMode(.middle)
                        Spacer()
                        Text(e.queuedAt, style: .relative).monospacedDigit().foregroundStyle(.secondary)
                    }
                }
            }
            section("Worktrees") {
                ForEach(m.worktrees) { w in
                    HStack {
                        Text(w.branch).lineLimit(1).truncationMode(.middle)
                            .foregroundStyle(w.missing ? Color.secondary : Color.primary)
                        Spacer()
                        if w.missing { dim("drive offline") }
                        if let a = w.ahead { dim("+\(a)") }
                        Text(w.head).font(.caption.monospaced()).foregroundStyle(.secondary)
                    }
                    .help(w.path)
                }
            }
            section("Services") {
                ForEach(m.services) { s in
                    HStack {
                        Circle().fill(color(s)).frame(width: 8, height: 8)
                        Text(s.name)
                        Spacer()
                        dim(s.up == nil ? "..." : s.up! ? "up" : (s.critical ? "DOWN" : "off"))
                    }
                }
            }
            section("Budget") {
                if !m.budgetLoaded { dim("loading...") }
                else if let lines = m.budget, !lines.isEmpty {
                    ForEach(lines, id: \.self) { Text($0).font(.caption.monospaced()) }
                } else { dim("CodexBar CLI unavailable") }
            }
            Divider()
            HStack {
                Button("Grafana") { open("http://127.0.0.1:3000") }
                Button("Logs") { NSWorkspace.shared.open(URL(fileURLWithPath: NSHomeDirectory() + "/Library/Logs/Sentinel")) }
                Button("Terminal") { openTerminal() }
                Spacer()
                Button { Task { await m.refresh(); await m.refreshBudget() } } label: { Image(systemName: "arrow.clockwise") }
                Button("Quit") { NSApp.terminate(nil) }
            }
            HStack {
                LoginToggle()
                Spacer()
                if let t = m.lastRefresh { dim("updated \(t.formatted(date: .omitted, time: .standard))") }
            }
        }
        .padding(12)
        .frame(width: 420)
    }

    private func section<C: View>(_ title: String, @ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.headline)
            content()
        }
    }

    private func dim(_ s: String) -> some View { Text(s).foregroundStyle(.secondary) }

    private func color(_ s: Service) -> Color {
        guard let up = s.up else { return .gray }
        return up ? .green : (s.critical ? .red : .gray)
    }

    private func open(_ url: String) { NSWorkspace.shared.open(URL(string: url)!) }

    private func openTerminal() {
        let terminal = URL(fileURLWithPath: "/System/Applications/Utilities/Terminal.app")
        NSWorkspace.shared.open([URL(fileURLWithPath: m.repo)], withApplicationAt: terminal,
                                configuration: NSWorkspace.OpenConfiguration())
    }
}

/// Open-at-login through SMAppService; only works from the bundled .app, not `swift run`.
struct LoginToggle: View {
    @State private var on = SMAppService.mainApp.status == .enabled

    var body: some View {
        Toggle("Open at login", isOn: $on)
            .toggleStyle(.checkbox)
            .onChange(of: on) { enable in
                if enable { try? SMAppService.mainApp.register() } else { try? SMAppService.mainApp.unregister() }
                on = SMAppService.mainApp.status == .enabled
            }
    }
}

struct AgentRow: View {
    let job: AgentJob

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            HStack {
                Image(systemName: job.kind == "codex" ? "chevron.left.forwardslash.chevron.right" : "sparkle")
                    .foregroundStyle(job.kind == "codex" ? Color.blue : Color.orange)
                Text(job.model.map { "\(job.kind) \($0)" } ?? job.kind).bold()
                Text(job.worktree ?? (job.cwd as NSString).lastPathComponent)
                    .lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary)
                Spacer()
                if let t = job.lastActive { Text(t, style: .relative).monospacedDigit().foregroundStyle(.secondary) }
            }
            Text(job.step ?? "no transcript found").font(.caption).lineLimit(2)
                .foregroundStyle(job.step == nil ? HierarchicalShapeStyle.tertiary : HierarchicalShapeStyle.secondary)
        }
        .help("pid \(job.pid), up \(job.elapsed)\n\(job.cwd)")
    }
}
