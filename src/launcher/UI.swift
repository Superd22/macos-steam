import SwiftUI

/// The two panes, and the small design system they share.
///
/// Neither pane appears on the happy path: the checklist only when preflight is
/// broken or nothing has been proven yet, the settings pane only when asked for
/// (⌥ at launch, or the nested helper app).
///
/// The window is tall rather than square because both panes are a header and
/// then a column of rows, which is a shape a phone-sized window fits and a
/// square one pads out. Everything below uses semantic colours only — `.primary`,
/// `.secondary`, the materials, the user's accent — so light and dark are the
/// same code and follow the system rather than a preference of ours.
enum Metrics {
    static let width: CGFloat = 420
    static let height: CGFloat = 580
    /// The traffic lights float over the content, since the titlebar is
    /// transparent. This is the room they need.
    static let titlebar: CGFloat = 28
    static let gutter: CGFloat = 22
    static let radius: CGFloat = 12
}

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
        .frame(width: Metrics.width, height: Metrics.height)
        .background(Color(nsColor: .windowBackgroundColor))
    }
}

// MARK: - shared pieces

extension Verdict {
    var symbol: String {
        switch self {
        case .ok: "checkmark"
        case .blocked: "exclamationmark"
        case .warning: "exclamationmark"
        case .pending: "circle"
        }
    }

    var tint: Color {
        switch self {
        case .ok: .green
        case .blocked: .orange
        case .warning: .yellow
        case .pending: .secondary
        }
    }
}

/// A verdict as a small filled disc. Colour carries the state and the glyph
/// repeats it, so it still reads without colour vision.
struct Pip: View {
    let verdict: Verdict
    var size: CGFloat = 18

    var body: some View {
        ZStack {
            Circle().fill(verdict.tint.opacity(verdict == .pending ? 0.12 : 0.16))
            if verdict == .pending {
                Circle().strokeBorder(.secondary.opacity(0.35), lineWidth: 1.2).padding(size * 0.28)
            } else {
                Image(systemName: verdict.symbol)
                    .font(.system(size: size * 0.5, weight: .bold))
                    .foregroundStyle(verdict.tint)
            }
        }
        .frame(width: size, height: size)
    }
}

/// The one big element on the checklist: what state the whole thing is in.
/// An empty ring for "unknown" reads as unfinished, so the resting state is the
/// app's own glyph in the user's accent colour, and only a real verdict
/// recolours it.
struct HeaderBadge: View {
    let verdict: Verdict
    let working: Bool

    private var tint: Color { verdict == .pending ? .accentColor : verdict.tint }
    private var glyph: String {
        switch verdict {
        case .ok: "checkmark"
        case .blocked, .warning: "exclamationmark"
        case .pending: "gamecontroller.fill"
        }
    }

    var body: some View {
        ZStack {
            Circle().fill(tint.opacity(0.14))
            if working {
                ProgressView().controlSize(.small)
            } else {
                Image(systemName: glyph)
                    .font(.system(size: verdict == .pending ? 23 : 26, weight: .semibold))
                    .foregroundStyle(tint)
            }
        }
        .frame(width: 58, height: 58)
    }
}

/// One line in either pane's list. The title carries the state, so the detail
/// line is absent whenever there is nothing to add.
struct StatusRow<Trailing: View>: View {
    let verdict: Verdict
    let title: String
    let detail: String
    @ViewBuilder var trailing: () -> Trailing

    var body: some View {
        HStack(alignment: .top, spacing: 11) {
            Pip(verdict: verdict).padding(.top, 1)
            VStack(alignment: .leading, spacing: 3) {
                Text(title)
                    .font(.subheadline)
                    .fontWeight(verdict == .ok ? .regular : .medium)
                    .foregroundStyle(.primary)
                if !detail.isEmpty {
                    Text(detail)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 6)
            trailing()
                .controlSize(.small)
                .buttonStyle(.bordered)
        }
        .padding(.vertical, 11)
        .padding(.horizontal, 14)
    }
}

/// Rows grouped into one rounded surface, hairlines between them. The card is
/// the only chrome either pane has.
struct Card<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        VStack(spacing: 0) { content() }
            .background(Color(nsColor: .controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: Metrics.radius, style: .continuous))
            // The separator colour and not a fixed opacity: in dark mode the
            // card fill sits close to the window fill, and a hand-picked alpha
            // that reads in light disappears there.
            .overlay(
                RoundedRectangle(cornerRadius: Metrics.radius, style: .continuous)
                    .strokeBorder(Color(nsColor: .separatorColor), lineWidth: 1))
    }
}

