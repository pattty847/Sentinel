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
                if monitor.runningAgentCount > 0 { Text("\(monitor.runningAgentCount)") }
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
    // MenuBarExtra windows size to the ScrollView's ideal height, which is ~0: measure the content instead.
    @State private var contentHeight: CGFloat = 400

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 8) {
                    if let glyph = panelGlyph() {
                        Image(nsImage: glyph).resizable().frame(width: 20, height: 20)
                    }
                    Text("Sentinel").font(.title3.bold())
                    Spacer()
                    if let updated = monitor.lastRefresh {
                        Text("updated \(updated.formatted(date: .omitted, time: .shortened))")
                            .font(.caption).foregroundStyle(.secondary).monospacedDigit()
                    }
                }
                if !monitor.queue.isEmpty || monitor.builds.contains(where: \.outsideQueue) {
                    section("Running now") {
                        ForEach(monitor.queue.filter(\.running)) { item in
                            HStack(alignment: .top) {
                                Image(systemName: "hammer.fill").foregroundStyle(.secondary)
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(item.label).font(.callout.bold())
                                    Text(item.command ?? "building").font(.caption.monospaced())
                                        .foregroundStyle(.secondary).lineLimit(1)
                                    if let next = monitor.queue.first(where: { !$0.running }) {
                                        Text("next: \(next.label)").font(.caption).foregroundStyle(.secondary)
                                    }
                                }
                                Spacer()
                                Text(monitor.runningSince[item.id] ?? item.queuedAt, style: .timer)
                                    .font(.title3).monospacedDigit()
                            }
                        }
                        ForEach(monitor.builds.filter(\.outsideQueue)) { build in
                            Label("outside queue · \(build.command)", systemImage: "exclamationmark.triangle")
                                .font(.caption.monospaced()).foregroundStyle(.orange).lineLimit(2)
                        }
                    }
                }
                let planDone = monitor.plan?.phases.flatMap(\.slices).filter { monitor.landing($0.id) != nil }.count ?? 0
                let planCount = monitor.plan?.phases.flatMap(\.slices).count ?? 0
                section("Plan", caption: monitor.plan == nil ? nil : "\(planDone)/\(planCount)") {
                    if let plan = monitor.plan {
                        Text(plan.title).font(.subheadline.bold()).lineLimit(2)
                        Text("\(phaseName(plan)) · \(planDone)/\(planCount)")
                            .font(.caption).foregroundStyle(.secondary)
                        CapsuleBar(fraction: planCount == 0 ? 0 : Double(planDone) / Double(planCount),
                                   color: .accentColor, height: 6)
                        let currentPhase = plan.phases.first {
                            !$0.slices.allSatisfy { monitor.landing($0.id) != nil }
                        }?.id
                        ForEach(plan.phases) { phase in
                            let completed = phase.slices.allSatisfy { monitor.landing($0.id) != nil }
                            if completed {
                                Text("✓ \(phase.title) · \(phase.slices.count)/\(phase.slices.count)")
                                    .font(.subheadline).foregroundStyle(.secondary)
                            } else if phase.id == currentPhase {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(phase.title).font(.subheadline).bold()
                                    ForEach(phase.slices) { slice in sliceRow(slice) }
                                }
                            } else {
                                DisclosureGroup {
                                    ForEach(phase.slices) { slice in sliceRow(slice) }
                                } label: {
                                    HStack {
                                        Text(phase.title).font(.subheadline.bold())
                                        Spacer()
                                        Text("\(phase.slices.count) slices")
                                            .font(.caption).foregroundStyle(.secondary)
                                    }
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
                section("Agents", caption: "\(monitor.runningAgentCount) running · \(monitor.agents.count) total") {
                    if monitor.agents.isEmpty { dim("none running") }
                    ForEach(monitor.agents.indices, id: \.self) { index in
                        if index > 0 { Divider() }
                        AgentCard(job: monitor.agents[index], worktrees: monitor.worktrees,
                                  role: role(for: monitor.agents[index]))
                    }
                }
                section("Activity") {
                    let activity = monitor.activity
                    if activity.isEmpty { dim("No activity yet") }
                    ForEach(activity) { group in
                        HStack(alignment: .firstTextBaseline) {
                            if group.count == 1 {
                                Text(group.latest.at, style: .time)
                                    .monospacedDigit().foregroundStyle(.secondary)
                            } else {
                                Text(activityTimeRange(group))
                                    .monospacedDigit().foregroundStyle(.secondary).lineLimit(1)
                            }
                            Text(group.text).lineLimit(2)
                        }.font(.caption)
                    }
                }
                section("Infra", caption: "\(monitor.worktrees.count) worktrees") {
                    DisclosureGroup("Worktrees (\(monitor.worktrees.count))", isExpanded: $showWorktrees) {
                        ForEach(monitor.worktrees) { tree in
                            HStack {
                                Text(tree.branch).lineLimit(1)
                                Spacer()
                                if tree.missing { Text("offline") }
                                if let ahead = tree.ahead { Text("+\(ahead)") }
                                Text(tree.head).font(.caption.monospaced())
                            }.font(.caption).foregroundStyle(.secondary).help(tree.path)
                        }
                    }
                    HStack(spacing: 10) {
                        ForEach(monitor.services) { service in
                            HStack(spacing: 4) {
                                Circle().fill(service.up == nil ? .gray : service.up == true ? .green : .red)
                                    .frame(width: 7, height: 7)
                                Text("\(service.name) \(service.up == nil ? "…" : service.up == true ? "up" : "down")")
                                    .lineLimit(1).minimumScaleFactor(0.8)
                            }
                        }
                    }.font(.caption2).foregroundStyle(.secondary)
                }
                section("Budget") {
                    if !monitor.budgetLoaded { dim("Budget loading…") }
                    else if let budget = monitor.budget {
                        ForEach(budget.indices, id: \.self) { index in
                            budgetRow(budget[index])
                        }
                    } else { dim("Budget unavailable") }
                }
                Divider()
                footer
            }
            .padding(12)
            .background(GeometryReader { proxy in
                Color.clear.preference(key: PanelHeightKey.self, value: proxy.size.height)
            })
        }
        .frame(width: 420, height: min(max(contentHeight, 120), 640))
        .onPreferenceChange(PanelHeightKey.self) { contentHeight = $0 }
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
            HStack(alignment: .firstTextBaseline, spacing: 5) {
                Text(landed != nil ? "✓" : agent != nil ? "◐" : "○").font(.caption)
                Text(slice.id).font(.caption.monospaced().bold())
                Text(slice.title).font(.callout).lineLimit(2)
                Spacer(minLength: 4)
                if let landed { Text(landed.id).font(.caption2.monospaced()).foregroundStyle(.secondary) }
            }
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
            }
        }.font(.caption).buttonStyle(.bordered).controlSize(.small)
    }

    private func section<C: View>(_ title: String, caption: String? = nil,
                                  @ViewBuilder content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(title).font(.headline)
                Spacer()
                if let caption { Text(caption).font(.caption).foregroundStyle(.secondary).monospacedDigit() }
            }
            content()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 10)
            .fill(Color(nsColor: .quaternaryLabelColor).opacity(0.5)))
    }

    private func dim(_ text: String) -> some View { Text(text).foregroundStyle(.secondary).font(.caption) }

    private func panelGlyph() -> NSImage? {
        let name = monitor.alert ? "MenuGlyphAlert" : "MenuGlyph"
        guard let image = Bundle.main.image(forResource: name) else { return nil }
        image.isTemplate = true
        return image
    }

    private func activityTimeRange(_ group: ActivityGroup) -> String {
        let formatter = DateIntervalFormatter()
        formatter.dateStyle = .none
        formatter.timeStyle = .short
        return formatter.string(from: group.earliestAt, to: group.latest.at)
    }

    @ViewBuilder private func budgetRow(_ entry: BudgetEntry) -> some View {
        switch entry {
        case .window(let window):
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("\(window.provider.capitalized) · \(window.window)").font(.headline)
                    Spacer()
                    Text("\(window.percentLeft)% left").font(.headline).monospacedDigit()
                }
                if let reset = window.reset {
                    HStack {
                        Spacer()
                        Text("Resets in \(BudgetGuide.duration(reset) ?? reset)")
                            .font(.caption).foregroundStyle(.secondary).monospacedDigit()
                    }
                }
                CapsuleBar(fraction: Double(window.percentLeft) / 100,
                           color: window.provider == "codex" ? .teal : .orange,
                           height: 8,
                           expected: BudgetGuide.evenPaceLeft(window: window.window, reset: window.reset))
                if let pace = window.pace {
                    let label = pace.kind == .deficit ? "in deficit" : "in reserve"
                    Text("\(abs(pace.percent))% \(label)" +
                         (pace.runsOut.map { " · runs out in \(BudgetGuide.duration($0) ?? $0)" } ?? ""))
                        .font(.caption)
                        .foregroundStyle(pace.kind == .deficit ? Color.red : Color.green)
                }
            }.padding(.vertical, 3)
        case .credits(let provider, let count):
            Text("\(provider.capitalized) · \(count) credit\(count == 1 ? "" : "s") held by owner")
                .font(.caption).foregroundStyle(.secondary)
        case .raw(let line):
            Text(line).font(.caption.monospaced()).fixedSize(horizontal: false, vertical: true)
        }
    }

    private func open(_ url: URL) { NSWorkspace.shared.open(url) }
}

