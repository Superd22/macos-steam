import SwiftUI

/// The two panes. Neither of them appears on the happy path: the checklist only
/// when preflight is broken or nothing has been proven yet, and the settings
/// pane only when asked for (option-click, or the nested helper app).
struct RootView: View {
    enum Pane { case checklist, settings }
    let pane: Pane
    @StateObject var model = AppModel()

    var body: some View {
        Group {
            switch pane {
            case .checklist: ChecklistView(model: model)
            case .settings: SettingsPane(model: model)
            }
        }
        .frame(minWidth: 560, minHeight: 460)
    }
}

// MARK: - shared

struct VerdictIcon: View {
    let verdict: Verdict
    var body: some View {
        switch verdict {
        case .ok: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .blocked: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
        case .warning: Image(systemName: "info.circle.fill").foregroundStyle(.yellow)
        case .pending: Image(systemName: "circle.dashed").foregroundStyle(.secondary)
        }
    }
}

struct Row<Trailing: View>: View {
    let verdict: Verdict
    let title: String
    let detail: String
    @ViewBuilder var trailing: () -> Trailing

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            VerdictIcon(verdict: verdict)
            VStack(alignment: .leading, spacing: 3) {
                Text(title).fontWeight(verdict == .ok ? .regular : .medium)
                if !detail.isEmpty {
                    Text(detail).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 8)
            trailing()
        }
        .padding(.vertical, 4)
    }
}

// MARK: - checklist (first run, or preflight broken)

struct ChecklistView: View {
    @ObservedObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Steam (macOS Play)").font(.title2).bold()
                Text(headline).foregroundStyle(.secondary)
            }
            .padding(20)

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(model.checks) { check in
                        Row(verdict: displayed(check).verdict,
                            title: displayed(check).title,
                            detail: displayed(check).detail) {
                            remedyButton(for: check)
                        }
                        Divider().opacity(0.4)
                    }
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 8)
            }

            Divider()

            HStack {
                if let busy = model.busy {
                    ProgressView().controlSize(.small)
                    Text(busy).foregroundStyle(.secondary)
                } else {
                    Text("Hold ⌥ while opening this app for settings and diagnostics.")
                        .font(.callout).foregroundStyle(.secondary)
                }
                Spacer()
                Button("Re-check") { model.refresh() }
                Button(launchTitle) { model.launchAndProve() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(model.launchState == .launching)
            }
            .padding(16)
        }
        .onAppear { model.refresh() }
    }

    private var headline: String {
        switch model.launchState {
        case .ready(let sites):
            return "Ready — the compat gate was flipped (\(sites) site\(sites == 1 ? "" : "s")). You can close this window; it will not appear again."
        case .launching:
            return "Steam is starting. Watching for the compat gate…"
        case .launchedButUnproven:
            return "Steam started, but this app could not confirm the gate."
        case .idle:
            return Preflight.isClear(model.checks)
                ? "Everything checks out. Start Steam once from here and this window will confirm it worked."
                : "Something needs doing before Windows titles will work."
        }
    }

    private var launchTitle: String {
        switch model.launchState {
        case .launching: return "Starting…"
        // The gate is flipped per process, so a client that is already up
        // cannot be given one — it has to be replaced, and the button says so
        // rather than quietly quitting the user's Steam.
        default: return model.mustQuitSteamFirst ? "Quit Steam and start it here" : "Start Steam"
        }
    }

    /// The self-verification row is the one the launch drives live, so it shows
    /// the launch state rather than the (still pending) preflight verdict.
    private func displayed(_ check: Check) -> Check {
        guard check.id == "verified" else { return check }
        switch model.launchState {
        case .launching:
            return Check(id: check.id, title: "Watching for the compat gate…", verdict: .pending, detail: "")
        case .ready(let sites):
            return Check(id: check.id, title: "Compat gate flipped — patched \(sites) site\(sites == 1 ? "" : "s")",
                         verdict: .ok, detail: "")
        case .launchedButUnproven(let why):
            return Check(id: check.id, title: "Compat gate not confirmed", verdict: .blocked, detail: why)
        case .idle:
            return check
        }
    }

    @ViewBuilder
    private func remedyButton(for check: Check) -> some View {
        switch check.remedy {
        case .openURL(let url):
            Link(check.remedyTitle, destination: url).buttonStyle(.bordered)
        case .createBottle:
            Button(check.remedyTitle) { model.createBottle() }.disabled(model.busy != nil)
        case .reinstall:
            Button(check.remedyTitle) { Help.showReinstall() }
        case .none:
            EmptyView()
        }
    }
}