struct Hairline: View {
    var body: some View { Divider().opacity(0.5).padding(.leading, 44) }
}

/// A row's remedy, wherever the row is shown. Shared between the two panes
/// because a remedy is a property of the check, not of the pane: the DRM
/// download appears on the checklist and in Diagnose, and one button drawn
/// twice is how the two would end up disagreeing about what it does (#105).
struct RemedyButton: View {
    let remedy: Remedy
    let title: String
    @ObservedObject var model: AppModel

    var body: some View {
        switch remedy {
        case .openURL(let url): Link(title, destination: url)
        case .openApp(let path):
            Button(title) { NSWorkspace.shared.open(URL(fileURLWithPath: path)) }
        case .createBottle: Button(title) { model.createBottle() }.disabled(model.busy != nil)
        case .fetchValveClient:
            Button(title) { Task { await model.fetchValveClient() } }.disabled(model.busy != nil)
        case .reinstall: Button(title) { Help.showReinstall() }
        case .none: EmptyView()
        }
    }
}

/// What a row says, once the model knows better than the row does. Two rows
/// have anything to add, and for one reason: their checks are stat calls, so
/// after a remedy that did not work the file is still missing and the row goes
/// back to explaining why it is needed — while what the user needs to read is
/// why it did not arrive (the DRM download) or why it did not finish (the
/// bottle, #108).
@MainActor private func rowDetail(_ id: String, _ detail: String, _ model: AppModel) -> String {
    switch id {
    case ValveClient.rowID: return model.valveClientProblem ?? detail
    case Preflight.bottleRowID: return model.bottleProblem ?? detail
    default: return detail
    }
}

// MARK: - checklist (first run, or preflight broken)

struct ChecklistView: View {
    @ObservedObject var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            header
            ScrollView {
                Card {
                    ForEach(Array(model.checks.enumerated()), id: \.element.id) { index, check in
                        let shown = displayed(check)
                        StatusRow(verdict: shown.verdict, title: shown.title,
                                  detail: rowDetail(shown.id, shown.detail, model)) {
                            RemedyButton(remedy: check.remedy, title: check.remedyTitle, model: model)
                        }
                        if index < model.checks.count - 1 { Hairline() }
                    }
                }
                .padding(.horizontal, Metrics.gutter)
                .padding(.bottom, 18)
            }
            footer
        }
        .onAppear { model.refreshSoon() }
    }

    /// The state of the whole thing, once, in the largest type on screen. A
    /// user who reads nothing else should still know where they stand.
    private var header: some View {
        VStack(spacing: 12) {
            HeaderBadge(verdict: overall, working: model.launchState == .launching)
            VStack(spacing: 5) {
                Text("Steam (macOS Play)")
                    .font(.title3.weight(.semibold))
                Text(headline)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.top, Metrics.titlebar + 14)
        .padding(.horizontal, Metrics.gutter + 8)
        .padding(.bottom, 22)
    }

    private var footer: some View {
        VStack(spacing: 10) {
            Button(action: { model.launchAndProve() }) {
                Text(launchTitle).frame(maxWidth: .infinity)
            }
            .controlSize(.large)
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
            .disabled(model.launchState == .launching || model.busy != nil)

            HStack(spacing: 6) {
                if let busy = model.busy {
                    ProgressView().controlSize(.small)
                    Text(busy)
                } else {
                    Text("Hold ⌥ when you open this app for settings.")
                }
                Spacer()
                Button("Re-check") { model.recheck() }
                    .controlSize(.small)
                    .disabled(model.busy != nil)
            }
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        .padding(.horizontal, Metrics.gutter)
        .padding(.vertical, 16)
        .background(.bar)
    }

    /// A blocked check outranks a good launch: Steam Play can be switched on
    /// while the bottle is still missing, and "All set" over an orange row is
    /// the header contradicting the list under it.
    private var overall: Verdict {
        if model.launchState == .launching { return .pending }
        if !Preflight.isClear(model.checks) { return .blocked }
        switch model.launchState {
        case .ready: return .ok
        case .launchedButUnproven: return .blocked
        default: return .pending
        }
    }

    private var headline: String {
        if model.launchState != .launching && !Preflight.isClear(model.checks) {
            return "A couple of things need sorting before Windows games will work."
        }
        switch model.launchState {
        case .ready:
            return "All set. You can close this window. It will not come back."
        case .launching:
            return "Starting Steam…"
        case .launchedButUnproven:
            return "Steam started, but Steam Play did not switch on."
        case .idle:
            return Preflight.isClear(model.checks)
                ? "Looks good. Start Steam once from here and this window will confirm it."
                : "A couple of things need sorting before Windows games will work."
        }
    }

    private var launchTitle: String {
        switch model.launchState {
        case .launching: return "Starting…"
        default: return model.mustQuitSteamFirst ? "Quit Steam and start it here" : "Start Steam"
        }
    }

    /// The self-verification row is the one the launch drives live, so it shows
    /// the launch state rather than the (still pending) preflight verdict.
    private func displayed(_ check: Check) -> Check {
        guard check.id == "verified" else { return check }
        switch model.launchState {
        case .launching:
            return Check(id: check.id, title: "Checking…", verdict: .pending, detail: "")
        case .ready:
            return Check(id: check.id, title: "Steam Play is switched on", verdict: .ok, detail: "")
        case .launchedButUnproven(let why):
            return Check(id: check.id, title: "Steam Play did not switch on", verdict: .blocked, detail: why)
        case .idle:
            return check
        }
    }
}

// MARK: - settings (⌥-click, or the nested helper)

struct SettingsPane: View {
    @ObservedObject var model: AppModel
    @State private var tab = Tab.settings

    enum Tab: String, CaseIterable, Identifiable {
        case settings = "Settings", diagnose = "Diagnose", uninstall = "Uninstall"
        var id: String { rawValue }
    }

    var body: some View {
        VStack(spacing: 0) {
            TabBar(selection: $tab)
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    switch tab {
                    case .settings: OverlayTab(model: model)
                    case .diagnose: DiagnoseTab(model: model)
                    case .uninstall: UninstallTab(model: model)
                    }
                }
                .padding(.horizontal, Metrics.gutter)
                .padding(.top, 18)
                .padding(.bottom, 22)
            }
        }
    }
}