private struct CapsuleBar: View {
    let fraction: Double
    let color: Color
    let height: CGFloat
    var expected: Double? = nil

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.1))
                Capsule().fill(color)
                    .frame(width: proxy.size.width * min(1, max(0, fraction)))
                if let expected {
                    Rectangle().fill(Color.primary.opacity(0.8))
                        .frame(width: 1, height: height)
                        .offset(x: min(proxy.size.width - 1, proxy.size.width * expected))
                }
            }
        }.frame(height: height)
    }
}

struct AgentCard: View {
    let job: AgentJob
    let worktrees: [Worktree]
    let role: String?

    var body: some View {
        let details = [job.subagent ? "subagent" : nil, role,
                       job.tokens.map { "\($0) tokens" }].compactMap { $0 }
        return Button {
            if NSApp.currentEvent?.modifierFlags.contains(.option) == true, let transcript = job.transcript {
                NSWorkspace.shared.open(transcript)
            } else if let tree = worktrees.first(where: { $0.branch == job.branch }) {
                openTerminal(tree.path)
            }
        } label: {
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Image(systemName: job.kind == "codex" ? "chevron.left.forwardslash.chevron.right" : "sparkle")
                    Text(job.model ?? job.kind).font(.callout.bold()).lineLimit(1)
                    Text(job.branch ?? "unmanaged")
                        .font(.caption2).lineLimit(1).truncationMode(.middle)
                        .frame(maxWidth: 128)
                        .padding(.horizontal, 6).padding(.vertical, 1)
                        .background(Capsule().fill(.quaternary))
                        .fixedSize()
                    Spacer()
                    if let time = job.lastActive {
                        Text(time, style: .relative).font(.caption)
                            .monospacedDigit().foregroundStyle(.secondary)
                    }
                }
                Text("\(job.idle ? "idle" : "running") · \(job.step ?? "no transcript step")")
                    .font(.caption).lineLimit(2).foregroundStyle(.secondary)
                if !details.isEmpty {
                    Text(details.joined(separator: " · "))
                        .font(.caption2).foregroundStyle(.tertiary)
                }
            }.frame(maxWidth: .infinity, alignment: .leading)
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
    @State private var research: String = ""
    @State private var login = SMAppService.mainApp.status == .enabled
    private let kinds = ["agent started", "agent finished", "agent failed", "slice landed",
                         "land failed", "review", "blocker raised", "blocker resolved",
                         "plan or phase change", "build failed", "service down", "service back up"]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("SentinelBar settings").font(.headline)
            TextField("Repository", text: $repo)
            TextField("Research directory (blank: sibling Sentinel-Private/research)", text: $research)
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
                    monitor.setResearchPath(research)
                    dismiss()
                }
            }
        }
        .padding(16).frame(width: 380)
        .onAppear { repo = monitor.repo; pinned = monitor.pinnedPlan; research = monitor.researchPath }
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

private struct PanelHeightKey: PreferenceKey {
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}