// MARK: - settings (⌥-click, or the nested helper)

struct SettingsPane: View {
    @ObservedObject var model: AppModel
    @State private var tab = 0

    var body: some View {
        TabView(selection: $tab) {
            OverlayTab(model: model).tabItem { Text("Settings") }.tag(0)
            DiagnoseTab(model: model).tabItem { Text("Diagnose") }.tag(1)
            UninstallTab(model: model).tabItem { Text("Uninstall") }.tag(2)
        }
        .padding(16)
    }
}

struct OverlayTab: View {
    @ObservedObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Toggle("Steam overlay in Windows games", isOn: Binding(
                get: { model.overlay },
                set: { model.setOverlay($0) }))
            .toggleStyle(.switch)

            Text("Valve's own overlay renderer, loaded into the game process. Applies to the whole Steam session, not to one title — the setting is read when Steam starts.")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)

            if model.overlayPendingRestart {
                HStack {
                    Text("Takes effect the next time Steam starts.")
                    Spacer()
                    Button("Restart Steam") { model.restartSteam() }.disabled(model.busy != nil)
                }
                .padding(10)
                .background(.quaternary, in: RoundedRectangle(cornerRadius: 8))
            }

            Divider()

            if let r = model.receipt {
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 4) {
                    GridRow { Text("Version").foregroundStyle(.secondary); Text(r.version) }
                    GridRow { Text("Deployed").foregroundStyle(.secondary); Text(r.deployedAt) }
                    GridRow { Text("Tested on").foregroundStyle(.secondary)
                              Text("macOS \(r.tested.macOS), CrossOver \(r.tested.crossover)") }
                    GridRow { Text("This Mac").foregroundStyle(.secondary)
                              Text("macOS \(r.observed.macOS), CrossOver \(r.observed.crossover)") }
                }
                .font(.callout)
            } else {
                Text("Nothing is deployed — no receipt found.").foregroundStyle(.secondary)
            }
            Spacer()
        }
    }
}

struct DiagnoseTab: View {
    @ObservedObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Button("Run checks") { model.runDiagnose() }.disabled(model.busy != nil)
                if let busy = model.busy {
                    ProgressView().controlSize(.small); Text(busy).foregroundStyle(.secondary)
                }
                Spacer()
                Button("Copy report") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(Diagnose.report(), forType: .string)
                }
                .disabled(model.findings.isEmpty)
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(model.findings) { f in
                        Row(verdict: f.verdict, title: f.title, detail: f.detail) { EmptyView() }
                        Divider().opacity(0.4)
                    }
                    if model.findings.isEmpty && model.busy == nil {
                        Text("Nothing checked yet.").foregroundStyle(.secondary).padding(.top, 8)
                    }
                }
            }
        }
    }
}

struct UninstallTab: View {
    @ObservedObject var model: AppModel
    @State private var deleteBottle = false
    @State private var confirming = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Removes this launcher, the payload and the logs.")
            Text("Valve's Steam was never modified — nothing there has to be undone, and your games and library are untouched.")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Toggle("Also delete the CrossOver bottle “\(Preflight.bottleName)”", isOn: $deleteBottle)
            Text("The bottle may hold save games written by Windows titles, and it may not have been created by this app.")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            HStack {
                Spacer()
                Button("Uninstall…") { confirming = true }.disabled(model.busy != nil)
            }
            Spacer()
        }
        .alert("Remove Steam (macOS Play)?", isPresented: $confirming) {
            Button("Cancel", role: .cancel) {}
            Button("Remove", role: .destructive) { model.uninstall(deleteBottle: deleteBottle) }
        } message: {
            Text(deleteBottle
                 ? "The launcher, the payload, the logs and the bottle will be deleted."
                 : "The launcher, the payload and the logs will be deleted.")
        }
    }
}

enum Help {
    /// A user who has to reinstall has, by definition, a broken install — so
    /// this says the one command rather than opening something that may not be
    /// there.
    static func showReinstall() {
        let alert = NSAlert()
        alert.messageText = "Reinstall the payload"
        alert.informativeText = """
            From a clone of the repository:

                ./src/installer/install.sh

            That rebuilds anything stale, lays the payload down again and rewrites this app.
            """
        alert.runModal()
    }
}
