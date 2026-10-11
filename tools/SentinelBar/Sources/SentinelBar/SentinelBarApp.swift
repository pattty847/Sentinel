import AppKit
import SentinelBarCore
import ServiceManagement
import SwiftUI

@main
struct SentinelBarApp: App {
    @StateObject private var monitor = Monitor(defaults: .standard)

    init() { NSApplication.shared.setActivationPolicy(.accessory) }

    var body: some Scene {
        MenuBarExtra {
            StatusPanel().environmentObject(monitor)
        } label: {
            HStack(spacing: 3) {
                if let glyph = menuGlyph(alert: monitor.alert) {
                    Image(nsImage: glyph).resizable().frame(width: 18, height: 18)
                }
                if !monitor.agents.isEmpty { Text("\(monitor.agents.count)") }
            }
        }
        .menuBarExtraStyle(.window)
    }

    private func menuGlyph(alert: Bool) -> NSImage? {
        let name = alert ? "MenuGlyphAlert" : "MenuGlyph"
        guard let image = Bundle.main.image(forResource: name) else { return nil }
        image.isTemplate = true
        return image
    }
}

struct StatusPanel: View {
    @EnvironmentObject var monitor: Monitor
    @State private var showWorktrees = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                if !monitor.queue.isEmpty || monitor.builds.contains(where: \.outsideQueue) {
                    section("Running now") {
                        ForEach(monitor.queue.filter(\.running)) { item in
                            HStack(alignment: .top) {
                                Image(systemName: "hammer.fill")
                                VStack(alignment: .leading) {
                                    Text(item.label).bold()
                                    Text(item.command ?? "building").font(.caption).lineLimit(1)
                                    if let next = monitor.queue.first(where: { !$0.running }) {
                                        Text("next: \(next.label)").font(.caption).foregroundStyle(.secondary)
                                    }
                                }
                                Spacer()
                                Text(monitor.runningSince[item.id] ?? item.queuedAt, style: .timer)
                                    .monospacedDigit()
                            }
                        }
                        ForEach(monitor.builds.filter(\.outsideQueue)) { build in
                            Label("outside queue · \(build.command)", systemImage: "exclamationmark.triangle")
                                .font(.caption).foregroundStyle(.orange).lineLimit(2)
                        }
                    }
                }
                section("Plan") {
                    if let plan = monitor.plan {
                        Text(plan.title).bold().lineLimit(2)
                        let done = plan.phases.flatMap(\.slices).filter { monitor.landing($0.id) != nil }.count
                        let count = plan.phases.flatMap(\.slices).count
                        Text("\(phaseName(plan)) · \(done)/\(count)")
                            .font(.caption).foregroundStyle(.secondary)
                        ForEach(plan.phases) { phase in
                            let completed = phase.slices.allSatisfy { monitor.landing($0.id) != nil }
                            if completed {
                                Text("✓ \(phase.title) · \(phase.slices.count)/\(phase.slices.count)")
                                    .font(.subheadline).foregroundStyle(.secondary)
                            } else {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(phase.title).font(.subheadline).bold()
                                    ForEach(phase.slices) { slice in sliceRow(slice) }
                                }
                            }
                        }
                    } else {
                        dim("No plan pinned or named by the latest merge")
                    }
                }
                if !monitor.blockers.isEmpty {
                    section("Blockers") {
                        ForEach(monitor.blockers) { event in
                            Label(event.text, systemImage: "exclamationmark.octagon.fill")
                                .foregroundStyle(.red).lineLimit(3)
                        }
                    }
                }
                section("Agents") {
                    if monitor.agents.isEmpty { dim("none running") }
                    ForEach(monitor.agents) { agent in
                        AgentCard(job: agent, worktrees: monitor.worktrees, role: role(for: agent))
                    }
                }
                section("Activity") {
                    if monitor.activity.isEmpty { dim("No activity yet") }
                    ForEach(monitor.activity) { event in
                        HStack(alignment: .firstTextBaseline) {
                            Text(event.at, style: .time).monospacedDigit().foregroundStyle(.secondary)
                            Text(event.text).lineLimit(2)
                        }.font(.caption)
                    }
                }
                section("Infra") {
                    DisclosureGroup("Worktrees (\(monitor.worktrees.count))", isExpanded: $showWorktrees) {
                        ForEach(monitor.worktrees) { tree in
                            HStack {
                                Text(tree.branch).lineLimit(1)
                                Spacer()
                                if tree.missing { Text("offline") }
                                if let ahead = tree.ahead { Text("+\(ahead)") }
                                Text(tree.head).monospaced()
                            }.font(.caption).foregroundStyle(.secondary).help(tree.path)
                        }
                    }
                    ForEach(monitor.services) { service in
                        HStack {
                            Circle().fill(service.up == nil ? .gray : service.up == true ? .green : .red)
                                .frame(width: 7, height: 7)
                            Text(service.name)
                            Spacer()
                            Text(service.up == nil ? "…" : service.up == true ? "up" : "down")
                                .foregroundStyle(.secondary)
                        }.font(.caption)
                    }
                    if !monitor.budgetLoaded { dim("Budget loading…") }
                    else if let budget = monitor.budget {
                        ForEach(budget, id: \.self) { Text($0).font(.caption.monospaced()) }
                    } else { dim("Budget unavailable") }
                }
                Divider()
                footer
            }
            .padding(12)
        }
        .frame(width: 420)
        .frame(maxHeight: 640)
        .sheet(isPresented: $monitor.showSettings) {
            SettingsPanel(monitor: monitor)
        }
    }

    private func phaseName(_ plan: Plan) -> String {
        plan.phases.first { !$0.slices.allSatisfy { monitor.landing($0.id) != nil } }?.title
            ?? "Complete"
    }

    private func role(for agent: AgentJob) -> String? {
        guard let branch = agent.branch else { return nil }
        if monitor.events.contains(where: { $0.kind == "review" && $0.branch == branch }) { return "reviewer" }
        let ids = monitor.plan?.phases.flatMap(\.slices).map { $0.id.lowercased() } ?? []
        return PlanParser.branchSlice(branch, ids: ids) == nil ? nil : "writer"
    }

    private func sliceRow(_ slice: PlanSlice) -> some View {
        let landed = monitor.landing(slice.id)
        let agent = monitor.agentForSlice(slice.id)
        return VStack(alignment: .leading, spacing: 1) {
            HStack(alignment: .firstTextBaseline) {
                Text(landed != nil ? "✓" : agent != nil ? "◐" : "○")
                Text("\(slice.id): \(slice.title)").lineLimit(2)
                Spacer()
                if let landed { Text(landed.id).font(.caption.monospaced()).foregroundStyle(.secondary) }
            }.font(.caption)
            if let agent {
                Text("\(agent.kind == "codex" ? "⌘" : "✦") \(agent.model ?? agent.kind) · \(agent.idle ? "idle" : "running") · \(agent.step ?? "working")")
                    .font(.caption2).foregroundStyle(.secondary).lineLimit(2).padding(.leading, 20)
                if let event = monitor.milestoneForSlice(slice.id) {
                    Text(event.text).font(.caption2).foregroundStyle(.secondary).padding(.leading, 20)
                }
            }
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Button("Grafana") {
                    if let url = URL(string: "http://127.0.0.1:3000") { open(url) }
                }
                Button("Logs") { open(URL(fileURLWithPath: NSHomeDirectory() + "/Library/Logs/Sentinel")) }
                Button("Terminal") { openTerminal(monitor.repo) }
                Spacer()
                Button("Settings") { monitor.showSettings = true }
            }
            HStack {
                Button("Refresh") { Task { await monitor.refresh() } }
                Button("Quit") { NSApp.terminate(nil) }
                Spacer()
                if let updated = monitor.lastRefresh {
                    Text("updated \(updated.formatted(date: .omitted, time: .standard))")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }.font(.caption)
    }

    private func section<C: View>(_ title: String, @ViewBuilder content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title).font(.headline)
            content()
        }
    }

    private func dim(_ text: String) -> some View { Text(text).foregroundStyle(.secondary).font(.caption) }
    private func open(_ url: URL) { NSWorkspace.shared.open(url) }
}