/// The three panes are the header. Full bleed to both edges, sitting directly
/// under the traffic lights, with nothing above it and no window title
/// competing — there is nothing else at this level to name.
///
/// Hand-built rather than a segmented Picker: an inset pill floats inside a
/// header, and what was asked for is a header that IS the control. Each tab
/// takes exactly a third of the width and marks itself with the accent colour.
struct TabBar: View {
    @Binding var selection: SettingsPane.Tab

    var body: some View {
        VStack(spacing: 0) {
            Color.clear.frame(height: Metrics.titlebar)   // room for the traffic lights
            HStack(spacing: 0) {
                ForEach(SettingsPane.Tab.allCases) { tab in
                    Button {
                        selection = tab
                    } label: {
                        VStack(spacing: 7) {
                            Text(tab.rawValue)
                                .font(.subheadline)
                                .fontWeight(selection == tab ? .semibold : .regular)
                                .foregroundStyle(selection == tab ? Color.accentColor : .secondary)
                            Rectangle()
                                .fill(selection == tab ? Color.accentColor : .clear)
                                .frame(height: 2)
                        }
                        .padding(.top, 9)
                        .frame(maxWidth: .infinity)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .background(.bar)
        .overlay(alignment: .bottom) { Divider() }
    }
}

/// A heading for a group of rows. Small, quiet, and never repeated by the first
/// line of the group under it.
struct SectionLabel: View {
    let text: String
    var body: some View {
        Text(text.uppercased())
            .font(.caption2.weight(.semibold))
            .tracking(0.6)
            .foregroundStyle(.secondary)
            .padding(.leading, 2)
    }
}

struct OverlayTab: View {
    @ObservedObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Card {
                VStack(alignment: .leading, spacing: 8) {
                    Toggle("Steam overlay in Windows games", isOn: Binding(
                        get: { model.overlay },
                        set: { model.setOverlay($0) }))
                    .toggleStyle(.switch)
                    .font(.subheadline)

                    Text("The Shift+Tab overlay, in Windows games. It applies to every game you play, and changes take effect the next time Steam starts.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(14)

                if model.overlayPendingRestart {
                    Hairline().padding(.leading, 0)
                    HStack {
                        Text("Takes effect the next time Steam starts.")
                            .font(.caption)
                        Spacer()
                        Button("Restart Steam") { model.restartSteam() }
                            .controlSize(.small)
                            .disabled(model.busy != nil)
                    }
                    .padding(14)
                }
            }

            if let r = model.receipt {
                SectionLabel(text: "About")
                Card {
                    InfoRow(label: "Version", value: r.version)
                    Hairline().padding(.leading, 14)
                    InfoRow(label: "Installed", value: String(r.deployedAt.prefix(10)))
                    Hairline().padding(.leading, 14)
                    InfoRow(label: "Tested with", value: "macOS \(r.tested.macOS), CrossOver \(r.tested.crossover)")
                    Hairline().padding(.leading, 14)
                    InfoRow(label: "You have", value: "macOS \(r.observed.macOS), CrossOver \(r.observed.crossover)")
                }
            } else {
                Text("Nothing is installed yet.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

struct InfoRow: View {
    let label: String
    let value: String
    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).foregroundStyle(.secondary)
            Spacer(minLength: 12)
            Text(value)
                .multilineTextAlignment(.trailing)
                .fixedSize(horizontal: false, vertical: true)
        }
        .font(.caption)
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }
}

struct DiagnoseTab: View {
    @ObservedObject var model: AppModel

    var body: some View {
        content.onAppear {
            // Entering the tab asks the question; nobody opens Diagnose to look
            // at an empty pane and a button. Deferred, because starting the work
            // inside the update that is drawing this tab is what SwiftUI aborts on.
            model.diagnoseSoon()
        }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 8) {
                // The checks are why someone opened this tab, so they are
                // already running by the time the tab is drawn. The button is
                // for the second look, after changing something.
                Button("Re-run") { model.runDiagnose() }
                    .controlSize(.small)
                    .disabled(model.busy != nil)
                if model.busy != nil { ProgressView().controlSize(.small) }
                Spacer()
                Button("Copy report") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(Diagnose.report(), forType: .string)
                }
                .buttonStyle(.link)
                .font(.caption)
                .disabled(model.findings.isEmpty)
            }

            if model.findings.isEmpty {
                Text(model.busy == nil ? "Nothing to report." : "Checking…")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                Card {
                    ForEach(Array(model.findings.enumerated()), id: \.element.id) { index, f in
                        StatusRow(verdict: f.verdict, title: f.title,
                                  detail: rowDetail(f.id, f.detail, model)) {
                            RemedyButton(remedy: f.remedy, title: f.remedyTitle, model: model)
                        }
                        if index < model.findings.count - 1 { Hairline() }
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
        VStack(alignment: .leading, spacing: 18) {
            Card {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Removes this app and the files it installed.")
                        .font(.subheadline)
                    Text("Your Steam was never changed, so there is nothing to undo there. Your games and library stay where they are.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(14)

                Hairline().padding(.leading, 0)

                VStack(alignment: .leading, spacing: 8) {
                    Toggle("Also delete the bottle “\(Preflight.bottleName)”", isOn: $deleteBottle)
                        .font(.subheadline)
                    Text("It may hold save games from Windows games, and it may not have been made by this app.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(14)
            }

            Button(role: .destructive, action: { confirming = true }) {
                Text("Uninstall…").frame(maxWidth: .infinity)
            }
            .controlSize(.large)
            .buttonStyle(.bordered)
            .disabled(model.busy != nil)
        }
        .alert("Remove Steam (macOS Play)?", isPresented: $confirming) {
            Button("Cancel", role: .cancel) {}
            Button("Remove", role: .destructive) { model.uninstall(deleteBottle: deleteBottle) }
        } message: {
            Text(deleteBottle
                 ? "The app, its files and the bottle will be deleted."
                 : "The app and its files will be deleted.")
        }
    }
}

enum Help {
    /// Someone who has to reinstall has, by definition, a broken install — so
    /// this says the one command rather than opening something that may not be
    /// there.
    static func showReinstall() {
        let alert = NSAlert()
        alert.messageText = "Reinstalling"
        alert.informativeText = """
            In a Terminal, from your copy of the project:

                ./src/installer/install.sh

            That puts the files back and updates this app.
            """
        alert.runModal()
    }
}