struct AgentCard: View {
    let job: AgentJob
    let worktrees: [Worktree]
    let role: String?

    var body: some View {
        Button {
            if NSApp.currentEvent?.modifierFlags.contains(.option) == true, let transcript = job.transcript {
                NSWorkspace.shared.open(transcript)
            } else if let tree = worktrees.first(where: { $0.branch == job.branch }) {
                openTerminal(tree.path)
            }
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                HStack {
                    Image(systemName: job.kind == "codex" ? "chevron.left.forwardslash.chevron.right" : "sparkle")
                    Text(job.model ?? job.kind).bold()
                    if job.subagent { Text("subagent") }
                    if let role { Text(role) }
                    Text(job.branch ?? "unmanaged").lineLimit(1).truncationMode(.middle)
                    Spacer()
                    if let time = job.lastActive { Text(time, style: .relative).monospacedDigit() }
                }
                Text("\(job.idle ? "idle" : "running") · \(job.step ?? "no transcript step")")
                    .lineLimit(2).foregroundStyle(.secondary)
                if let tokens = job.tokens { Text("\(tokens) tokens").foregroundStyle(.secondary) }
            }.font(.caption).frame(maxWidth: .infinity, alignment: .leading)
        }.buttonStyle(.plain).help("Click: Terminal · Option-click: transcript")
    }
}

func openTerminal(_ path: String) {
    let terminal = URL(fileURLWithPath: "/System/Applications/Utilities/Terminal.app")
    NSWorkspace.shared.open([URL(fileURLWithPath: path)], withApplicationAt: terminal,
                            configuration: NSWorkspace.OpenConfiguration())
}

struct SettingsPanel: View {
    @ObservedObject var monitor: Monitor
    @Environment(\.dismiss) private var dismiss
    @State private var repo: String = ""
    @State private var pinned: String = ""
    @State private var login = SMAppService.mainApp.status == .enabled
    private let kinds = ["agent started", "agent finished", "agent failed", "slice landed",
                         "land failed", "review", "blocker raised", "blocker resolved",
                         "plan or phase change", "build failed", "service down", "service back up"]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("SentinelBar settings").font(.headline)
            TextField("Repository", text: $repo)
            TextField("Pinned plan (optional)", text: $pinned)
            Toggle("Open at login", isOn: $login).onChange(of: login) { enabled in
                do {
                    if enabled { try SMAppService.mainApp.register() }
                    else { try SMAppService.mainApp.unregister() }
                } catch { login = SMAppService.mainApp.status == .enabled }
            }
            Text("Notifications").font(.subheadline).bold()
            ForEach(kinds, id: \.self) { kind in
                NotificationToggle(kind: kind, defaults: monitor.defaults)
            }
            HStack {
                Spacer()
                Button("Done") {
                    monitor.setRepo(repo)
                    monitor.setPlan(pinned)
                    dismiss()
                }
            }
        }
        .padding(16).frame(width: 380)
        .onAppear { repo = monitor.repo; pinned = monitor.pinnedPlan }
    }
}

struct NotificationToggle: View {
    let kind: String
    let defaults: UserDefaults
    @State private var enabled = true

    var body: some View {
        Toggle(kind, isOn: $enabled).onAppear {
            enabled = (defaults.object(forKey: "notify." + kind) as? Bool) ?? (kind != "agent started")
        }.onChange(of: enabled) { defaults.set($0, forKey: "notify." + kind) }
    }
}
